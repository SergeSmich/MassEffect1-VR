// _CRT_SECURE_NO_WARNINGS for _wfopen/sscanf is set project-wide in the vcxproj PreprocessorDefinitions.

#include "vr_config.h"

#include "logger.h"
#include "pchud.h"   // k*ElemNames: per-element override key names come from the SWF-dump tables

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace MELEVR::Config
{
namespace
{
// Resolve MELEVR.ini next to THIS dll (the game folder), so the mod is portable - not a hard-coded user
// path. GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS with an address inside this module hands back this module's own
// HMODULE even though it's a renamed proxy (dxgi.dll).
std::wstring IniPath() noexcept
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&IniPath), &self);
    wchar_t path[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path, n);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);  // strip the dll filename, keep the trailing slash
    p += L"MELEVR.ini";
    return p;
}

// ---- Profile slots --------------------------------------------------------------------------------
// Fixed 4 slots. g_slots holds each slot's last-known values (defaults until loaded from disk). Get()
// returns g_cfg, the LIVE config the menu + xr_session bind to; its address must stay stable, so it's
// never reseated - Load* copies a slot's values INTO it, Save* copies it back OUT into a slot.
constexpr int kProfileCount = 4;
const char* const kProfileNames[kProfileCount] = { "Default", "Custom 1", "Custom 2", "Custom 3" };

VrConfig g_slots[kProfileCount];   // per-slot values (member initializers = defaults)
int      g_active = 0;             // index of the slot Get() currently reflects

// Profile hotkeys (GLOBAL, one VK per slot; 0 = unmapped). Persisted in the ini's [hotkeys] section,
// NOT inside any profile slot, so the map stays put when you switch profiles. See ProfileHotkey* API.
// DEFAULT map = F1/F2/F3/F4 -> slots 0..3. CHANGED 2026-08-11 from the original 1/2/3/4 binds
// (2026-07-20): 1-4 are Mass Effect's OWN weapon keys, so a keypress in combat live-loaded a whole
// settings profile - silently swapping resolution, separation and HUD layout mid-fight. That is the
// most likely reason a tuned sfr2ResY drifted back to a stale value on a working install. F1-F4 are
// unbound in ME1 and unused by the mod (Insert/R/K/P), and the map is still rebindable in the
// Profiles tab. VK_F1=0x70..VK_F4=0x73.
const int kDefaultProfileHotkey[kProfileCount] = {VK_F1, VK_F2, VK_F3, VK_F4};
int  g_profileHotkey[kProfileCount] = {VK_F1, VK_F2, VK_F3, VK_F4};
bool g_profileHotkeysEnabled = true;

// Per-element override keys: "hudElem_<Name>_<field>" / "convoElem_<Name>_<field>" with field one of
// on/hide/offX/offY/scaleX/scaleY. Names match MELEVR::Pchud::k*ElemNames (from the SWF dumps).
bool ApplyElemKey(VrConfig& c, const char* key, float v, bool b) noexcept
{
    auto tryList = [&](const char* prefix, const char* const* names, int n,
                       VrConfig::ElemOverride* elems) noexcept -> bool
    {
        const size_t plen = strlen(prefix);
        if (strncmp(key, prefix, plen) != 0) return false;
        for (int i = 0; i < n; ++i)
        {
            const size_t nlen = strlen(names[i]);
            if (strncmp(key + plen, names[i], nlen) != 0 || key[plen + nlen] != '_') continue;
            const char* field = key + plen + nlen + 1;
            if      (strcmp(field, "on") == 0)     elems[i].on = b;
            else if (strcmp(field, "hide") == 0)   elems[i].hide = b;
            else if (strcmp(field, "offX") == 0)   elems[i].offX = v;
            else if (strcmp(field, "offY") == 0)   elems[i].offY = v;
            else if (strcmp(field, "scaleX") == 0) elems[i].scaleX = v;
            else if (strcmp(field, "scaleY") == 0) elems[i].scaleY = v;
            else return false;
            return true;
        }
        return false;
    };
    auto tryGroup = [&](const char* prefix, VrConfig::ElemOverride& e) noexcept -> bool
    {
        const size_t plen = strlen(prefix);
        if (strncmp(key, prefix, plen) != 0) return false;
        const char* field = key + plen;
        if      (strcmp(field, "on") == 0)     e.on = b;
        else if (strcmp(field, "hide") == 0)   e.hide = b;
        else if (strcmp(field, "offX") == 0)   e.offX = v;
        else if (strcmp(field, "offY") == 0)   e.offY = v;
        else if (strcmp(field, "scaleX") == 0) e.scaleX = v;
        else if (strcmp(field, "scaleY") == 0) e.scaleY = v;
        else return false;
        return true;
    };
    return tryGroup("hudGroupTop_", c.hudGroupTop) ||
           tryGroup("hudGroupBottom_", c.hudGroupBottom) ||
           tryList("hudElemStereo_", MELEVR::Pchud::kHudElemNames, VrConfig::kHudElemCount, c.hudElemsStereo) ||
           tryList("hudElemDibr_", MELEVR::Pchud::kHudElemNames, VrConfig::kHudElemCount, c.hudElemsDibr) ||
           tryList("hudElem_", MELEVR::Pchud::kHudElemNames, VrConfig::kHudElemCount, c.hudElems) ||
           tryList("convoElem_", MELEVR::Pchud::kConvoElemNames, VrConfig::kConvoElemCount, c.convoElems);
}

// Apply one parsed "key = value" pair to a config. Tolerant: unknown keys ignored.
// Per-mode (Stereo/DIBR) scalar layout keys, split out of ApplyKeyValue so its else-if chain stays under
// MSVC's block-nesting limit (C1061). Returns true if handled.
bool ApplyPerModeKey(VrConfig& c, const char* key, float v) noexcept
{
    if (strcmp(key, "pchudScaleXStereo") == 0) { c.pchudScaleXStereo = v; return true; }
    if (strcmp(key, "pchudScaleYStereo") == 0) { c.pchudScaleYStereo = v; return true; }
    if (strcmp(key, "pchudOffsetXStereo") == 0) { c.pchudOffsetXStereo = v; return true; }
    if (strcmp(key, "pchudOffsetYStereo") == 0) { c.pchudOffsetYStereo = v; return true; }
    if (strcmp(key, "pchudScaleXDibr") == 0) { c.pchudScaleXDibr = v; return true; }
    if (strcmp(key, "pchudScaleYDibr") == 0) { c.pchudScaleYDibr = v; return true; }
    if (strcmp(key, "pchudOffsetXDibr") == 0) { c.pchudOffsetXDibr = v; return true; }
    if (strcmp(key, "pchudOffsetYDibr") == 0) { c.pchudOffsetYDibr = v; return true; }
    if (strcmp(key, "convoScaleXStereo") == 0) { c.convoScaleXStereo = v; return true; }
    if (strcmp(key, "convoScaleYStereo") == 0) { c.convoScaleYStereo = v; return true; }
    if (strcmp(key, "convoOffsetXStereo") == 0) { c.convoOffsetXStereo = v; return true; }
    if (strcmp(key, "convoOffsetYStereo") == 0) { c.convoOffsetYStereo = v; return true; }
    if (strcmp(key, "convoScaleXDibr") == 0) { c.convoScaleXDibr = v; return true; }
    if (strcmp(key, "convoScaleYDibr") == 0) { c.convoScaleYDibr = v; return true; }
    if (strcmp(key, "convoOffsetXDibr") == 0) { c.convoOffsetXDibr = v; return true; }
    if (strcmp(key, "convoOffsetYDibr") == 0) { c.convoOffsetYDibr = v; return true; }
    if (strcmp(key, "subtitlePosXFracStereo") == 0) { c.subtitlePosXFracStereo = v; return true; }
    if (strcmp(key, "subtitlePosYFracStereo") == 0) { c.subtitlePosYFracStereo = v; return true; }
    if (strcmp(key, "subtitleScaleXStereo") == 0) { c.subtitleScaleXStereo = v; return true; }
    if (strcmp(key, "subtitleScaleYStereo") == 0) { c.subtitleScaleYStereo = v; return true; }
    if (strcmp(key, "subtitlePosXFracDibr") == 0) { c.subtitlePosXFracDibr = v; return true; }
    if (strcmp(key, "subtitlePosYFracDibr") == 0) { c.subtitlePosYFracDibr = v; return true; }
    if (strcmp(key, "subtitleScaleXDibr") == 0) { c.subtitleScaleXDibr = v; return true; }
    if (strcmp(key, "subtitleScaleYDibr") == 0) { c.subtitleScaleYDibr = v; return true; }
    // First-person keys (fpEnabled + fpExplore_on/_fwd/_right/_up etc.). Longer prefixes FIRST --
    // "fpExplore" is a prefix of "fpExploreStorm".
    if (strcmp(key, "fpEnabled") == 0) { c.fpEnabled = (v != 0.0f); return true; }
    if (strcmp(key, "fpToggleKey") == 0) { c.fpToggleKey = static_cast<int>(v); return true; }
    if (strcmp(key, "depthPopKey") == 0) { c.depthPopKey = static_cast<int>(v); return true; }
    if (strcmp(key, "menuKey") == 0) { c.menuKey = static_cast<int>(v); return true; }
    if (strcmp(key, "recenterKeyEnabled") == 0) { c.recenterKeyEnabled = (v != 0.0f); return true; }
    if (strcmp(key, "fpToggleKeyEnabled") == 0) { c.fpToggleKeyEnabled = (v != 0.0f); return true; }
    if (strcmp(key, "depthPopKeyEnabled") == 0) { c.depthPopKeyEnabled = (v != 0.0f); return true; }
    if (strcmp(key, "fpNativeFlag") == 0) { c.fpNativeFlag = (v != 0.0f); return true; }
    if (strcmp(key, "fpHideHead") == 0) { c.fpHideHead = (v != 0.0f); return true; }
    if (strcmp(key, "fpHeadHideDelay") == 0) { c.fpHeadHideDelay = v; return true; }
    struct FpKeyMap { const char* prefix; VrConfig::FpStateCfg* st; };
    const FpKeyMap fpStates[] = {
        { "fpExploreStorm", &c.fpExploreStorm },
        { "fpExplore",      &c.fpExplore },
        { "fpCombatStorm",  &c.fpCombatStorm },
        { "fpCombatAim",    &c.fpCombatAim },
        { "fpCombat",       &c.fpCombat },
    };
    for (const FpKeyMap& s : fpStates)
    {
        const size_t n = strlen(s.prefix);
        if (strncmp(key, s.prefix, n) != 0) continue;
        if (strcmp(key + n, "_on")    == 0) { s.st->on    = (v != 0.0f); return true; }
        if (strcmp(key + n, "_fwd")   == 0) { s.st->fwd   = v; return true; }
        if (strcmp(key + n, "_right") == 0) { s.st->right = v; return true; }
        if (strcmp(key + n, "_up")    == 0) { s.st->up    = v; return true; }
    }
    return false;
}

void ApplyKeyValue(VrConfig& c, const char* key, double val) noexcept
{
    const float v = static_cast<float>(val);
    const bool  b = (val != 0.0);
    const int   i = static_cast<int>(val);
    if (ApplyElemKey(c, key, v, b)) return;
    if (ApplyPerModeKey(c, key, v)) return;   // BUG FIX 2026-07-08: helper existed but was never CALLED -> the
                                              // Stereo/DIBR per-mode scalars + now the fp* keys saved but never
                                              // loaded back. Wired in; also carries the First-person keys.
    // Resolution watchdog: what MELE-VR.bat installed. Standalone if+return.
    if (strcmp(key, "expectedResX") == 0) { c.expectedResX = static_cast<int>(v); return; }
    if (strcmp(key, "expectedResY") == 0) { c.expectedResY = static_cast<int>(v); return; }
    // AER's own depth-pop set (separate from Stereo's since 2026-07-10). Standalone if+return.
    if (strcmp(key, "reliefStrengthAer") == 0) { c.reliefStrengthAer = v; return; }
    if (strcmp(key, "reliefEdgeGuardAer") == 0) { c.reliefEdgeGuardAer = v; return; }
    if (strcmp(key, "reliefNearFreezeAer") == 0) { c.reliefNearFreezeAer = v; return; }
    if (strcmp(key, "reliefAutoConvergeAer") == 0) { c.reliefAutoConvergeAer = b; return; }
    if (strcmp(key, "reliefConvergenceAer") == 0) { c.reliefConvergenceAer = v; return; }
    if (strcmp(key, "reliefFlipAer") == 0) { c.reliefFlipAer = b; return; }
    if (strcmp(key, "reliefCurveAer") == 0) { c.reliefCurveAer = v; return; }
    // Supersample test knob (ULocalPlayer::DynamicResolutionFraction). Standalone if+return.
    if (strcmp(key, "dynResFraction") == 0) { c.dynResFraction = v; return; }
    // [LASERUI] mining-laser layout transform. Standalone if+return (C1061 guard - the big chain is full).
    if (strcmp(key, "laserUiEnabled")  == 0) { c.laserUiEnabled  = b; return; }
    if (strcmp(key, "laserUiScaleX")   == 0) { c.laserUiScaleX   = v; return; }
    if (strcmp(key, "laserUiScaleY")   == 0) { c.laserUiScaleY   = v; return; }
    if (strcmp(key, "laserUiOffsetX")  == 0) { c.laserUiOffsetX  = v; return; }
    if (strcmp(key, "laserUiOffsetY")  == 0) { c.laserUiOffsetY  = v; return; }
    // [CINEVR2] opt-in experimental VR cine. Standalone if+return (C1061 guard). FRESH key names -
    // the old-era keys (convoVr/cutsceneVr/convoZoom/...) are dead and stale ini values must not
    // resurrect as settings.
    if (strcmp(key, "cineVrConvo") == 0) { c.cineVrConvo = b; return; }
    if (strcmp(key, "cineVrCutscene") == 0) { c.cineVrCutscene = b; return; }
    if (strcmp(key, "cineZoom") == 0) { c.cineZoom = v; return; }
    if (strcmp(key, "cineSepScale") == 0) { c.cineSepScale = v; return; }
    if (strcmp(key, "poseTagExact") == 0) { c.poseTagExact = b; return; }
    if (strcmp(key, "cineHeadTracking") == 0) { c.cineHeadTracking = b; return; }
    if (strcmp(key, "cineFpsCap") == 0) { c.cineFpsCap = b; return; }
    if (strcmp(key, "cineWorldYaw") == 0) { c.cineWorldYaw = b; return; }
    if (strcmp(key, "cinePoseTagDelayFrames") == 0) { c.cinePoseTagDelayFrames = v; return; }
    // RETIRED 2026-08-02 with the ME2/ME3 rebuild - stale keys fall through and are ignored:
    // cineV2, cineV2Zoom, cinePoseTagDelayFrames, cineVrHeadTracking, cineVrConvoZoom,
    // cineVrCutsceneZoom, cineVrFillH, cineVrFillV, cineVrViewSize.
    // Relief warp. Standalone if+return, NOT part of the chain below (C1061 guard).
    if (strcmp(key, "reliefProbe") == 0) { c.reliefProbe = b; return; }
    if (strcmp(key, "reliefEnabled") == 0) { c.reliefEnabled = b; return; }
    if (strcmp(key, "reliefStrength") == 0) { c.reliefStrength = v; return; }
    if (strcmp(key, "reliefDarkStrength") == 0) { c.reliefDarkStrength = v; return; }
    if (strcmp(key, "reliefDarkRadius") == 0) { c.reliefDarkRadius = v; return; }
    if (strcmp(key, "reliefUnsharpStrength") == 0) { c.reliefUnsharpStrength = v; return; }
    if (strcmp(key, "reliefUnsharpRadius") == 0) { c.reliefUnsharpRadius = v; return; }
    if (strcmp(key, "reliefEdgeGuard") == 0) { c.reliefEdgeGuard = v; return; }
    if (strcmp(key, "reliefNearFreeze") == 0) { c.reliefNearFreeze = v; return; }
    if (strcmp(key, "reliefVizManual") == 0) { c.reliefVizManual = b; return; }
    if (strcmp(key, "reliefAutoConverge") == 0) { c.reliefAutoConverge = b; return; }
    if (strcmp(key, "reliefConvergence") == 0) { c.reliefConvergence = v; return; }
    if (strcmp(key, "reliefFlip") == 0) { c.reliefFlip = b; return; }
    if (strcmp(key, "reliefCurve") == 0) { c.reliefCurve = v; return; }
    // Comfort aim toggles. Standalone if+return, NOT part of the else-if chain below (C1061 nesting guard).
    if (strcmp(key, "lateLatchPose") == 0) { c.lateLatchPose = b; return; }
    if (strcmp(key, "poseTagDelay") == 0) { c.poseTagDelay = b; return; }
    if (strcmp(key, "poseTagDelayFrames") == 0) { c.poseTagDelayFrames = v; return; }
    if (strcmp(key, "stereoFramePacing") == 0) { c.stereoFramePacing = b; return; }
    if (strcmp(key, "mirrorPresentEvery") == 0) { c.mirrorPresentEvery = (i < 1) ? 1 : ((i > 32) ? 32 : i); return; }
    if (strcmp(key, "forceAllowTearing") == 0) { c.forceAllowTearing = b; return; }
    if (strcmp(key, "questFovMatch") == 0) { c.questFovMatch = b; return; }
    if (strcmp(key, "stereoPerEyeViewState") == 0) { c.stereoPerEyeViewState = b; return; }
    // stereoGlowMirror / cineCharRemap / cineNullViewState / cineKeepGameViewState /
    // cineUnconstrainAspect: REMOVED 2026-07-14 (falsified black-char levers); stale keys fall through.
    if (strcmp(key, "decoupledPitch") == 0) { c.decoupledPitch = b; return; }
    if (strcmp(key, "decoupledYaw") == 0) { c.decoupledYaw = b; return; }
    if (strcmp(key, "moveFollowsHead") == 0) { c.moveFollowsHead = b; return; }   // [MOVEFIX]
    if (strcmp(key, "combatCamHold") == 0) { c.combatCamHold = b; return; }
    if (strcmp(key, "combatCamHoldInvert") == 0) { c.combatCamHoldInvert = b; return; }
    if (strcmp(key, "combatCamHoldGain") == 0) { c.combatCamHoldGain = v; return; }
    // Stage 1: VR controller input
    if (strcmp(key, "controllerAimSmoothing") == 0) { c.controllerAimSmoothing = v; return; }
    if (strcmp(key, "controllerAimHeadBlend") == 0) { c.controllerAimHeadBlend = v; return; }
    if (strcmp(key, "controllerTriggerDeadzone") == 0) { c.controllerTriggerDeadzone = v; return; }
    if      (strcmp(key, "recenterKey")        == 0) c.recenterKey        = i;
    if (strcmp(key, "headLookEnabled")         == 0) c.headLookEnabled    = b;
    else if (strcmp(key, "invertLookYaw")      == 0) c.invertLookYaw      = b;
    else if (strcmp(key, "invertLookPitch")    == 0) c.invertLookPitch    = b;
    else if (strcmp(key, "lookSensitivity")    == 0) c.lookSensitivity    = v;
    else if (strcmp(key, "headLookSmoothing")  == 0) c.headLookSmoothing  = v;
    else if (strcmp(key, "combatHeadTracking") == 0) c.combatHeadTracking = b;
    else if (strcmp(key, "combatHeadAim")      == 0) c.combatHeadAim      = b;
    else if (strcmp(key, "makoHeadAim")        == 0) c.makoHeadAim        = b;
    else if (strcmp(key, "controllerInput") == 0) c.controllerInput = b;
    else if (strcmp(key, "controllerAim") == 0) c.controllerAim = b;
    else if (strcmp(key, "controllerAimFlipForward") == 0) c.controllerAimFlipForward = b;
    else if (strcmp(key, "controllerRightStickLook") == 0) c.controllerRightStickLook = b;
    else if (strcmp(key, "controllerLogRealPad") == 0) c.controllerLogRealPad = b;
    else if (strcmp(key, "invertAimYaw")       == 0) c.invertAimYaw       = b;
    else if (strcmp(key, "invertAimPitch")     == 0) c.invertAimPitch     = b;
    else if (strcmp(key, "movieFlatEnabled")   == 0) c.movieFlatEnabled   = b;
    else if (strcmp(key, "movieMonoQuadWidth") == 0) c.movieMonoQuadWidth = v;
    else if (strcmp(key, "disableDof")         == 0) c.disableDof         = b;
    else if (strcmp(key, "menuFlat")           == 0) c.menuFlat           = b;
    else if (strcmp(key, "menuMonoQuadWidth")  == 0) c.menuMonoQuadWidth  = v;
    else if (strcmp(key, "galaxyFlat")         == 0) c.galaxyFlat         = b;
    else if (strcmp(key, "galaxyMonoQuadWidth")== 0) c.galaxyMonoQuadWidth= v;
    else if (strcmp(key, "galaxyVrZoom")       == 0) c.galaxyVrZoom       = v;
    else if (strcmp(key, "convoMonoQuadWidth") == 0) c.convoMonoQuadWidth = v;
    else if (strcmp(key, "cutsceneMonoQuadWidth") == 0) c.cutsceneMonoQuadWidth = v;
    else if (strcmp(key, "cameraOffsetEnabled")== 0) c.cameraOffsetEnabled= b;
    else if (strcmp(key, "cameraOffsetRight")  == 0) c.cameraOffsetRight  = v;
    else if (strcmp(key, "cameraOffsetUp")     == 0) c.cameraOffsetUp     = v;
    else if (strcmp(key, "flatScreenDistance") == 0) c.flatScreenDistance = v;
    else if (strcmp(key, "screenDistanceEnabled")== 0) c.screenDistanceEnabled= b;
    else if (strcmp(key, "vrFovFillEnabled")  == 0) c.vrFovFillEnabled  = b;
    else if (strcmp(key, "invMatrixFix")      == 0) c.invMatrixFix      = b;
    else if (strcmp(key, "vrFillH")           == 0) c.vrFillH           = v;
    else if (strcmp(key, "vrFillV")           == 0) c.vrFillV           = v;
    else if (strcmp(key, "fovScaleH")          == 0) { /* retired view-fill experiment ignored */ }
    else if (strcmp(key, "fovScaleV")          == 0) { /* retired view-fill experiment ignored */ }
    else if (strcmp(key, "textureFillH")       == 0) { /* retired view-fill experiment ignored */ }
    else if (strcmp(key, "textureFillV")       == 0) { /* retired view-fill experiment ignored */ }
    else if (strcmp(key, "eyeTextureScaleH")   == 0) { /* retired eye-texture experiment ignored */ }
    else if (strcmp(key, "eyeTextureScaleV")   == 0) { /* retired eye-texture experiment ignored */ }
    else if (strcmp(key, "imageFillH")         == 0) { /* retired image-fill experiment ignored */ }
    else if (strcmp(key, "imageFillV")         == 0) { /* retired image-fill experiment ignored */ }
    else if (strcmp(key, "imageCropBottom")    == 0) { /* retired image-crop experiment ignored */ }
    else if (strcmp(key, "screenFillBottomCrop")== 0) { /* retired image-crop experiment ignored */ }
    else if (strcmp(key, "imageMoveY")         == 0) { /* retired image-move experiment ignored */ }
    else if (strcmp(key, "showEyeBackground")  == 0) { /* retired diagnostic ignored */ }
    else if (strcmp(key, "useFullHmdFov")      == 0) { /* retired diagnostic ignored */ }
    else if (strcmp(key, "leanEnabled")        == 0) c.leanEnabled        = b;
    else if (strcmp(key, "leanGain")           == 0) c.leanGain           = v;
    // AER (re-ported 2026-06-29). Legacy aer* keys (aerMode/aerIsolation) are silently ignored below.
    else if (strcmp(key, "aerEnabled")        == 0) c.aerEnabled         = b;
    else if (strcmp(key, "aerHalfEyeUU")      == 0) c.aerHalfEyeUU       = v;
    else if (strcmp(key, "aerSwapEyes")       == 0) c.aerSwapEyes        = b;
    else if (strcmp(key, "aerFpEyeScale")     == 0) c.aerFpEyeScale      = v;
    else if (strcmp(key, "aerFramePacing")    == 0) c.aerFramePacing     = b;
    else if (strcmp(key, "fullRefreshPacing") == 0) c.fullRefreshPacing  = b;
    else if (strcmp(key, "aerFramePacingHz")  == 0) c.aerFramePacingHz   = i;
    else if (strcmp(key, "aerMode")           == 0) { /* legacy: pair-latch mode select, ignored */ }
    else if (strcmp(key, "aerIsolation")      == 0) { /* legacy: ignored */ }
    // Legacy SBS stereo (phased out 2026-07-15): an old ini saying stereoEnabled=1 MIGRATES to the
    // shipping Stereo (SFR) instead of resurrecting the retired mode. Order-proof: this key can never
    // set stereoEnabled true again, no matter where it sits relative to vrMode (duplicated-section
    // inis made key order a real hazard). The field itself stays for struct compat, pinned false.
    else if (strcmp(key, "stereoEnabled")     == 0) { if (b) c.sfr2Enabled = true; c.stereoEnabled = false; }
    else if (strcmp(key, "stereoHalfEyeUU")   == 0) c.stereoHalfEyeUU   = v;
    else if (strcmp(key, "sfr2HalfEyeUU")     == 0) c.sfr2HalfEyeUU     = v;
    else if (strcmp(key, "sfr2Convergence")   == 0) c.sfr2Convergence   = v;
    else if (strcmp(key, "sfr2SwapEyes")      == 0) c.sfr2SwapEyes      = b;
    else if (strcmp(key, "stereoSwapEyes")    == 0) c.stereoSwapEyes    = b;
    else if (strcmp(key, "stereoUiYShift")    == 0) c.stereoUiYShift    = v;
    else if (strcmp(key, "dibrEnabled")       == 0) c.dibrEnabled        = b;
    else if (strcmp(key, "firstRunDone")      == 0) c.firstRunDone       = b;
    // Single mode selector. 1=AER 3=DIBR 0/else=Mono. Stereo = SFR now: BOTH 2 (was old SBS stereo)
    // and 4 (was "Stereo 2") map to sfr2Enabled - old SBS stereo phased out 2026-07-15, never activated.
    else if (strcmp(key, "vrMode")            == 0) { c.aerEnabled = (i==1); c.dibrEnabled = (i==3); c.sfr2Enabled = (i==2 || i==4); c.stereoEnabled = false; }
    else if (strcmp(key, "applyModeResolution")==0) c.applyModeResolution= b;
    else if (strcmp(key, "aerResX")           == 0) c.aerResX            = i;
    else if (strcmp(key, "aerResY")           == 0) c.aerResY            = i;
    else if (strcmp(key, "dibrResX")          == 0) c.dibrResX           = i;
    else if (strcmp(key, "dibrResY")          == 0) c.dibrResY           = i;
    else if (strcmp(key, "stereoResX")        == 0) c.stereoResX         = i;
    else if (strcmp(key, "stereoResY")        == 0) c.stereoResY         = i;
    else if (strcmp(key, "monoResX")          == 0) c.monoResX           = i;
    else if (strcmp(key, "monoResY")          == 0) c.monoResY           = i;
    else if (strcmp(key, "dibrAutoConverge")  == 0) c.dibrAutoConverge   = b;
    else if (strcmp(key, "depthMapNear")       == 0) c.depthMapNear       = v;
    else if (strcmp(key, "depthMapFar")        == 0) c.depthMapFar        = v;
    else if (strcmp(key, "depthMapFlip")       == 0) c.depthMapFlip       = b;
    else if (strcmp(key, "depthMapGamma")      == 0) c.depthMapGamma      = v;
    else if (strcmp(key, "depthWarpGain")      == 0) c.depthWarpGain      = v;
    else if (strcmp(key, "depthWarpConv")      == 0) c.depthWarpConv      = v;
    else if (strcmp(key, "depthWarpFlip")      == 0) c.depthWarpFlip      = b;
    else if (strcmp(key, "pchudEnabled")       == 0) c.pchudEnabled        = b;
    else if (strcmp(key, "pchudScaleX")        == 0) c.pchudScaleX         = v;
    else if (strcmp(key, "pchudScaleY")        == 0) c.pchudScaleY         = v;
    else if (strcmp(key, "pchudOffsetX")       == 0) c.pchudOffsetX        = v;
    else if (strcmp(key, "pchudOffsetY")       == 0) c.pchudOffsetY        = v;
    else if (strcmp(key, "convoEnabled")       == 0) c.convoEnabled        = b;
    else if (strcmp(key, "convoScaleX")        == 0) c.convoScaleX         = v;
    else if (strcmp(key, "convoScaleY")        == 0) c.convoScaleY         = v;
    else if (strcmp(key, "convoOffsetX")       == 0) c.convoOffsetX        = v;
    else if (strcmp(key, "convoOffsetY")       == 0) c.convoOffsetY        = v;
    else if (strcmp(key, "designerUiEnabled")  == 0) c.designerUiEnabled   = b;
    else if (strcmp(key, "designerUiScaleX")   == 0) c.designerUiScaleX    = v;
    else if (strcmp(key, "designerUiScaleY")   == 0) c.designerUiScaleY    = v;
    else if (strcmp(key, "designerUiOffsetX")  == 0) c.designerUiOffsetX   = v;
    else if (strcmp(key, "designerUiOffsetY")  == 0) c.designerUiOffsetY   = v;
    else if (strcmp(key, "subtitleForce")      == 0) c.subtitleForce       = b;
    else if (strcmp(key, "subtitleMode")       == 0) c.subtitleMode        = static_cast<int>(v);
    else if (strcmp(key, "subtitleRedraw")     == 0) c.subtitleRedraw      = b;
    else if (strcmp(key, "subtitleHideOriginal")==0) c.subtitleHideOriginal= b;
    else if (strcmp(key, "subtitlePosXFrac")   == 0) c.subtitlePosXFrac    = v;
    else if (strcmp(key, "subtitlePosYFrac")   == 0) c.subtitlePosYFrac    = v;
    else if (strcmp(key, "subtitleFontIndex")  == 0) c.subtitleFontIndex   = static_cast<int>(val);
    else if (strcmp(key, "subtitleScaleX")     == 0) c.subtitleScaleX      = v;
    else if (strcmp(key, "subtitleScaleY")     == 0) c.subtitleScaleY      = v;
    else if (strcmp(key, "nativeSubtitleEnabled")  == 0) c.nativeSubtitleEnabled  = b;
    else if (strcmp(key, "nativeSubtitlePosXFrac") == 0) c.nativeSubtitlePosXFrac = v;
    else if (strcmp(key, "nativeSubtitlePosYFrac") == 0) c.nativeSubtitlePosYFrac = v;
    else if (strcmp(key, "nativeSubtitleScaleX")   == 0) c.nativeSubtitleScaleX   = v;
    else if (strcmp(key, "nativeSubtitleScaleY")   == 0) c.nativeSubtitleScaleY   = v;
    else if (strcmp(key, "nativeSubtitleFontSize") == 0) c.nativeSubtitleFontSize = v;
    else if (strcmp(key, "menuDistanceM")      == 0) c.menuDistanceM      = v;
    else if (strcmp(key, "menuSizeM")          == 0) c.menuSizeM          = v;
    else if (strcmp(key, "menuOffsetXM")       == 0) c.menuOffsetXM       = v;
    else if (strcmp(key, "menuOffsetYM")       == 0) c.menuOffsetYM       = v;
    // unknown keys are ignored on purpose (forward/backward tolerant - old AER/comfort keys are dropped here)

    // Separate short chain (the else-if run above is at MSVC's block-nesting limit - a new key MUST NOT
    // extend it or it hits C1061). Stereo 2 square-resolution + VR-cinematics keys live here.
    if      (strcmp(key, "sfr2ResX")          == 0) c.sfr2ResX           = i;
    else if (strcmp(key, "sfr2ResY")          == 0) c.sfr2ResY           = i;
    // convoVr/cutsceneVr/convoFp/cutsceneFp/cineFpFwdUU/cineFpUpUU/convoZoom/cutsceneZoom/
    // cineSepScale/vrCinematics: REMOVED 2026-07-18 (VR cine ripped out - cine is flat, permanently).
}

// Write one slot's fields as a "[profile:<name>]" section. Mirrors ApplyKeyValue's key set.
// Emits every setting, including the no-UI knobs.
void WriteProfileSection(FILE* f, const char* name, const VrConfig& c) noexcept
{
    fprintf(f, "[profile:%s]\n", name);

    // ---- VR MODE (the one setting most people touch) ------------------------------------------------
    fprintf(f, "; ===== VR MODE ===== change this ONE number:  2=Stereo   1=AER   3=DIBR   0=Mono\n");
    // Stereo = SFR now -> writes vrMode 2 ("Stereo"). Old SBS stereo is never set, so it never writes.
    fprintf(f, "vrMode = %d\n", c.aerEnabled ? 1 : (c.sfr2Enabled ? 2 : (c.dibrEnabled ? 3 : 0)));
    fprintf(f, "applyModeResolution = %d\n", c.applyModeResolution ? 1 : 0);  // auto-set res per mode (restart)
    fprintf(f, "aerResX = %d\n",    c.aerResX);   fprintf(f, "aerResY = %d\n",    c.aerResY);
    fprintf(f, "dibrResX = %d\n",   c.dibrResX);  fprintf(f, "dibrResY = %d\n",   c.dibrResY);
    fprintf(f, "stereoResX = %d\n", c.stereoResX);fprintf(f, "stereoResY = %d\n", c.stereoResY);
    fprintf(f, "monoResX = %d\n",   c.monoResX);  fprintf(f, "monoResY = %d\n",   c.monoResY);
    fprintf(f, "sfr2ResX = %d\n",   c.sfr2ResX);  fprintf(f, "sfr2ResY = %d\n",   c.sfr2ResY);   // Stereo 2 = square (full frame per eye)

    fprintf(f, "; ----- AER -----\n");
    fprintf(f, "aerHalfEyeUU = %.3f\n",    c.aerHalfEyeUU);       // AER scale/IPD
    fprintf(f, "aerSwapEyes = %d\n",       c.aerSwapEyes ? 1 : 0);   // persist the user's eye-swap
    fprintf(f, "fullRefreshPacing = %d\n", c.fullRefreshPacing ? 1 : 0); // ME2/ME3 AERFULL
    {
        fprintf(f, "aerFpEyeScale = %.3f\n",   c.aerFpEyeScale);
        fprintf(f, "aerFramePacing = %d\n",    c.aerFramePacing ? 1 : 0);
        fprintf(f, "aerFramePacingHz = %d\n",  c.aerFramePacingHz);
    }

    fprintf(f, "; ----- Stereo -----\n");
    fprintf(f, "sfr2HalfEyeUU = %.3f\n", c.sfr2HalfEyeUU);        // stereo separation
    fprintf(f, "sfr2Convergence = %.4f\n", c.sfr2Convergence);    // stereo convergence: off-axis fusion-plane shift
    fprintf(f, "stereoFramePacing = %d\n", c.stereoFramePacing ? 1 : 0);   // MUST persist the user's cap choice
    fprintf(f, "mirrorPresentEvery = %d\n", c.mirrorPresentEvery);         // [MIRRORTHROTTLE] 1 = present the flat mirror every frame
    fprintf(f, "questFovMatch = %d\n", c.questFovMatch ? 1 : 0);           // Meta-runtime FOV crop (fixes Quest Link double image); no-op on VDXR
    fprintf(f, "sfr2SwapEyes = %d\n", c.sfr2SwapEyes ? 1 : 0);             // MUST persist the user's eye-swap
    {
        fprintf(f, "forceAllowTearing = %d\n", c.forceAllowTearing ? 1 : 0);   // adv-only (hidden 2026-07-19); default stays ON
        fprintf(f, "stereoHalfEyeUU = %.3f\n", c.stereoHalfEyeUU);      // legacy SBS (no UI)
        fprintf(f, "stereoSwapEyes = %d\n",    c.stereoSwapEyes ? 1 : 0);
        fprintf(f, "stereoUiYShift = %.3f\n",  c.stereoUiYShift);
        fprintf(f, "stereoPerEyeViewState = %d\n", c.stereoPerEyeViewState ? 1 : 0);
    }

    fprintf(f, "; ----- Resolution watchdog (written by MELE-VR.bat; 0 = unset) -----\n");
    fprintf(f, "expectedResX = %d\n", c.expectedResX);
    fprintf(f, "expectedResY = %d\n", c.expectedResY);

        fprintf(f, "dynResFraction = %.3f\n",  c.dynResFraction);

    fprintf(f, "; ----- DIBR / depth -----\n");
    fprintf(f, "dibrAutoConverge = %d\n",  c.dibrAutoConverge ? 1 : 0);
    // Depth pop NEVER persists on (2026-07-09 decision): force-write 0 so the ini can't carry it enabled
    // across launches (AER flicker + startup-load crash). Opt-in per session via the Insert menu only.
    fprintf(f, "reliefProbe = 0\n");
    fprintf(f, "reliefEnabled = 0\n");
    fprintf(f, "; AER keeps its own depth-pop values - it warps a whole frame per eye, Stereo warps SBS halves.\n");
    fprintf(f, "reliefStrengthAer = %.4f\n",    c.reliefStrengthAer);
    fprintf(f, "reliefEdgeGuardAer = %.3f\n",   c.reliefEdgeGuardAer);
    fprintf(f, "reliefNearFreezeAer = %.4f\n",  c.reliefNearFreezeAer);
    fprintf(f, "reliefAutoConvergeAer = %d\n",  c.reliefAutoConvergeAer ? 1 : 0);
    fprintf(f, "reliefConvergenceAer = %.4f\n", c.reliefConvergenceAer);
    fprintf(f, "reliefFlipAer = %d\n",          c.reliefFlipAer ? 1 : 0);
    fprintf(f, "reliefCurveAer = %.3f\n",       c.reliefCurveAer);
    fprintf(f, "reliefStrength = %.4f\n",   c.reliefStrength);
    fprintf(f, "reliefDarkStrength = %.4f\n", c.reliefDarkStrength);
    fprintf(f, "reliefDarkRadius = %.2f\n",   c.reliefDarkRadius);
    fprintf(f, "reliefUnsharpStrength = %.4f\n", c.reliefUnsharpStrength);
    fprintf(f, "reliefUnsharpRadius = %.2f\n",   c.reliefUnsharpRadius);
    fprintf(f, "reliefEdgeGuard = %.3f\n",  c.reliefEdgeGuard);
    fprintf(f, "reliefNearFreeze = %.4f\n", c.reliefNearFreeze);
    fprintf(f, "reliefVizManual = %d\n",    c.reliefVizManual ? 1 : 0);
    fprintf(f, "reliefAutoConverge = %d\n", c.reliefAutoConverge ? 1 : 0);
    fprintf(f, "reliefConvergence = %.4f\n", c.reliefConvergence);
    fprintf(f, "reliefFlip = %d\n",         c.reliefFlip ? 1 : 0);
    fprintf(f, "reliefCurve = %.3f\n",      c.reliefCurve);
    fprintf(f, "depthMapNear = %.4f\n",     c.depthMapNear);
    fprintf(f, "depthMapFar = %.4f\n",      c.depthMapFar);
    fprintf(f, "depthMapFlip = %d\n",       c.depthMapFlip ? 1 : 0);
    fprintf(f, "depthMapGamma = %.3f\n",    c.depthMapGamma);
    fprintf(f, "depthWarpGain = %.3f\n",    c.depthWarpGain);     // "Depth strength"
    {
        fprintf(f, "depthWarpConv = %.4f\n",    c.depthWarpConv);
        fprintf(f, "depthWarpFlip = %d\n",      c.depthWarpFlip ? 1 : 0);
    }

    fprintf(f, "; ----- Head tracking / aim -----\n");
    fprintf(f, "recenterKey = %d\n",        c.recenterKey);
    fprintf(f, "recenterKeyEnabled = %d\n", c.recenterKeyEnabled ? 1 : 0);
    fprintf(f, "menuKey = %d\n",            c.menuKey);
    fprintf(f, "headLookEnabled = %d\n",    c.headLookEnabled ? 1 : 0);
    fprintf(f, "invertLookYaw = %d\n",      c.invertLookYaw ? 1 : 0);
    fprintf(f, "invertLookPitch = %d\n",    c.invertLookPitch ? 1 : 0);
    fprintf(f, "lookSensitivity = %.3f\n",  c.lookSensitivity);
    fprintf(f, "headLookSmoothing = %.3f\n", c.headLookSmoothing);
    {
        fprintf(f, "lateLatchPose = %d\n",      c.lateLatchPose ? 1 : 0);
        fprintf(f, "poseTagDelay = %d\n",       c.poseTagDelay ? 1 : 0);
        fprintf(f, "poseTagDelayFrames = %.3f\n", c.poseTagDelayFrames);
        fprintf(f, "combatHeadTracking = %d\n", c.combatHeadTracking ? 1 : 0);
    }
    fprintf(f, "combatHeadAim = %d\n",      c.combatHeadAim ? 1 : 0);
    fprintf(f, "makoHeadAim = %d\n",        c.makoHeadAim ? 1 : 0);
    fprintf(f, "controllerInput = %d\n",            c.controllerInput ? 1 : 0);
    fprintf(f, "controllerAim = %d\n",              c.controllerAim ? 1 : 0);
    fprintf(f, "controllerAimFlipForward = %d\n",    c.controllerAimFlipForward ? 1 : 0);
    fprintf(f, "controllerAimSmoothing = %.3f\n",   c.controllerAimSmoothing);
    fprintf(f, "controllerAimHeadBlend = %.3f\n",   c.controllerAimHeadBlend);
    fprintf(f, "controllerTriggerDeadzone = %.3f\n",c.controllerTriggerDeadzone);
    fprintf(f, "controllerRightStickLook = %d\n",   c.controllerRightStickLook ? 1 : 0);
    fprintf(f, "controllerLogRealPad = %d\n",       c.controllerLogRealPad ? 1 : 0);
    fprintf(f, "invertAimYaw = %d\n",       c.invertAimYaw ? 1 : 0);
    fprintf(f, "invertAimPitch = %d\n",     c.invertAimPitch ? 1 : 0);
    fprintf(f, "decoupledPitch = %d\n",     c.decoupledPitch ? 1 : 0);
    fprintf(f, "decoupledYaw = %d\n",       c.decoupledYaw ? 1 : 0);
    fprintf(f, "moveFollowsHead = %d\n",    c.moveFollowsHead ? 1 : 0);   // [MOVEFIX] run where you look
    fprintf(f, "combatCamHold = %d\n",      c.combatCamHold ? 1 : 0);
    fprintf(f, "combatCamHoldInvert = %d\n", c.combatCamHoldInvert ? 1 : 0);
    fprintf(f, "combatCamHoldGain = %.3f\n", c.combatCamHoldGain);

    fprintf(f, "; ----- Conversations / cutscenes -----\n");
    fprintf(f, "cineVrConvo = %d\n",        c.cineVrConvo ? 1 : 0);        // ME2/ME3 cine model
    fprintf(f, "cineVrCutscene = %d\n",     c.cineVrCutscene ? 1 : 0);
    fprintf(f, "cineZoom = %.3f\n",         c.cineZoom);
    fprintf(f, "cineSepScale = %.3f\n",     c.cineSepScale);                // 1.0 = ME2 parity (no cine separation cut)
    fprintf(f, "cineFpsCap = %d\n",         c.cineFpsCap ? 1 : 0);          // [CINEPACE60] VR cine (cutscene + convo) paces to display/2
    fprintf(f, "cineWorldYaw = %d\n",       c.cineWorldYaw ? 1 : 0);        // [WORLDYAW2] A/B: cine head-yaw about world up (0 = local up)
    fprintf(f, "poseTagExact = %d\n",       c.poseTagExact ? 1 : 0);   // tag with the pose the render actually used
    fprintf(f, "convoMonoQuadWidth = %.3f\n", c.convoMonoQuadWidth);
    fprintf(f, "cutsceneMonoQuadWidth = %.3f\n", c.cutsceneMonoQuadWidth);
    fprintf(f, "disableDof = %d\n",         c.disableDof ? 1 : 0);
    fprintf(f, "menuFlat = %d\n",           c.menuFlat ? 1 : 0);
    fprintf(f, "menuMonoQuadWidth = %.3f\n", c.menuMonoQuadWidth);
    fprintf(f, "galaxyFlat = %d\n",         c.galaxyFlat ? 1 : 0);
    fprintf(f, "galaxyMonoQuadWidth = %.3f\n", c.galaxyMonoQuadWidth);
    fprintf(f, "galaxyVrZoom = %.3f\n",     c.galaxyVrZoom);
    fprintf(f, "firstRunDone = %d\n", c.firstRunDone ? 1 : 0);
    fprintf(f, "; ----- First person -----\n");
    fprintf(f, "fpEnabled = %d\n", c.fpEnabled ? 1 : 0);
    fprintf(f, "fpToggleKey = %d\n", c.fpToggleKey);
    fprintf(f, "fpToggleKeyEnabled = %d\n", c.fpToggleKeyEnabled ? 1 : 0);
    fprintf(f, "depthPopKey = %d\n", c.depthPopKey);
    fprintf(f, "depthPopKeyEnabled = %d\n", c.depthPopKeyEnabled ? 1 : 0);
    fprintf(f, "fpNativeFlag = %d\n", c.fpNativeFlag ? 1 : 0);   // no UI (default off)
    fprintf(f, "fpHideHead = %d\n", c.fpHideHead ? 1 : 0);
    fprintf(f, "fpHeadHideDelay = %.1f\n", c.fpHeadHideDelay);
    {
        struct FpSave { const char* prefix; const VrConfig::FpStateCfg* st; };
        const FpSave fpSave[] = {
            { "fpExplore",      &c.fpExplore },
            { "fpExploreStorm", &c.fpExploreStorm },
            { "fpCombat",       &c.fpCombat },
            { "fpCombatStorm",  &c.fpCombatStorm },
            { "fpCombatAim",    &c.fpCombatAim },
        };
        for (const FpSave& s : fpSave)
        {
            fprintf(f, "%s_on = %d\n",     s.prefix, s.st->on ? 1 : 0);
            fprintf(f, "%s_fwd = %.2f\n",  s.prefix, s.st->fwd);
            fprintf(f, "%s_right = %.2f\n",s.prefix, s.st->right);
            fprintf(f, "%s_up = %.2f\n",   s.prefix, s.st->up);
        }
    }

    fprintf(f, "; ----- Camera / FOV fill -----\n");
    fprintf(f, "cameraOffsetEnabled = %d\n",c.cameraOffsetEnabled ? 1 : 0);
    fprintf(f, "cameraOffsetRight = %.3f\n",c.cameraOffsetRight);
    fprintf(f, "cameraOffsetUp = %.3f\n",   c.cameraOffsetUp);
    fprintf(f, "flatScreenDistance = %.3f\n", c.flatScreenDistance);
    fprintf(f, "screenDistanceEnabled = %d\n",c.screenDistanceEnabled ? 1 : 0);
    fprintf(f, "vrFovFillEnabled = %d\n",   c.vrFovFillEnabled ? 1 : 0);
    fprintf(f, "invMatrixFix = %d\n", c.invMatrixFix ? 1 : 0);   // no UI, always-on fix
    fprintf(f, "vrFillH = %.3f\n",          c.vrFillH);
    fprintf(f, "vrFillV = %.3f\n",          c.vrFillV);
    fprintf(f, "leanEnabled = %d\n",        c.leanEnabled ? 1 : 0);
    fprintf(f, "leanGain = %.3f\n",         c.leanGain);

    // HUD move/scale + subtitle layout - the "HUD" menu tab.
    {
    fprintf(f, "; ----- HUD (movie move/scale + subtitles) -----\n");
    fprintf(f, "pchudEnabled = %d\n",       c.pchudEnabled ? 1 : 0);
    fprintf(f, "pchudScaleX = %.3f\n",      c.pchudScaleX);
    fprintf(f, "pchudScaleY = %.3f\n",      c.pchudScaleY);
    fprintf(f, "pchudOffsetX = %.3f\n",     c.pchudOffsetX);
    fprintf(f, "pchudOffsetY = %.3f\n",     c.pchudOffsetY);
    fprintf(f, "pchudScaleXStereo = %.3f\n",  c.pchudScaleXStereo);
    fprintf(f, "pchudScaleYStereo = %.3f\n",  c.pchudScaleYStereo);
    fprintf(f, "pchudOffsetXStereo = %.3f\n", c.pchudOffsetXStereo);
    fprintf(f, "pchudOffsetYStereo = %.3f\n", c.pchudOffsetYStereo);
    fprintf(f, "convoEnabled = %d\n",       c.convoEnabled ? 1 : 0);
    fprintf(f, "convoScaleX = %.3f\n",      c.convoScaleX);
    fprintf(f, "convoScaleY = %.3f\n",      c.convoScaleY);
    fprintf(f, "convoOffsetX = %.3f\n",     c.convoOffsetX);
    fprintf(f, "convoOffsetY = %.3f\n",     c.convoOffsetY);
    fprintf(f, "convoScaleXStereo = %.3f\n",  c.convoScaleXStereo);
    fprintf(f, "convoScaleYStereo = %.3f\n",  c.convoScaleYStereo);
    fprintf(f, "convoOffsetXStereo = %.3f\n", c.convoOffsetXStereo);
    fprintf(f, "convoOffsetYStereo = %.3f\n", c.convoOffsetYStereo);
    fprintf(f, "designerUiEnabled = %d\n",  c.designerUiEnabled ? 1 : 0);
    fprintf(f, "designerUiScaleX = %.3f\n", c.designerUiScaleX);
    fprintf(f, "designerUiScaleY = %.3f\n", c.designerUiScaleY);
    fprintf(f, "designerUiOffsetX = %.3f\n", c.designerUiOffsetX);
    fprintf(f, "designerUiOffsetY = %.3f\n", c.designerUiOffsetY);
    fprintf(f, "laserUiEnabled = %d\n",  c.laserUiEnabled ? 1 : 0);   // [LASERUI] mining-laser layout set
    fprintf(f, "laserUiScaleX = %.3f\n", c.laserUiScaleX);
    fprintf(f, "laserUiScaleY = %.3f\n", c.laserUiScaleY);
    fprintf(f, "laserUiOffsetX = %.3f\n", c.laserUiOffsetX);
    fprintf(f, "laserUiOffsetY = %.3f\n", c.laserUiOffsetY);
    fprintf(f, "subtitleRedraw = %d\n",     c.subtitleRedraw ? 1 : 0);
    fprintf(f, "subtitleHideOriginal = %d\n", c.subtitleHideOriginal ? 1 : 0);
    fprintf(f, "subtitlePosXFrac = %.4f\n", c.subtitlePosXFrac);
    fprintf(f, "subtitlePosYFrac = %.4f\n", c.subtitlePosYFrac);
    fprintf(f, "subtitleFontIndex = %d\n",  c.subtitleFontIndex);
    fprintf(f, "subtitleScaleX = %.3f\n",   c.subtitleScaleX);
    fprintf(f, "subtitleScaleY = %.3f\n",   c.subtitleScaleY);
    fprintf(f, "subtitlePosXFracStereo = %.4f\n", c.subtitlePosXFracStereo);
    fprintf(f, "subtitlePosYFracStereo = %.4f\n", c.subtitlePosYFracStereo);
    fprintf(f, "subtitleScaleXStereo = %.3f\n",   c.subtitleScaleXStereo);
    fprintf(f, "subtitleScaleYStereo = %.3f\n",   c.subtitleScaleYStereo);
    fprintf(f, "; native subtitle: font-size write (proven) + render-mode force (retest); region = proven dead\n");
    fprintf(f, "nativeSubtitleFontSize = %.1f\n", c.nativeSubtitleFontSize);
    fprintf(f, "subtitleForce = %d\n",            c.subtitleForce ? 1 : 0);
    fprintf(f, "subtitleMode = %d\n",             c.subtitleMode);

    // Per-element overrides: only non-default elements are written (keeps the ini readable).
    auto writeElems = [&](const char* prefix, const char* const* names, int n,
                          const VrConfig::ElemOverride* elems)
    {
        for (int i = 0; i < n; ++i)
        {
            const VrConfig::ElemOverride& e = elems[i];
            const bool isDefault = !e.on && !e.hide && e.offX == 0.0f && e.offY == 0.0f &&
                                   e.scaleX == 1.0f && e.scaleY == 1.0f;
            if (isDefault) continue;
            fprintf(f, "%s%s_on = %d\n",       prefix, names[i], e.on ? 1 : 0);
            fprintf(f, "%s%s_hide = %d\n",     prefix, names[i], e.hide ? 1 : 0);
            fprintf(f, "%s%s_offX = %.1f\n",   prefix, names[i], e.offX);
            fprintf(f, "%s%s_offY = %.1f\n",   prefix, names[i], e.offY);
            fprintf(f, "%s%s_scaleX = %.3f\n", prefix, names[i], e.scaleX);
            fprintf(f, "%s%s_scaleY = %.3f\n", prefix, names[i], e.scaleY);
        }
    };
    // Individual element overrides (radar + weapon/health bar are the only ones the UI exposes; only
    // non-default elements are written). Group keys retired 2026-07-06 (master center-pivot replaced them).
    fprintf(f, "; individual HUD element overrides (radar, weapon/health bar)\n");
    writeElems("hudElem_", MELEVR::Pchud::kHudElemNames, VrConfig::kHudElemCount, c.hudElems);
    fprintf(f, "; individual HUD element overrides - STEREO layout\n");
    writeElems("hudElemStereo_", MELEVR::Pchud::kHudElemNames, VrConfig::kHudElemCount, c.hudElemsStereo);
    writeElems("hudElemDibr_", MELEVR::Pchud::kHudElemNames, VrConfig::kHudElemCount, c.hudElemsDibr);
    fprintf(f, "; conversation-movie element overrides (backend; subtitle work)\n");
    writeElems("convoElem_", MELEVR::Pchud::kConvoElemNames, VrConfig::kConvoElemCount, c.convoElems);
    }   // end HUD tab

    fprintf(f, "; ----- Comfort / menu -----\n");
    fprintf(f, "menuDistanceM = %.3f\n",    c.menuDistanceM);
    fprintf(f, "menuSizeM = %.3f\n",        c.menuSizeM);
    fprintf(f, "menuOffsetXM = %.3f\n",     c.menuOffsetXM);
    fprintf(f, "menuOffsetYM = %.3f\n",     c.menuOffsetYM);
    fprintf(f, "\n");
}

// Match a slot name (the bit after "profile:") to an index, or -1.
int ProfileIndexByName(const char* name) noexcept
{
    for (int i = 0; i < kProfileCount; ++i)
        if (strcmp(name, kProfileNames[i]) == 0) return i;
    return -1;
}

// Trim leading whitespace and trailing whitespace/newline in place (for the section-name token).
void Trim(char* s) noexcept
{
    char* start = s;
    while (*start == ' ' || *start == '\t') ++start;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || s[len - 1] == ' ' || s[len - 1] == '\t'))
        s[--len] = '\0';
}

// Read the whole ini into g_slots[] + resolve g_active. Missing file/sections -> defaults. The current
// g_cfg is NOT touched here; callers copy the chosen slot into it afterwards.
// Returns the active profile index parsed from [active] (default 0 / "Default").
int ReadIniIntoSlots() noexcept
{
    for (int i = 0; i < kProfileCount; ++i) g_slots[i] = VrConfig{};  // start from defaults

    const std::wstring path = IniPath();
    FILE* f = _wfopen(path.c_str(), L"r");
    if (f == nullptr)
    {
        Logger::LogLine("[config] no MELEVR.ini at " + Logger::WideToUtf8(path) + " - using defaults");
        return 0;
    }

    // Global hotkeys reset to the DEFAULT map before parse, so an ini with no [hotkeys] section (e.g. a
    // fresh install) still gets 1/2/3/4. A user who CLEARS a bind writes profileN=0, which the parse below
    // then restores over this default - the clear is respected because it is present in the file.
    for (int i = 0; i < kProfileCount; ++i) g_profileHotkey[i] = kDefaultProfileHotkey[i];
    g_profileHotkeysEnabled = true;

    int activeIdx = 0;
    int curSlot   = -1;      // -1 = no profile section open yet
    bool inActive = false;   // true while inside the [active] section
    bool inHotkeys = false;  // true while inside the [hotkeys] section

    // 1024, not 256: a long profile line must not be split across two fgets calls.
    char line[1024];
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        // Section header? "[name]" - find first non-space char.
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '[')
        {
            char section[160] = {0};
            // capture everything up to the closing ']'
            if (sscanf(p, "[%159[^]]]", section) == 1)
            {
                Trim(section);
                inActive = (strcmp(section, "active") == 0);
                inHotkeys = (strcmp(section, "hotkeys") == 0);
                curSlot  = -1;
                if (strncmp(section, "profile:", 8) == 0)
                    curSlot = ProfileIndexByName(section + 8);  // -1 if an unknown profile name
            }
            continue;
        }

        if (inHotkeys)
        {
            int hv = 0;
            if (sscanf(line, " enabled = %d", &hv) == 1) { g_profileHotkeysEnabled = (hv != 0); continue; }
            int slot = 0, vk = 0;
            if (sscanf(line, " profile%d = %d", &slot, &vk) == 2 && slot >= 0 && slot < kProfileCount)
                g_profileHotkey[slot] = vk;
            continue;
        }

        if (inActive)
        {
            // [active] profile = <name>  (string value, not numeric)
            char pname[160] = {0};
            if (sscanf(line, " profile = %159[^\r\n]", pname) == 1)
            {
                Trim(pname);
                const int idx = ProfileIndexByName(pname);
                if (idx >= 0) activeIdx = idx;
            }
            continue;
        }

        // key = value inside a profile section. String-valued keys are parsed first (they aren't %lf),
        // then the numeric path. Lines outside any known section are ignored.
        if (curSlot >= 0)
        {
            char key[128] = {0};
            double val = 0.0;
            if (sscanf(line, " %127[^= ] = %lf", key, &val) == 2)
                ApplyKeyValue(g_slots[curSlot], key, val);
        }
    }
    fclose(f);
    Logger::LogLine("[config] loaded " + Logger::WideToUtf8(path) +
                    " (active profile: " + kProfileNames[activeIdx] + ")");
    return activeIdx;
}

// Write all 4 slots + the [active] line. g_slots[] is the source of truth, so slots left untouched
// retain their last-loaded values (read-modify-write across the whole file in one shot).
void WriteIniFromSlots() noexcept
{
    const std::wstring path = IniPath();
    FILE* f = _wfopen(path.c_str(), L"w");
    if (f == nullptr)
    {
        Logger::LogLine("[config] FAILED to write " + Logger::WideToUtf8(path));
        return;
    }
    fprintf(f, "; MELE VR config - written by the in-headset menu.\n\n");
    fprintf(f, "[active]\n");
    fprintf(f, "profile=%s\n\n", kProfileNames[g_active]);
    // Global profile-switch hotkeys (VK code per slot; 0 = unmapped). Not inside any [profile:*] section.
    fprintf(f, "[hotkeys]\n");
    fprintf(f, "enabled=%d\n", g_profileHotkeysEnabled ? 1 : 0);
    for (int i = 0; i < kProfileCount; ++i)
        fprintf(f, "profile%d=%d\n", i, g_profileHotkey[i]);
    fprintf(f, "\n");
    for (int i = 0; i < kProfileCount; ++i)
        WriteProfileSection(f, kProfileNames[i], g_slots[i]);
    fclose(f);
    Logger::LogLine("[config] saved " + Logger::WideToUtf8(path) +
                    " (active profile: " + kProfileNames[g_active] + ")");
}
}  // namespace

// Which depth-pop set is live. AER only - DIBR has its own depthWarp* knobs, and Mono has no pop.
ReliefParams ActiveRelief(const VrConfig& c) noexcept
{
    if (c.aerEnabled)
    {
        return ReliefParams{c.reliefStrengthAer, c.reliefEdgeGuardAer, c.reliefNearFreezeAer,
                            c.reliefConvergenceAer, c.reliefCurveAer, c.reliefAutoConvergeAer, c.reliefFlipAer,
                            c.reliefDarkStrength, c.reliefDarkRadius,
                            c.reliefUnsharpStrength, c.reliefUnsharpRadius};
    }
    return ReliefParams{c.reliefStrength, c.reliefEdgeGuard, c.reliefNearFreeze,
                        c.reliefConvergence, c.reliefCurve, c.reliefAutoConverge, c.reliefFlip,
                        c.reliefDarkStrength, c.reliefDarkRadius,
                        c.reliefUnsharpStrength, c.reliefUnsharpRadius};
}

VrConfig& Get() noexcept
{
    static VrConfig g_cfg;  // Meyers singleton - the LIVE config; xr_session holds a const ref to this.
    return g_cfg;
}

void ResetDefaults() noexcept
{
    // Copy-assign a fresh default-constructed config into the live one. The struct's member initializers
    // ARE the defaults, and copying in place keeps every reference bound to Get() valid.
    Get() = VrConfig{};
}

void LoadFromIni() noexcept
{
    g_active = ReadIniIntoSlots();   // parse all slots + resolve active from [active]
    Get() = g_slots[g_active];       // copy the active slot into the live config (address unchanged)
    // First person ALWAYS boots OFF (2026-07-08 decision): it's experimental, so make it opt-in per session
    // (toggle with the K key in-game) rather than restoring a saved-on state that could surprise on launch.
    Get().fpEnabled = false;
    // Depth pop (relief warp) ALWAYS boots OFF (2026-07-09 decision): with it on, AER flickers on load-in
    // and it was implicated in an intermittent startup-load NVIDIA-driver crash (0xC0000005 in
    // nvwgf2umx.dll during the level load). Opt-in per session via the Insert menu, never restored from
    // disk. reliefProbe (the diagnostic depth capture) rides the same rule so nothing runs the depth
    // pipeline at launch. The serializer force-writes these as 0 so the ini never carries them on.
    Get().reliefEnabled = false;
    Get().reliefProbe   = false;
    // Per-eye view state MIGRATION (2026-07-13): every ini written before today carries the old default 0,
    // which re-disables the fix for the right-eye missing bloom/effects + black convo characters. Force ON
    // at load (the menu toggle still works live for an in-session A/B; the serializer writes 1 from now on).
    Get().stereoPerEyeViewState = true;
    // vrCinematics force-off REMOVED 2026-07-15 to make VR cinematics work. The 07-14 retirement predates
    // Stereo 2 - back then VR cine meant SBS stereo's black-character bug. Stereo 2 renders each eye as
    // a full primary render, so that bug family is impossible by construction there, and the toggle is
    // back as a real menu option (EXPERIMENTAL section), honored from the ini like any other setting.
}

void SaveToIni() noexcept
{
    g_slots[g_active] = Get();       // snapshot the live config into the active slot
    WriteIniFromSlots();             // re-write the whole file (other slots preserved from g_slots)
}

int ProfileCount() noexcept { return kProfileCount; }

const char* ProfileName(int index) noexcept
{
    if (index < 0 || index >= kProfileCount) return "?";
    return kProfileNames[index];
}

int ActiveProfileIndex() noexcept { return g_active; }

void SaveProfile(int index) noexcept
{
    if (index < 0 || index >= kProfileCount) return;
    g_slots[index] = Get();          // copy the live config into slot `index` (active unchanged)
    WriteIniFromSlots();             // persist all slots + the unchanged [active]
    Logger::LogLine(std::string("[config] saved profile slot ") + kProfileNames[index]);
}

void LoadProfile(int index) noexcept
{
    if (index < 0 || index >= kProfileCount) return;
    g_active = index;                // mark active
    Get() = g_slots[index];          // apply the slot's values to the live config (address unchanged)
    WriteIniFromSlots();             // persist the new [active] line (+ all slots, unchanged values)
    Logger::LogLine(std::string("[config] loaded profile slot ") + kProfileNames[index]);
}

// ---- Profile hotkeys (global map, [hotkeys] section) --------------------------------------------------
int ProfileHotkey(int index) noexcept
{
    if (index < 0 || index >= kProfileCount) return 0;
    return g_profileHotkey[index];
}

int& ProfileHotkeyRef(int index) noexcept
{
    static int dummy = 0;
    if (index < 0 || index >= kProfileCount) { dummy = 0; return dummy; }
    return g_profileHotkey[index];
}

bool  ProfileHotkeysEnabled() noexcept    { return g_profileHotkeysEnabled; }
bool& ProfileHotkeysEnabledRef() noexcept { return g_profileHotkeysEnabled; }

// ---- setres per mode -----------------------------------------------------------------------------------
// Resolve GamerSettings.ini relative to this dll: it sits in ...\Game\ME1\Binaries\Win64\, the config is in
// ...\Game\ME1\BioGame\Config\GamerSettings.ini -> up two, then BioGame\Config.
std::wstring GameResIniPath() noexcept
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&GameResIniPath), &self);
    wchar_t path[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path, n);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash != std::wstring::npos) p.resize(slash + 1);
    p += L"..\\..\\BioGame\\Config\\GamerSettings.ini";
    return p;
}

bool WriteGameResolution(int resX, int resY) noexcept
{
    if (resX <= 0 || resY <= 0 || resX > 16384 || resY > 16384) return false;
    const std::wstring path = GameResIniPath();

    FILE* f = _wfopen(path.c_str(), L"rb");
    if (f == nullptr) { Logger::LogLine("[setres] GamerSettings.ini not found; skipped"); return false; }
    std::string data;
    { char buf[4096]; size_t r; while ((r = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, r); }
    fclose(f);

    // Line-based, section-agnostic replace of the exact keys ResX= / ResY= (preserve CRLF + everything else).
    auto replaceKey = [&](const char* key, int value) {
        const std::string k = key;                 // e.g. "ResX="
        std::string out;
        out.reserve(data.size() + 16);
        size_t i = 0;
        bool replaced = false;
        while (i < data.size())
        {
            size_t eol = data.find('\n', i);
            if (eol == std::string::npos) eol = data.size();
            std::string line = data.substr(i, eol - i);
            size_t s = line.find_first_not_of(" \t");
            if (!replaced && s != std::string::npos && line.compare(s, k.size(), k) == 0)
            {
                std::string tail;                  // keep a trailing '\r' if present
                if (!line.empty() && line.back() == '\r') tail = "\r";
                line = line.substr(0, s) + k + std::to_string(value) + tail;
                replaced = true;
            }
            out += line;
            if (eol < data.size()) out += '\n';
            i = eol + 1;
        }
        data.swap(out);
        return replaced;
    };
    const bool okX = replaceKey("ResX=", resX);
    const bool okY = replaceKey("ResY=", resY);
    if (!okX || !okY) { Logger::LogLine("[setres] ResX/ResY keys not found in GamerSettings.ini"); return false; }

    f = _wfopen(path.c_str(), L"wb");
    if (f == nullptr) { Logger::LogLine("[setres] GamerSettings.ini not writable"); return false; }
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    Logger::LogLine("[setres] GamerSettings.ini -> " + std::to_string(resX) + "x" + std::to_string(resY) +
                    " (applies on next launch)");
    return true;
}

void ApplyResolutionForCurrentMode(int* outResX, int* outResY) noexcept
{
    if (outResX) *outResX = 0;
    if (outResY) *outResY = 0;
    const VrConfig& c = Get();
    if (!c.applyModeResolution) return;

    int rx, ry;
    if (c.aerEnabled)         { rx = c.aerResX;    ry = c.aerResY; }
    else if (c.dibrEnabled)   { rx = c.dibrResX;   ry = c.dibrResY; }
    else if (c.sfr2Enabled)   { rx = c.sfr2ResX;   ry = c.sfr2ResY; }   // SFR = full frame per eye -> SQUARE (like AER)
    else if (c.stereoEnabled) { rx = c.stereoResX; ry = c.stereoResY; }
    else                      { rx = c.monoResX;   ry = c.monoResY; }   // mono / flat

    if (WriteGameResolution(rx, ry))
    {
        if (outResX) *outResX = rx;
        if (outResY) *outResY = ry;
    }
}
}
