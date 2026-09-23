#include "xr_session.h"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3d11.lib")   // [SVRDIAG2] fresh-device discriminator probe

#include <atomic>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

#include <cmath>

#include "xr_types.h"
#include "logger.h"
#include "render_hook.h"
#include "d3d_capture.h"
#include "vr_menu.h"
#include "vr_config.h"
#include "head_aim.h"
#include "pchud.h"
#include "xr_input.h"   // Stage 1: controller input (M0: Init/OnFrame, consumers off)

// Milestone 1: 6DOF head tracking (rotation), no stereo. Builds on M0's OpenXR plumbing.
//  - LOOK AROUND, two modes by weapon state:
//      * Explore (weapon holstered): the CalcSceneView hook (render_hook) rotates the rendered view by the
//        head yaw/pitch - camera-only, decoupled (the pawn/spring-arm never move).
//      * Combat (weapon drawn): the head drives ControlRotation (DriveCameraWithHead) so you AIM with your
//        head; CalcSceneView look-around is off to avoid double rotation.
//  - WORLD-LOCKED submission: a projection layer (not a face-strapped quad) tagged with the head orientation
//    but ROLL REMOVED (horizontal right-vector) so the world stays level when you tilt. Both eyes = same
//    image (no stereo yet). FOV matched to what the game actually rendered.
//  - RECENTER (R): re-origins the OpenXR reference space to the current head yaw + position, so "forward"
//    truly resets. Lean (positional) is M1b.

using namespace MELEVR::Xr;
using MELEVR::Logger::LogLine;

namespace
{
std::atomic_bool g_started{false};
std::atomic_bool g_shutdown{false};
std::atomic_bool g_ready{false};

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
int64_t g_colorFormat = 0;
uint32_t g_bbWidth = 0;
uint32_t g_bbHeight = 0;
std::atomic_uint32_t g_recommendedEyeWidth{0};
std::atomic_uint32_t g_recommendedEyeHeight{0};
std::atomic_uint32_t g_lastBackbufferWidth{0};
std::atomic_uint32_t g_lastBackbufferHeight{0};
std::atomic_bool     g_hdrDetected{false};   // true when the game's backbuffer comes through as an HDR float format (= Windows/game HDR is ON -> blue/doubled image)
std::atomic_bool     g_headsetDriven{false}; // [MIRRORTHROTTLE] true while the headset is actually being submitted to (read by PresentHook)
std::atomic_uint32_t g_lastCopySourceWidth{0};
std::atomic_uint32_t g_lastCopySourceHeight{0};
std::atomic_bool g_flatCineCopyActive{false};
// [FREEZETAG] did the SFR pair advance THIS present? Set by the captured-pass submit below and
// read at tag time. A held (stale) pair means the pixels did not move; the tag must not either.
bool g_sfrPairFreshThisFrame = false;
// [CAPTUREPOSE] Arm metadata carried by the exact captured pair selected for this submit. Unlike
// RenderHook::GetPairArmSeq(), this cannot race ahead to a later render after the pixels were copied.
uint64_t g_sfrPairArmThisFrame = 0;

HMODULE g_loader = nullptr;
PFN_xrGetInstanceProcAddr g_getProc = nullptr;
XrInstance g_instance = nullptr;
XrSystemId g_systemId = 0;
Functions g_fn;
bool g_hasRefreshRateExt = false;    // XR_FB_display_refresh_rate enabled => mod can query the true headset Hz
// [LINKFOV] Meta's PC runtime ("Oculus" over Quest Link/Air Link) composites projection layers assuming ITS
// OWN per-eye asymmetric FOV from xrLocateViews; a declared FOV that differs (the symmetric render window)
// is not honored -> each eye's image lands ~7 deg off in OPPOSITE directions = the constant unfusable
// doubling in every mode. VDXR (Virtual Desktop) honors the declared FOV, which is why the same build fuses
// there. Detected once at instance creation from XrInstanceProperties.runtimeName.
bool g_isOculusRuntime = false;
// [STEAMVRFOV] SteamVR: projection views declared WIDER than the runtime's own per-eye frustum are
// displayed as NOTHING (endFrame succeeds, session FOCUSED, compositor void). Log-proven 2026-07-20:
// every visible SteamVR submit declared the hmd's exact asymmetric fov (menus, pre-render fallback);
// every void submit declared the game's symmetric fill fov (0.9425 > hmd's 0.6981 right edge). The
// existing [LINKFOV] tan-space crop (declare the intersection, crop the rect to match) is the exact
// cure - on SteamVR it runs UNCONDITIONALLY (correctness there, not a preference).
bool g_isSteamVrRuntime = false;
bool g_refreshRateApplied = false;   // true once the runtime-reported Hz has been locked into the AER pace
XrSession g_session = nullptr;
XrSpace g_baseSpace = nullptr;  // LOCAL, never re-origined - used to read the absolute head pose at recenter
XrSpace g_appSpace = nullptr;   // LOCAL re-origined on recenter - locate + submit happen against this
XrSpace g_viewSpace = nullptr;  // VIEW (head-locked) - the options menu quad rides this
XrSessionState g_sessionState = 0;
bool g_sessionRunning = false;

struct EyeSwapchain
{
    XrSwapchain swapchain = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<ID3D11Texture2D*> images;
};
EyeSwapchain g_eyes[2];   // [0] = LEFT, [1] = RIGHT. M1 copies the same mono frame into both (no stereo).
// STEREO-ONLY eye swapchains, sized to the game's SBS HALF (bbW/2 x bbH) so each half copies 1:1
// (CopySubresourceRegion, no resample) -> sharp, matching ME2. Separate from g_eyes (headset-res, shared with
// AER/DIBR) so those modes are byte-for-byte untouched. Used only when Stereo mode is active + these are ready.
EyeSwapchain g_stereoEyes[2];
bool g_stereoEyesReady = false;

// Options menu (ImGui -> owned texture -> this head-locked, alpha-blended quad). Insert toggles it.
constexpr int kMenuW = 900;
constexpr int kMenuH = 720;
constexpr XrFlags64 kBlendSourceAlpha = 0x00000002;  // XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
XrSwapchain g_menuSwapchain = nullptr;
std::vector<ID3D11Texture2D*> g_menuImages;
bool g_menuInited = false;
bool g_insertKeyPrev = false;

// [STEAMVRSCENE] SteamVR-only: a tiny swapchain holding opaque black, pre-cleared once at bring-up.
// Submitted as a placeholder PROJECTION layer under the quads whenever a frame would otherwise carry
// no projection layer at all (pre-capture frames, menus, flat cine). SteamVR's compositor is a
// scene-app state machine, not a layer blender: OpenXR quad layers become OVERLAYS (visible in any
// state) but the waiting-room screen stays until the scene app submits projection frames - and it
// returns whenever they stop (vrcompositor stats 2026-07-20: 3205/3245 presents "Timed out" while the
// menu quad showed the whole run). Meta/VDXR have no such state machine, so quad-only frames passed
// there for weeks. Never created / never submitted on non-SteamVR runtimes - those paths stay
// bit-identical to the shipped build.
constexpr int kBlackSceneSize = 64;
XrSwapchain g_blackSceneSwapchain = nullptr;

// The menu-open ControlRotation PIN (a live game write) was REMOVED 2026-07-03 (strip-firstperson-livecode).
// The menu camera-freeze now holds via the menu's READ-ONLY input block ALONE (vr_menu: XInput neutral-pad +
// XInputGetStateEx ordinal-100 + WM_INPUT/raw-mouse/DirectInput swallow), which zeroes what the game reads.

ID3D11Texture2D* g_xrAtlasSource = nullptr;
ID3D11ShaderResourceView* g_xrAtlasSrv = nullptr;
ID3D11VertexShader* g_xrAtlasVs = nullptr;
ID3D11PixelShader* g_xrAtlasPs[2] = {nullptr, nullptr};
ID3D11PixelShader* g_xrFillPs = nullptr;
// [DOWNCHAIN] plain-copy PS + two ping-pong scratch targets for the halving chain (see DownsampleWithin2x).
ID3D11PixelShader* g_xrCopyPs = nullptr;
ID3D11Texture2D* g_dsTex[2] = {nullptr, nullptr};
ID3D11ShaderResourceView* g_dsSrv[2] = {nullptr, nullptr};
ID3D11RenderTargetView* g_dsRtv[2] = {nullptr, nullptr};
UINT g_dsW[2] = {0, 0};
UINT g_dsH[2] = {0, 0};
DXGI_FORMAT g_dsFmt[2] = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN};
ID3D11Buffer* g_xrFillCb = nullptr;
ID3D11SamplerState* g_xrAtlasSampler = nullptr;
ID3D11BlendState* g_xrAtlasBlend = nullptr;
ID3D11RasterizerState* g_xrAtlasRasterizer = nullptr;
ID3D11DepthStencilState* g_xrAtlasDepth = nullptr;
ID3D11Texture2D* g_xrAtlasEyeTex[2] = {nullptr, nullptr};
ID3D11RenderTargetView* g_xrAtlasEyeRtv[2] = {nullptr, nullptr};
bool g_xrAtlasBlitTriedInit = false;
bool g_xrAtlasBlitReady = false;

// AER (alternate-eye rendering) was fully removed 2026-06-28: on a 5090/9800X3D running
// this engine there is no GPU budget to save, so AER's half-rate content + cadence flicker were pure
// downside vs same-frame stereo (which renders both eyes fresh every frame). Same-frame stereo is now the
// primary VR mode; DIBR remains as the depth-reprojection alternative. AER source lives in git history.
int32_t g_lastCtrlYawUU = 0, g_lastCtrlPitchUU = 0;
bool g_lastVOValid = false;
float g_lastVOx = 0.0f, g_lastVOy = 0.0f, g_lastVOz = 0.0f;

// Head pose, refreshed each frame by xrLocateViews against g_appSpace (already re-origined by recenter).
XrView g_subViews[2] = {};
bool g_subViewsValid = false;
bool g_stereoPassSubmitLogged = false;
uint64_t g_viewFillLogCounter = 0;

// Recenter: re-origin g_appSpace to the current head yaw + position. Auto once on the first pose, then on
// the R key edge. recenterKey is rebindable (the menu, Build B); default 'R'.
bool g_haveRecentered = false;
bool g_recenterKeyPrev = false;
int g_recenterKey = 'R';
// Set true the frame a recenter succeeds; consumed in the head-aim section (after the once-per-frame
// controller gates) to release + re-latch head aim so the aim re-zeros to the new forward. Without this,
// recenter re-origins only the VIEW and leaves stale ControlRotation injection - which the stick used to
// paper over, but decoupled pitch/yaw removes that escape hatch, so recenter MUST reset the aim itself.
bool g_recenterReLatchAim = false;
// Recenter re-origins g_appSpace, so every pose derived from the OLD space is instantly wrong: reset the
// unified smoothed head quat and the pose-tag history that frame instead of blending across the jump.
bool g_smoothPoseReset = false;
bool g_tagHistReset = false;

// Head-aim (HMD -> ControlRotation, crosshair follows the head) was stripped 2026-07-03 with the rest of the
// live-write machinery, then RESTORED the same day as the SINGLE sanctioned write exception. The controller
// plumbing lives in head_aim.cpp (slot-identity revalidation + SEH + 90-frame transition quarantine); the
// driver (DriveAimWithHead/ReleaseHeadAim, additive-with-stick model from bdc3c4c) sits below, hard-gated in
// the RunFrame head block. Head-LOOK (render-side CalcSceneView rotation, no game write) is unchanged.
uint64_t g_headControlLogCounter = 0;

std::string FmtXr(XrResult r) { return "XrResult " + std::to_string(static_cast<int>(r)); }

bool MarkerEnabled(const wchar_t* fileName) noexcept
{
    if (fileName == nullptr || fileName[0] == L'\0') return false;

    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;

    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash == nullptr) return false;
    *(slash + 1) = L'\0';

    wchar_t marker[MAX_PATH] = {};
    if (swprintf_s(marker, L"%s%s", exePath, fileName) <= 0) return false;
    const DWORD attrs = GetFileAttributesW(marker);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool XrAtlasSubmitEnabled() noexcept
{
    return MarkerEnabled(L"MELEVR_ENABLE_CAMCB96_XR_ATLAS.txt");
}

void CopyAscii(char* dst, size_t cap, const char* src)
{
    if (dst == nullptr || cap == 0) return;
    size_t i = 0;
    for (; src != nullptr && src[i] != '\0' && i + 1 < cap; ++i) dst[i] = src[i];
    dst[i] = '\0';
}

// Tell the compositor the swapchain is sRGB so the game's already-gamma-encoded pixels aren't encoded twice.
int64_t SrgbSwapchainFormat(int64_t fmt) noexcept
{
    switch (fmt)
    {
    case 28: return 29;  // R8G8B8A8_UNORM -> R8G8B8A8_UNORM_SRGB
    case 87: return 91;  // B8G8R8A8_UNORM -> B8G8R8A8_UNORM_SRGB
    default: return fmt;
    }
}

bool ResolveProc(const char* name, PFN_xrVoidFunction* out)
{
    *out = nullptr;
    const XrResult r = g_getProc(g_instance, name, out);
    if (!XrSucceeded(r) || *out == nullptr)
    {
        LogLine(std::string("[XR] resolve ") + name + " FAILED: " + FmtXr(r));
        return false;
    }
    return true;
}

bool CreateInstance()
{
    g_loader = LoadLibraryW(L"openxr_loader.dll");
    if (g_loader == nullptr)
    {
        MELEVR::Logger::LogWindowsError("[XR] LoadLibraryW(openxr_loader.dll) failed", GetLastError());
        return false;
    }
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN, L"openxr_loader.dll", &pinned);
    LogLine("[XR] openxr_loader.dll loaded.");

    g_getProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(GetProcAddress(g_loader, "xrGetInstanceProcAddr"));
    if (g_getProc == nullptr)
    {
        MELEVR::Logger::LogWindowsError("[XR] xrGetInstanceProcAddr export missing", GetLastError());
        return false;
    }

    PFN_xrVoidFunction createInstanceFn = nullptr;
    if (!XrSucceeded(g_getProc(nullptr, "xrCreateInstance", &createInstanceFn)) || createInstanceFn == nullptr)
    {
        LogLine("[XR] xrCreateInstance resolve FAILED.");
        return false;
    }

    // Is XR_FB_display_refresh_rate available? Enumerate FIRST - enabling an UNSUPPORTED extension makes
    // xrCreateInstance fail outright (= no VR), so this only enables it if the runtime advertises it. (AER section 15.1)
    g_hasRefreshRateExt = false;
    {
        PFN_xrVoidFunction enumFn = nullptr;
        if (XrSucceeded(g_getProc(nullptr, "xrEnumerateInstanceExtensionProperties", &enumFn)) && enumFn != nullptr)
        {
            auto enumExt = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(enumFn);
            uint32_t count = 0;
            if (XrSucceeded(enumExt(nullptr, 0, &count, nullptr)) && count > 0 && count < 512)
            {
                std::vector<XrExtensionProperties> props(count);
                for (auto& p : props) { p.type = XR_TYPE_EXTENSION_PROPERTIES_VALUE; p.next = nullptr; }
                if (XrSucceeded(enumExt(nullptr, count, &count, props.data())))
                {
                    for (const auto& p : props)
                        if (std::strcmp(p.extensionName, kDisplayRefreshRateExtensionName) == 0) { g_hasRefreshRateExt = true; break; }
                }
            }
        }
        LogLine(std::string("[XR] XR_FB_display_refresh_rate available: ") + (g_hasRefreshRateExt ? "yes" : "no"));
    }

    const char* extensions[2] = {kD3D11ExtensionName, kDisplayRefreshRateExtensionName};
    XrInstanceCreateInfo info = {};
    info.type = XR_TYPE_INSTANCE_CREATE_INFO_VALUE;
    CopyAscii(info.applicationInfo.applicationName, sizeof(info.applicationInfo.applicationName), "MELEVR M0");
    info.applicationInfo.applicationVersion = 1;
    CopyAscii(info.applicationInfo.engineName, sizeof(info.applicationInfo.engineName), "MELEVR");
    info.applicationInfo.engineVersion = 1;
    info.applicationInfo.apiVersion = MakeXrVersion(1, 0, 0);
    info.enabledExtensionCount = g_hasRefreshRateExt ? 2u : 1u;
    info.enabledExtensionNames = extensions;

    const XrResult r = reinterpret_cast<PFN_xrCreateInstance>(createInstanceFn)(&info, &g_instance);
    LogLine(std::string("[XR] xrCreateInstance (XR_KHR_D3D11_enable") +
            (g_hasRefreshRateExt ? " + XR_FB_display_refresh_rate" : "") + "): " + FmtXr(r));
    if (!XrSucceeded(r) || g_instance == nullptr)
        return false;

    // [LINKFOV] Detect Meta's runtime (see g_isOculusRuntime note). Non-fatal on failure: the flag just
    // stays false and behaviour is identical to before this change (= the VDXR-proven path).
    g_isOculusRuntime = false;
    {
        PFN_xrVoidFunction gipFn = nullptr;
        if (XrSucceeded(g_getProc(g_instance, "xrGetInstanceProperties", &gipFn)) && gipFn != nullptr)
        {
            XrInstanceProperties props = {};
            props.type = XR_TYPE_INSTANCE_PROPERTIES_VALUE;
            if (XrSucceeded(reinterpret_cast<PFN_xrGetInstanceProperties>(gipFn)(g_instance, &props)))
            {
                props.runtimeName[XR_MAX_RUNTIME_NAME_SIZE_VALUE - 1] = '\0';
                g_isOculusRuntime = std::strstr(props.runtimeName, "Oculus") != nullptr ||
                                    std::strstr(props.runtimeName, "Meta") != nullptr;
                g_isSteamVrRuntime = std::strstr(props.runtimeName, "SteamVR") != nullptr;
                LogLine(std::string("[XRRUNTIME] name='") + props.runtimeName +
                        "' oculusFovQuirk=" + (g_isOculusRuntime ? "1" : "0") +
                        " steamVrFovCrop=" + (g_isSteamVrRuntime ? "1" : "0"));
            }
        }
    }
    return true;
}

bool ResolveSessionFunctions()
{
    PFN_xrVoidFunction p = nullptr;
#define RESOLVE(name, field, type)            \
    if (!ResolveProc(name, &p)) return false; \
    g_fn.field = reinterpret_cast<type>(p);
    RESOLVE("xrDestroyInstance", destroyInstance, PFN_xrDestroyInstance)
    RESOLVE("xrPollEvent", pollEvent, PFN_xrPollEvent)
    RESOLVE("xrGetSystem", getSystem, PFN_xrGetSystem)
    RESOLVE("xrEnumerateViewConfigurationViews", enumerateViewConfigurationViews, PFN_xrEnumerateViewConfigurationViews)
    RESOLVE("xrGetD3D11GraphicsRequirementsKHR", getD3D11GraphicsRequirements, PFN_xrGetD3D11GraphicsRequirementsKHR)
    RESOLVE("xrCreateSession", createSession, PFN_xrCreateSession)
    RESOLVE("xrDestroySession", destroySession, PFN_xrDestroySession)
    RESOLVE("xrCreateSwapchain", createSwapchain, PFN_xrCreateSwapchain)
    RESOLVE("xrEnumerateSwapchainImages", enumerateSwapchainImages, PFN_xrEnumerateSwapchainImages)
    RESOLVE("xrAcquireSwapchainImage", acquireSwapchainImage, PFN_xrAcquireSwapchainImage)
    RESOLVE("xrWaitSwapchainImage", waitSwapchainImage, PFN_xrWaitSwapchainImage)
    RESOLVE("xrReleaseSwapchainImage", releaseSwapchainImage, PFN_xrReleaseSwapchainImage)
    RESOLVE("xrBeginSession", beginSession, PFN_xrBeginSession)
    RESOLVE("xrEndSession", endSession, PFN_xrEndSession)
    RESOLVE("xrWaitFrame", waitFrame, PFN_xrWaitFrame)
    RESOLVE("xrBeginFrame", beginFrame, PFN_xrBeginFrame)
    RESOLVE("xrEndFrame", endFrame, PFN_xrEndFrame)
    RESOLVE("xrCreateReferenceSpace", createReferenceSpace, PFN_xrCreateReferenceSpace)
    RESOLVE("xrDestroySpace", destroySpace, PFN_xrDestroySpace)
    RESOLVE("xrLocateViews", locateViews, PFN_xrLocateViews)
#undef RESOLVE
    // Optional: only present if XR_FB_display_refresh_rate was enabled. Not fatal if it fails to resolve.
    g_fn.getDisplayRefreshRate = nullptr;
    if (g_hasRefreshRateExt)
    {
        PFN_xrVoidFunction q = nullptr;
        if (ResolveProc("xrGetDisplayRefreshRateFB", &q) && q != nullptr)
            g_fn.getDisplayRefreshRate = reinterpret_cast<PFN_xrGetDisplayRefreshRateFB>(q);
        LogLine(std::string("[XR] xrGetDisplayRefreshRateFB resolved: ") + (g_fn.getDisplayRefreshRate ? "yes" : "no"));
    }
    return true;
}

bool CreateSessionAndSwapchains()
{
    XrSystemGetInfo sgi = {};
    sgi.type = XR_TYPE_SYSTEM_GET_INFO_VALUE;
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY_VALUE;
    XrResult r = g_fn.getSystem(g_instance, &sgi, &g_systemId);
    LogLine("[XR] xrGetSystem (HMD): " + FmtXr(r));
    if (!XrSucceeded(r) || g_systemId == 0) return false;

    XrGraphicsRequirementsD3D11KHR req = {};
    req.type = XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR_VALUE;
    r = g_fn.getD3D11GraphicsRequirements(g_instance, g_systemId, &req);
    LogLine("[XR] xrGetD3D11GraphicsRequirementsKHR: " + FmtXr(r));
    if (!XrSucceeded(r)) return false;

    uint32_t viewCount = 0;
    r = g_fn.enumerateViewConfigurationViews(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE,
                                             0, &viewCount, nullptr);
    if (!XrSucceeded(r) || viewCount < 2)
    {
        LogLine("[XR] enumerateViewConfigurationViews(count) FAILED: " + FmtXr(r));
        return false;
    }
    std::vector<XrViewConfigurationView> views(viewCount);
    for (auto& v : views) { v.type = XR_TYPE_VIEW_CONFIGURATION_VIEW_VALUE; v.next = nullptr; }
    r = g_fn.enumerateViewConfigurationViews(g_instance, g_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE,
                                             viewCount, &viewCount, views.data());
    if (!XrSucceeded(r)) { LogLine("[XR] enumerateViewConfigurationViews FAILED: " + FmtXr(r)); return false; }
    const bool xrAtlasMarker = MarkerEnabled(L"MELEVR_ENABLE_CAMCB96_XR_ATLAS.txt");
    const bool liveAtlasMarker = MarkerEnabled(L"MELEVR_ENABLE_CAMCB96_ATLAS.txt");
    const bool xrAtlasBlit = xrAtlasMarker || liveAtlasMarker;
    uint32_t recommendedEyeWidth = 0;
    uint32_t recommendedEyeHeight = 0;
    for (uint32_t i = 0; i < viewCount; ++i)
    {
        if (views[i].recommendedImageRectWidth > recommendedEyeWidth) recommendedEyeWidth = views[i].recommendedImageRectWidth;
        if (views[i].recommendedImageRectHeight > recommendedEyeHeight) recommendedEyeHeight = views[i].recommendedImageRectHeight;
    }
    g_recommendedEyeWidth.store(recommendedEyeWidth, std::memory_order_relaxed);
    g_recommendedEyeHeight.store(recommendedEyeHeight, std::memory_order_relaxed);
    const uint32_t eyeWidth = recommendedEyeWidth != 0 ? recommendedEyeWidth : g_bbWidth;
    const uint32_t eyeHeight = recommendedEyeHeight != 0 ? recommendedEyeHeight : g_bbHeight;
    LogLine("[XRATLAS_MARKER] createSession xr=" + std::to_string(xrAtlasMarker ? 1 : 0) +
            " liveAtlas=" + std::to_string(liveAtlasMarker ? 1 : 0) +
            " enabled=" + std::to_string(xrAtlasBlit ? 1 : 0));
    LogLine("[XR] recommended eye image " + std::to_string(views[0].recommendedImageRectWidth) + "x" +
            std::to_string(views[0].recommendedImageRectHeight) + "; game render source " +
            std::to_string(g_bbWidth) + "x" + std::to_string(g_bbHeight) + "; using eye swapchain " +
            std::to_string(eyeWidth) + "x" + std::to_string(eyeHeight) +
            (xrAtlasBlit ? " CAMCB96 atlas-blit eye swapchains." : " backbuffer eye swapchains."));

    // Hand the headset's own recommendation to the auto-resolution cache. It cannot size THIS launch's
    // backbuffer (that was created before OpenXR came up) - it sizes the next one.
    {
        const MELEVR::Config::VrConfig& arc = MELEVR::Config::Get();
        const int vrMode = arc.stereoEnabled ? 2 : (arc.aerEnabled ? 1 : (arc.dibrEnabled ? 3 : 0));
        MELEVR::D3DCapture::NoteHmdEyeSize(views[0].recommendedImageRectWidth,
                                           views[0].recommendedImageRectHeight, vrMode);
    }

    XrGraphicsBindingD3D11KHR binding = {};
    binding.type = XR_TYPE_GRAPHICS_BINDING_D3D11_KHR_VALUE;
    binding.device = g_device;
    XrSessionCreateInfo sci = {};
    sci.type = XR_TYPE_SESSION_CREATE_INFO_VALUE;
    sci.next = &binding;
    sci.systemId = g_systemId;
    r = g_fn.createSession(g_instance, &sci, &g_session);
    LogLine("[XR] xrCreateSession (D3D11 game device): " + FmtXr(r));
    if (!XrSucceeded(r) || g_session == nullptr) return false;

    for (int eye = 0; eye < 2; ++eye)
    {
        XrSwapchainCreateInfo sc = {};
        sc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
        sc.format = xrAtlasBlit ? g_colorFormat : SrgbSwapchainFormat(g_colorFormat);
        sc.sampleCount = 1;
        sc.width = eyeWidth;
        sc.height = eyeHeight;
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        r = g_fn.createSwapchain(g_session, &sc, &g_eyes[eye].swapchain);
        LogLine("[XR] xrCreateSwapchain eye" + std::to_string(eye) + ": " + FmtXr(r));
        if (!XrSucceeded(r) || g_eyes[eye].swapchain == nullptr) return false;
        g_eyes[eye].width = eyeWidth;
        g_eyes[eye].height = eyeHeight;

        uint32_t imageCount = 0;
        r = g_fn.enumerateSwapchainImages(g_eyes[eye].swapchain, 0, &imageCount, nullptr);
        if (!XrSucceeded(r) || imageCount == 0)
        {
            LogLine("[XR] enumerateSwapchainImages(count) FAILED: " + FmtXr(r));
            return false;
        }
        std::vector<XrSwapchainImageD3D11KHR> images(imageCount);
        for (auto& im : images) { im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE; im.next = nullptr; im.texture = nullptr; }
        r = g_fn.enumerateSwapchainImages(g_eyes[eye].swapchain, imageCount, &imageCount,
                                          reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (!XrSucceeded(r)) { LogLine("[XR] enumerateSwapchainImages FAILED: " + FmtXr(r)); return false; }
        for (auto& im : images) g_eyes[eye].images.push_back(im.texture);
        LogLine("[XR] eye" + std::to_string(eye) + " swapchain images: " + std::to_string(imageCount));
    }

    // STEREO sharp eye swapchains: exactly the SBS half size (bbW/2 x bbH) so the halves copy 1:1 (no
    // resample blur). Non-fatal: if this fails, the stereo path falls back to the old blit into g_eyes.
    {
        const uint32_t halfW = g_bbWidth / 2;
        const uint32_t fullH = g_bbHeight;
        // Stage-1 DISABLED: this 1:1 half-copy path skips the vScale crop (CopySbsHalvesToStereoEyes copies
        // full halfW x fullHeight => 0.5 aspect, not the headset's ~0.941), so it distorts. The real blur cause
        // was the render RESOLUTION (square 3072 clamped to 2096), now fixed at 3840x2160 in GamerSettings.ini.
        // Falling back to the proven CopySbsBackbufferToEyes path (correct vScale). Re-enable ONLY after adding
        // the vScale crop to both the swapchain height here AND the CopySubresourceRegion box.
        g_stereoEyesReady = false;   // was: (halfW > 0 && fullH > 0);
        for (int eye = 0; eye < 2 && g_stereoEyesReady; ++eye)
        {
            XrSwapchainCreateInfo ssc = {};
            ssc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
            ssc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
            ssc.format = SrgbSwapchainFormat(g_colorFormat);   // SRGB so the runtime doesn't re-gamma; copy-compatible with the UNORM backbuffer
            ssc.sampleCount = 1; ssc.width = halfW; ssc.height = fullH;
            ssc.faceCount = 1; ssc.arraySize = 1; ssc.mipCount = 1;
            if (!XrSucceeded(g_fn.createSwapchain(g_session, &ssc, &g_stereoEyes[eye].swapchain)) || g_stereoEyes[eye].swapchain == nullptr)
            { g_stereoEyesReady = false; break; }
            g_stereoEyes[eye].width = halfW; g_stereoEyes[eye].height = fullH;
            uint32_t sic = 0;
            g_fn.enumerateSwapchainImages(g_stereoEyes[eye].swapchain, 0, &sic, nullptr);
            std::vector<XrSwapchainImageD3D11KHR> simgs(sic);
            for (auto& im : simgs) { im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE; im.next = nullptr; im.texture = nullptr; }
            g_fn.enumerateSwapchainImages(g_stereoEyes[eye].swapchain, sic, &sic, reinterpret_cast<XrSwapchainImageBaseHeader*>(simgs.data()));
            for (auto& im : simgs) g_stereoEyes[eye].images.push_back(im.texture);
        }
        LogLine("[P1STEREO] sharp stereo eye swapchains " + std::to_string(halfW) + "x" + std::to_string(fullH) +
                " ready=" + std::to_string(g_stereoEyesReady ? 1 : 0));
    }

    // Options-menu quad swapchain (RGBA8 sRGB, fixed menu size). Non-fatal: if it fails the menu just
    // won't appear; head tracking is unaffected.
    {
        XrSwapchainCreateInfo msc = {};
        msc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
        msc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
        msc.format = SrgbSwapchainFormat(28);   // R8G8B8A8_UNORM -> _SRGB (matches the menu's owned texture family)
        msc.sampleCount = 1;
        msc.width = static_cast<uint32_t>(kMenuW);
        msc.height = static_cast<uint32_t>(kMenuH);
        msc.faceCount = 1;
        msc.arraySize = 1;
        msc.mipCount = 1;
        if (XrSucceeded(g_fn.createSwapchain(g_session, &msc, &g_menuSwapchain)) && g_menuSwapchain != nullptr)
        {
            uint32_t mc = 0;
            g_fn.enumerateSwapchainImages(g_menuSwapchain, 0, &mc, nullptr);
            std::vector<XrSwapchainImageD3D11KHR> mim(mc);
            for (auto& im : mim) { im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE; im.next = nullptr; im.texture = nullptr; }
            g_fn.enumerateSwapchainImages(g_menuSwapchain, mc, &mc, reinterpret_cast<XrSwapchainImageBaseHeader*>(mim.data()));
            for (auto& im : mim) g_menuImages.push_back(im.texture);
            LogLine("[M1] menu swapchain " + std::to_string(kMenuW) + "x" + std::to_string(kMenuH) +
                    " images: " + std::to_string(mc));
        }
        else
        {
            LogLine("[M1] menu swapchain creation FAILED - menu disabled this run.");
        }
    }

    // [STEAMVRSCENE] black placeholder-scene swapchain (SteamVR only; see the declaration comment).
    // Every image is filled with opaque black ONCE here via acquire/wait/copy/release, so the submit
    // path can reference the last-released image every frame without ever touching it again.
    // Non-fatal: if anything fails the placeholder is simply never submitted (= today's behaviour).
    if (g_isSteamVrRuntime)
    {
        XrSwapchainCreateInfo bsc = {};
        bsc.type = XR_TYPE_SWAPCHAIN_CREATE_INFO_VALUE;
        bsc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT_VALUE | XR_SWAPCHAIN_USAGE_SAMPLED_BIT_VALUE;
        bsc.format = SrgbSwapchainFormat(28);   // R8G8B8A8_UNORM -> _SRGB (same family as the menu quad)
        bsc.sampleCount = 1;
        bsc.width = static_cast<uint32_t>(kBlackSceneSize);
        bsc.height = static_cast<uint32_t>(kBlackSceneSize);
        bsc.faceCount = 1;
        bsc.arraySize = 1;
        bsc.mipCount = 1;
        if (XrSucceeded(g_fn.createSwapchain(g_session, &bsc, &g_blackSceneSwapchain)) && g_blackSceneSwapchain != nullptr)
        {
            uint32_t bc = 0;
            g_fn.enumerateSwapchainImages(g_blackSceneSwapchain, 0, &bc, nullptr);
            std::vector<XrSwapchainImageD3D11KHR> bim(bc);
            for (auto& im : bim) { im.type = XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR_VALUE; im.next = nullptr; im.texture = nullptr; }
            g_fn.enumerateSwapchainImages(g_blackSceneSwapchain, bc, &bc, reinterpret_cast<XrSwapchainImageBaseHeader*>(bim.data()));

            // Zero-filled UNORM source texture; CopyResource into the sRGB swapchain image is the same
            // UNORM->_SRGB family copy the eye path already relies on.
            std::vector<uint8_t> zeros(static_cast<size_t>(kBlackSceneSize) * kBlackSceneSize * 4, 0);
            D3D11_TEXTURE2D_DESC btd = {};
            btd.Width = kBlackSceneSize;
            btd.Height = kBlackSceneSize;
            btd.MipLevels = 1;
            btd.ArraySize = 1;
            btd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            btd.SampleDesc.Count = 1;
            btd.Usage = D3D11_USAGE_DEFAULT;
            D3D11_SUBRESOURCE_DATA bsd = {};
            bsd.pSysMem = zeros.data();
            bsd.SysMemPitch = kBlackSceneSize * 4;
            ID3D11Texture2D* blackTex = nullptr;
            bool cleared = false;
            if (g_device != nullptr && SUCCEEDED(g_device->CreateTexture2D(&btd, &bsd, &blackTex)) && blackTex != nullptr)
            {
                cleared = true;
                for (uint32_t i = 0; i < bc && cleared; ++i)
                {
                    XrSwapchainImageAcquireInfo ai = {};
                    ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
                    uint32_t idx = 0;
                    if (!XrSucceeded(g_fn.acquireSwapchainImage(g_blackSceneSwapchain, &ai, &idx))) { cleared = false; break; }
                    XrSwapchainImageWaitInfo wi = {};
                    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE;
                    wi.timeout = XR_INFINITE_DURATION_VALUE;
                    if (XrSucceeded(g_fn.waitSwapchainImage(g_blackSceneSwapchain, &wi)))
                    {
                        if (idx < bc && bim[idx].texture != nullptr)
                            g_context->CopyResource(bim[idx].texture, blackTex);
                    }
                    else
                    {
                        cleared = false;
                    }
                    XrSwapchainImageReleaseInfo ri = {};
                    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
                    g_fn.releaseSwapchainImage(g_blackSceneSwapchain, &ri);
                }
                blackTex->Release();
            }
            if (!cleared)
            {
                g_blackSceneSwapchain = nullptr;   // creation half-done: never submit it
                LogLine("[STEAMVRSCENE] black scene swapchain clear FAILED - placeholder disabled this run.");
            }
            else
            {
                LogLine("[STEAMVRSCENE] black placeholder-scene swapchain ready (" +
                        std::to_string(kBlackSceneSize) + "x" + std::to_string(kBlackSceneSize) +
                        ", images: " + std::to_string(bc) + ")");
            }
        }
        else
        {
            LogLine("[STEAMVRSCENE] black scene swapchain creation FAILED - placeholder disabled this run.");
        }
    }

    XrReferenceSpaceCreateInfo bsci = {};
    bsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    bsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
    bsci.poseInReferenceSpace = IdentityPose();
    r = g_fn.createReferenceSpace(g_session, &bsci, &g_baseSpace);
    LogLine("[XR] xrCreateReferenceSpace(LOCAL base): " + FmtXr(r));
    if (!XrSucceeded(r) || g_baseSpace == nullptr) return false;

    XrReferenceSpaceCreateInfo rsci = {};
    rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
    rsci.poseInReferenceSpace = IdentityPose();   // re-origined to the head on the first recenter
    r = g_fn.createReferenceSpace(g_session, &rsci, &g_appSpace);
    LogLine("[XR] xrCreateReferenceSpace(LOCAL app): " + FmtXr(r));
    if (!XrSucceeded(r) || g_appSpace == nullptr) return false;

    XrReferenceSpaceCreateInfo vrci = {};
    vrci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    vrci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW_VALUE;
    vrci.poseInReferenceSpace = IdentityPose();
    r = g_fn.createReferenceSpace(g_session, &vrci, &g_viewSpace);
    LogLine("[XR] xrCreateReferenceSpace(VIEW): " + FmtXr(r));

    // Stage 1: VR controller input. Self-contained (resolves its own loader
    // functions through g_getProc). Fail-safe: on failure the feature is simply
    // off - current behaviour unchanged. M0: all consumers are OFF by default;
    // the first run's [XRINPUT] lines report the loader's action-API generation.
    if (MELEVR::XrInput::Init(g_instance, g_session, g_getProc))
    {
        LogLine("[XR] controller input module initialized");
    }
    else
    {
        LogLine("[XR] controller input module init failed - feature disabled (fail-safe)");
    }
    return XrSucceeded(r) && g_viewSpace != nullptr;
}

void PollEvents()
{
    for (;;)
    {
        XrEventDataBuffer ev = {};
        ev.type = XR_TYPE_EVENT_DATA_BUFFER_VALUE;
        const XrResult pr = g_fn.pollEvent(g_instance, &ev);
        if (pr == XR_EVENT_UNAVAILABLE_VALUE || !XrSucceeded(pr)) return;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED_VALUE)
        {
            const auto* ssc = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
            g_sessionState = ssc->state;
            LogLine("[XR] session state -> " + std::to_string(g_sessionState));
            if (g_sessionState == XR_SESSION_STATE_READY_VALUE && !g_sessionRunning)
            {
                XrSessionBeginInfo sbi = {};
                sbi.type = XR_TYPE_SESSION_BEGIN_INFO_VALUE;
                sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
                const XrResult br = g_fn.beginSession(g_session, &sbi);
                LogLine("[XR] xrBeginSession: " + FmtXr(br));
                if (XrSucceeded(br))
                {
                    g_sessionRunning = true;
                    LogLine("[XR] session running - head-tracked frames submitting now.");
                }
            }
            else if (g_sessionState == XR_SESSION_STATE_STOPPING_VALUE)
            {
                g_fn.endSession(g_session);
                g_sessionRunning = false;
                MELEVR::XrInput::SessionInvalidated();   // Stage 1: drop session-bound action spaces (recreated on next RUNNING)
            }
            else if (g_sessionState == XR_SESSION_STATE_EXITING_VALUE ||
                     g_sessionState == XR_SESSION_STATE_LOSS_PENDING_VALUE)
            {
                g_shutdown.store(true);
            }
        }
    }
}

bool BlitTextureToEyeWithFill(int eye, ID3D11Texture2D* src, float sourceVScale, float fillH, float fillV, float imageMoveY, float sourceY0 = 0.0f, float sourceX = 0.0f) noexcept;

float GetFlatCinematicAspect() noexcept;

bool CopyTextureToEyeFullFrame(int eye, ID3D11Texture2D* src, float sourceX = 0.0f) noexcept
{
    if (eye < 0 || eye > 1 || g_eyes[eye].swapchain == nullptr || g_context == nullptr || src == nullptr) return false;

    D3D11_TEXTURE2D_DESC srcDesc = {};
    src->GetDesc(&srcDesc);
    g_lastCopySourceWidth.store(srcDesc.Width, std::memory_order_relaxed);
    g_lastCopySourceHeight.store(srcDesc.Height, std::memory_order_relaxed);

    // [SFRCONV] a non-zero submit-time convergence shift needs the sampling blit even at equal sizes.
    const bool sizeMismatch = srcDesc.Width != g_eyes[eye].width || srcDesc.Height != g_eyes[eye].height ||
                              sourceX != 0.0f;
    if (sizeMismatch) return BlitTextureToEyeWithFill(eye, src, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, sourceX);

    XrSwapchainImageAcquireInfo ai = {};
    ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    uint32_t index = 0;
    if (!XrSucceeded(g_fn.acquireSwapchainImage(g_eyes[eye].swapchain, &ai, &index))) return false;
    XrSwapchainImageWaitInfo wi = {};
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE;
    wi.timeout = XR_INFINITE_DURATION_VALUE;
    if (!XrSucceeded(g_fn.waitSwapchainImage(g_eyes[eye].swapchain, &wi)))
    {
        XrSwapchainImageReleaseInfo ri = {};
        ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
        g_fn.releaseSwapchainImage(g_eyes[eye].swapchain, &ri);
        return false;
    }

    bool copied = false;
    if (index < g_eyes[eye].images.size() && g_eyes[eye].images[index] != nullptr)
    {
        g_context->CopyResource(g_eyes[eye].images[index], src);
        copied = true;
    }
    XrSwapchainImageReleaseInfo ri = {};
    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(g_eyes[eye].swapchain, &ri);
    return copied;
}

// Copy a rendered game image into eye[eye]'s current swapchain image. The blit path is only a neutral
// size-mismatch fallback; user-facing VR fill is handled by the render-side projection target.
void CopyTextureToEye(int eye, ID3D11Texture2D* src) noexcept
{
    if (eye < 0 || eye > 1 || g_eyes[eye].swapchain == nullptr || g_context == nullptr || src == nullptr) return;

    D3D11_TEXTURE2D_DESC srcDesc = {};
    src->GetDesc(&srcDesc);
    g_lastCopySourceWidth.store(srcDesc.Width, std::memory_order_relaxed);
    g_lastCopySourceHeight.store(srcDesc.Height, std::memory_order_relaxed);

    const bool sizeMismatch = srcDesc.Width != g_eyes[eye].width || srcDesc.Height != g_eyes[eye].height;
    // Flat cinematics draw a 16:9 viewport inside the render target. Preserve that active image aspect instead of
    // assuming the full source texture is the visible frame.
    float monoFillV = 1.0f;
    float contentAspect = (srcDesc.Height > 0) ? static_cast<float>(srcDesc.Width) / static_cast<float>(srcDesc.Height) : 1.0f;
    // The only cinematic-copy trigger is the manual g_flatCineCopyActive flag (defaults false =
    // normal gameplay copy). The camera-mode name that used to label this path was removed with the
    // FP/GameCamera machinery and read empty ever since.
    const bool flatCineCopy = g_flatCineCopyActive.load(std::memory_order_acquire);
    const bool copyCinematic = flatCineCopy;
    float cineSourceVScale = 1.0f;
    if (copyCinematic)
    {
        // [CINERIP] the cine content and the render target are both 16:9 now, so the active region
        // IS the whole source. This resolves to 1.0 and is kept only so a future non-16:9 target
        // cannot silently submit a stretched flat panel.
        contentAspect = GetFlatCinematicAspect();
        if (srcDesc.Width > 0 && srcDesc.Height > 0 && contentAspect > 0.5f && contentAspect < 4.0f)
        {
            const float activeSourceH = static_cast<float>(srcDesc.Width) / contentAspect;
            cineSourceVScale = activeSourceH / static_cast<float>(srcDesc.Height);
            if (cineSourceVScale > 1.0f) cineSourceVScale = 1.0f;
            if (cineSourceVScale < 0.1f) cineSourceVScale = 0.1f;
        }
    }
    const float eyeAspect = (g_eyes[eye].height > 0) ? static_cast<float>(g_eyes[eye].width) / static_cast<float>(g_eyes[eye].height) : 1.0f;
    if (contentAspect > 0.5f && contentAspect < 4.0f && eyeAspect > 0.3f && eyeAspect < 3.0f)
    {
        monoFillV = eyeAspect / contentAspect;
        if (monoFillV > 1.0f) monoFillV = 1.0f;
        if (monoFillV < 0.1f) monoFillV = 0.1f;
    }
    if (sizeMismatch && BlitTextureToEyeWithFill(eye, src, cineSourceVScale, 1.0f, monoFillV, 0.0f)) return;

    XrSwapchainImageAcquireInfo ai = {};
    ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    uint32_t index = 0;
    if (!XrSucceeded(g_fn.acquireSwapchainImage(g_eyes[eye].swapchain, &ai, &index))) return;
    XrSwapchainImageWaitInfo wi = {};
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE;
    wi.timeout = XR_INFINITE_DURATION_VALUE;
    g_fn.waitSwapchainImage(g_eyes[eye].swapchain, &wi);
    if (index < g_eyes[eye].images.size() && g_eyes[eye].images[index] != nullptr)
    {
        g_context->CopyResource(g_eyes[eye].images[index], src);
    }
    XrSwapchainImageReleaseInfo ri = {};
    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(g_eyes[eye].swapchain, &ri);
}
bool CompileAtlasShader(const char* source, const char* entry, const char* profile, ID3DBlob** blob) noexcept
{
    if (blob == nullptr) return false;
    *blob = nullptr;

    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    if (compiler == nullptr) compiler = LoadLibraryW(L"d3dcompiler_43.dll");
    if (compiler == nullptr)
    {
        MELEVR::Logger::LogWindowsError("[XRATLAS_BLIT] LoadLibraryW(d3dcompiler) failed", GetLastError());
        return false;
    }

    using D3DCompileFn = HRESULT (WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                           LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto compile = reinterpret_cast<D3DCompileFn>(GetProcAddress(compiler, "D3DCompile"));
    if (compile == nullptr)
    {
        MELEVR::Logger::LogWindowsError("[XRATLAS_BLIT] GetProcAddress(D3DCompile) failed", GetLastError());
        FreeLibrary(compiler);
        return false;
    }

    ID3DBlob* errors = nullptr;
    const HRESULT hr = compile(source, strlen(source), nullptr, nullptr, nullptr, entry, profile, 0, 0, blob, &errors);
    if (FAILED(hr))
    {
        if (errors != nullptr)
        {
            const char* msg = reinterpret_cast<const char*>(errors->GetBufferPointer());
            LogLine(std::string("[XRATLAS_BLIT] shader compile failed: ") + (msg != nullptr ? msg : "(no text)"));
            errors->Release();
        }
        else
        {
            LogLine("[XRATLAS_BLIT] shader compile failed hr=" + std::to_string(static_cast<long>(hr)));
        }
        FreeLibrary(compiler);
        return false;
    }
    if (errors != nullptr) errors->Release();
    FreeLibrary(compiler);
    return true;
}

bool EnsureXrAtlasBlitResources(ID3D11Texture2D* src) noexcept
{
    if (g_device == nullptr || g_context == nullptr || src == nullptr) return false;
    if (g_xrAtlasBlitReady) return true;
    if (g_xrAtlasBlitTriedInit) return false;
    g_xrAtlasBlitTriedInit = true;

    const char* shaderSrc =
        "Texture2D t0 : register(t0);\n"
        "SamplerState s0 : register(s0);\n"
        "cbuffer FillParams : register(b0) { float4 fillUv; float4 imageRect; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut VSMain(uint id : SV_VertexID) {\n"
        "  float2 p = float2((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0);\n"
        "  VSOut o; o.pos = float4(p, 0.0, 1.0); o.uv = float2((p.x + 1.0) * 0.5, 1.0 - ((p.y + 1.0) * 0.5)); return o;\n"
        "}\n"
        // [DOWNCHAIN] plain 1:1 sample, used for the halving passes below.
        "float4 PSCopy(VSOut i) : SV_Target { return t0.Sample(s0, i.uv); }\n"
        "float4 PSLeft(VSOut i) : SV_Target { return t0.Sample(s0, float2(i.uv.x * 0.5, i.uv.y)); }\n"
        "float4 PSRight(VSOut i) : SV_Target { return t0.Sample(s0, float2(i.uv.x * 0.5 + 0.5, i.uv.y)); }\n"
        "float4 PSFill(VSOut i) : SV_Target { float2 local = (i.uv - imageRect.xy) / imageRect.zw; "
        "if (local.x < 0.0 || local.x > 1.0 || local.y < 0.0 || local.y > 1.0) return float4(0.0, 0.0, 0.0, 1.0); "
        "float2 uv = fillUv.xy + local * fillUv.zw; "
        "if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) return float4(0.0, 0.0, 0.0, 1.0); "
        "return t0.Sample(s0, uv); }\n";
    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psLeftBlob = nullptr;
    ID3DBlob* psRightBlob = nullptr;
    ID3DBlob* psFillBlob = nullptr;
    ID3DBlob* psCopyBlob = nullptr;
    if (!CompileAtlasShader(shaderSrc, "VSMain", "vs_4_0", &vsBlob) ||
        !CompileAtlasShader(shaderSrc, "PSLeft", "ps_4_0", &psLeftBlob) ||
        !CompileAtlasShader(shaderSrc, "PSRight", "ps_4_0", &psRightBlob) ||
        !CompileAtlasShader(shaderSrc, "PSFill", "ps_4_0", &psFillBlob) ||
        !CompileAtlasShader(shaderSrc, "PSCopy", "ps_4_0", &psCopyBlob))
    {
        if (vsBlob) vsBlob->Release();
        if (psLeftBlob) psLeftBlob->Release();
        if (psRightBlob) psRightBlob->Release();
        if (psFillBlob) psFillBlob->Release();
        if (psCopyBlob) psCopyBlob->Release();
        return false;
    }

    HRESULT hr = g_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &g_xrAtlasVs);
    if (SUCCEEDED(hr)) hr = g_device->CreatePixelShader(psLeftBlob->GetBufferPointer(), psLeftBlob->GetBufferSize(), nullptr, &g_xrAtlasPs[0]);
    if (SUCCEEDED(hr)) hr = g_device->CreatePixelShader(psRightBlob->GetBufferPointer(), psRightBlob->GetBufferSize(), nullptr, &g_xrAtlasPs[1]);
    if (SUCCEEDED(hr)) hr = g_device->CreatePixelShader(psFillBlob->GetBufferPointer(), psFillBlob->GetBufferSize(), nullptr, &g_xrFillPs);
    if (SUCCEEDED(hr)) hr = g_device->CreatePixelShader(psCopyBlob->GetBufferPointer(), psCopyBlob->GetBufferSize(), nullptr, &g_xrCopyPs);
    vsBlob->Release();
    psLeftBlob->Release();
    psRightBlob->Release();
    psFillBlob->Release();
    psCopyBlob->Release();
    if (FAILED(hr))
    {
        LogLine("[XRATLAS_BLIT] shader create failed hr=" + std::to_string(static_cast<long>(hr)));
        return false;
    }

    D3D11_TEXTURE2D_DESC srcDesc = {};
    src->GetDesc(&srcDesc);
    srcDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    srcDesc.CPUAccessFlags = 0;
    srcDesc.MiscFlags = 0;
    srcDesc.Usage = D3D11_USAGE_DEFAULT;
    hr = g_device->CreateTexture2D(&srcDesc, nullptr, &g_xrAtlasSource);
    if (FAILED(hr) || g_xrAtlasSource == nullptr)
    {
        LogLine("[XRATLAS_BLIT] CreateTexture2D atlas source failed hr=" + std::to_string(static_cast<long>(hr)));
        return false;
    }
    hr = g_device->CreateShaderResourceView(g_xrAtlasSource, nullptr, &g_xrAtlasSrv);
    if (FAILED(hr) || g_xrAtlasSrv == nullptr)
    {
        LogLine("[XRATLAS_BLIT] CreateShaderResourceView atlas source failed hr=" + std::to_string(static_cast<long>(hr)));
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = g_device->CreateSamplerState(&sd, &g_xrAtlasSampler);
    if (FAILED(hr) || g_xrAtlasSampler == nullptr)
    {
        LogLine("[XRATLAS_BLIT] CreateSamplerState failed hr=" + std::to_string(static_cast<long>(hr)));
        return false;
    }

    D3D11_BUFFER_DESC cbd = {};
    cbd.ByteWidth = 32;
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = g_device->CreateBuffer(&cbd, nullptr, &g_xrFillCb);
    if (FAILED(hr) || g_xrFillCb == nullptr)
    {
        LogLine("[XR_FILL] CreateBuffer fill constants failed hr=" + std::to_string(static_cast<long>(hr)));
        return false;
    }

    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = g_device->CreateBlendState(&bd, &g_xrAtlasBlend);
    if (FAILED(hr) || g_xrAtlasBlend == nullptr)
    {
        LogLine("[XRATLAS_BLIT] CreateBlendState failed hr=" + std::to_string(static_cast<long>(hr)));
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = FALSE;
    hr = g_device->CreateRasterizerState(&rd, &g_xrAtlasRasterizer);
    if (FAILED(hr) || g_xrAtlasRasterizer == nullptr)
    {
        LogLine("[XRATLAS_BLIT] CreateRasterizerState failed hr=" + std::to_string(static_cast<long>(hr)));
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = FALSE;
    dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
    dd.StencilEnable = FALSE;
    hr = g_device->CreateDepthStencilState(&dd, &g_xrAtlasDepth);
    if (FAILED(hr) || g_xrAtlasDepth == nullptr)
    {
        LogLine("[XRATLAS_BLIT] CreateDepthStencilState failed hr=" + std::to_string(static_cast<long>(hr)));
        return false;
    }

    for (int eye = 0; eye < 2; ++eye)
    {
        if (g_eyes[eye].width == 0 || g_eyes[eye].height == 0)
        {
            LogLine("[XRATLAS_BLIT] temp eye texture has invalid size for eye" + std::to_string(eye));
            return false;
        }

        D3D11_TEXTURE2D_DESC eyeDesc = {};
        eyeDesc.Width = g_eyes[eye].width;
        eyeDesc.Height = g_eyes[eye].height;
        eyeDesc.MipLevels = 1;
        eyeDesc.ArraySize = 1;
        eyeDesc.Format = static_cast<DXGI_FORMAT>(g_colorFormat);
        eyeDesc.SampleDesc.Count = 1;
        eyeDesc.Usage = D3D11_USAGE_DEFAULT;
        eyeDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        hr = g_device->CreateTexture2D(&eyeDesc, nullptr, &g_xrAtlasEyeTex[eye]);
        if (FAILED(hr) || g_xrAtlasEyeTex[eye] == nullptr)
        {
            LogLine("[XRATLAS_BLIT] CreateTexture2D temp eye" + std::to_string(eye) + " failed hr=" +
                    std::to_string(static_cast<long>(hr)) + " fmt=" + std::to_string(static_cast<int>(eyeDesc.Format)) +
                    " size=" + std::to_string(eyeDesc.Width) + "x" + std::to_string(eyeDesc.Height));
            return false;
        }

        hr = g_device->CreateRenderTargetView(g_xrAtlasEyeTex[eye], nullptr, &g_xrAtlasEyeRtv[eye]);
        if (FAILED(hr) || g_xrAtlasEyeRtv[eye] == nullptr)
        {
            LogLine("[XRATLAS_BLIT] CreateRenderTargetView temp eye" + std::to_string(eye) + " failed hr=" +
                    std::to_string(static_cast<long>(hr)));
            return false;
        }
    }

    g_xrAtlasBlitReady = true;
    LogLine("[XRATLAS_BLIT] shader crop/fill/blit resources ready; using temp eye RTs then CopyResource to OpenXR images.");
    return true;
}

float ClampVrFill(float v) noexcept
{
    if (v < 0.50f) return 0.50f;
    if (v > 1.25f) return 1.25f;
    return v;
}

// [CINERIP 2026-08-09] LE1 renders every cinematic through a 16:9-constrained camera. The render
// target is 16:9 now (ME2's shape), so the constraint fills the buffer exactly and the content
// aspect is simply 16:9 - there is nothing left to observe. The old draw-time viewport tracker
// that used to answer this was measured DEAD (it was fed a camera-mode name that was always
// empty, so it never recorded a single viewport and every caller silently took this fallback).
float GetFlatCinematicAspect() noexcept
{
    return 16.0f / 9.0f;
}

void BuildFillConstants(float out[8], float sourceX, float sourceWidth, float sourceVScale, float fillH, float fillV, float imageMoveY, float sourceY0 = 0.0f) noexcept
{
    const float h = fillH;
    const float v = fillV;
    float activeV = sourceVScale;
    if (activeV < 0.05f) activeV = 0.05f;
    if (activeV > 1.0f) activeV = 1.0f;

    const float sourceW = sourceWidth;
    const float sourceH = activeV;
    const float destW = h;
    const float destH = v;

    out[0] = sourceX + 0.5f * (sourceWidth - sourceW);
    out[1] = sourceY0;   // vertical band origin (0 = legacy top-aligned; letterbox crop passes the centered band)
    out[2] = sourceW;
    out[3] = sourceH;
    out[4] = 0.5f - (0.5f * destW);
    out[5] = 0.5f - (0.5f * destH) - imageMoveY;
    out[6] = destW;
    out[7] = destH;
}

// [DOWNCHAIN] Lazily (re)create one ping-pong scratch target. Returns false if it cannot be made,
// in which case the caller simply skips the chain and blits as before.
bool EnsureDsTarget(int slot, UINT w, UINT h, DXGI_FORMAT fmt) noexcept
{
    if (slot < 0 || slot > 1 || g_device == nullptr || w == 0 || h == 0) return false;
    if (g_dsTex[slot] != nullptr && g_dsW[slot] == w && g_dsH[slot] == h && g_dsFmt[slot] == fmt) return true;
    if (g_dsRtv[slot] != nullptr) { g_dsRtv[slot]->Release(); g_dsRtv[slot] = nullptr; }
    if (g_dsSrv[slot] != nullptr) { g_dsSrv[slot]->Release(); g_dsSrv[slot] = nullptr; }
    if (g_dsTex[slot] != nullptr) { g_dsTex[slot]->Release(); g_dsTex[slot] = nullptr; }
    g_dsW[slot] = 0; g_dsH[slot] = 0; g_dsFmt[slot] = DXGI_FORMAT_UNKNOWN;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(g_device->CreateTexture2D(&td, nullptr, &g_dsTex[slot])) || g_dsTex[slot] == nullptr) return false;
    if (FAILED(g_device->CreateShaderResourceView(g_dsTex[slot], nullptr, &g_dsSrv[slot])) ||
        FAILED(g_device->CreateRenderTargetView(g_dsTex[slot], nullptr, &g_dsRtv[slot])))
    {
        if (g_dsSrv[slot] != nullptr) { g_dsSrv[slot]->Release(); g_dsSrv[slot] = nullptr; }
        if (g_dsTex[slot] != nullptr) { g_dsTex[slot]->Release(); g_dsTex[slot] = nullptr; }
        return false;
    }
    g_dsW[slot] = w; g_dsH[slot] = h; g_dsFmt[slot] = fmt;
    LogLine("[DOWNCHAIN] scratch " + std::to_string(slot) + " = " + std::to_string(w) + "x" + std::to_string(h));
    return true;
}

// [DOWNCHAIN 2026-08-12] Halve the staged source until it is within 2x of the destination, and return
// the SRV to sample from. WHY: the eye blit is ONE bilinear pass and bilinear reads a 2x2 texel
// neighbourhood, so a >2x minification skips most source pixels - at the 10240 tier each output pixel
// needed ~11 source samples and got 4, which is undersampling and reads in-headset as blocky
// compression artifacts (identified the tier; measured 3.33x into a 3072-wide eye).
// A halving pass with a linear sampler averages exactly 2x2 texels, i.e. a correct box filter, so
// chaining halves is the same thing a mip chain would give without allocating or generating mips.
// Ratios at or under 2x (every shipped tier up to 6144) take ZERO passes and are bit-identical to
// before. Returns g_xrAtlasSrv unchanged on any failure, so this can only ever be a no-op.
ID3D11ShaderResourceView* DownsampleWithin2x(UINT dstW, UINT dstH) noexcept
{
    if (g_xrCopyPs == nullptr || g_xrAtlasSrv == nullptr || g_xrAtlasSource == nullptr) return g_xrAtlasSrv;
    if (dstW == 0 || dstH == 0) return g_xrAtlasSrv;

    D3D11_TEXTURE2D_DESC sd = {};
    g_xrAtlasSource->GetDesc(&sd);
    const UINT srcW = sd.Width;
    const UINT srcH = sd.Height;
    if (srcW <= dstW * 2 && srcH <= dstH * 2) return g_xrAtlasSrv;   // within one bilinear step already

    ID3D11ShaderResourceView* curSrv = g_xrAtlasSrv;
    UINT curW = srcW, curH = srcH;
    int slot = 0;
    int passes = 0;
    while ((curW > dstW * 2 || curH > dstH * 2) && passes < 4)
    {
        const UINT nextW = (curW > dstW * 2) ? (curW + 1) / 2 : curW;
        const UINT nextH = (curH > dstH * 2) ? (curH + 1) / 2 : curH;
        if (!EnsureDsTarget(slot, nextW, nextH, sd.Format)) break;

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(nextW);
        vp.Height = static_cast<float>(nextH);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ID3D11ShaderResourceView* nullSrv = nullptr;
        g_context->PSSetShaderResources(0, 1, &nullSrv);          // curSrv may be the previous RT
        g_context->OMSetRenderTargets(1, &g_dsRtv[slot], nullptr);
        g_context->RSSetViewports(1, &vp);
        g_context->IASetInputLayout(nullptr);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_xrAtlasVs, nullptr, 0);
        g_context->PSSetShader(g_xrCopyPs, nullptr, 0);
        g_context->PSSetShaderResources(0, 1, &curSrv);
        g_context->PSSetSamplers(0, 1, &g_xrAtlasSampler);
        g_context->Draw(3, 0);
        g_context->PSSetShaderResources(0, 1, &nullSrv);
        g_context->OMSetRenderTargets(0, nullptr, nullptr);

        curSrv = g_dsSrv[slot];
        curW = nextW; curH = nextH;
        slot ^= 1;
        ++passes;
    }
    static int s_logged = 0;
    if (passes > 0 && s_logged < 3)
    {
        ++s_logged;
        LogLine("[DOWNCHAIN] " + std::to_string(srcW) + "x" + std::to_string(srcH) + " -> " +
                std::to_string(curW) + "x" + std::to_string(curH) + " in " + std::to_string(passes) +
                " halving pass(es), then the eye blit (dst " + std::to_string(dstW) + "x" +
                std::to_string(dstH) + ")");
    }
    return curSrv;
}

bool BlitTextureToEyeWithFill(int eye, ID3D11Texture2D* src, float sourceVScale, float fillH, float fillV, float imageMoveY, float sourceY0, float sourceX) noexcept
{
    if (eye < 0 || eye > 1 || g_eyes[eye].swapchain == nullptr || g_context == nullptr || src == nullptr) return false;
    if (!EnsureXrAtlasBlitResources(src) || g_xrFillPs == nullptr || g_xrFillCb == nullptr ||
        g_xrAtlasSource == nullptr || g_xrAtlasSrv == nullptr || g_xrAtlasEyeTex[eye] == nullptr ||
        g_xrAtlasEyeRtv[eye] == nullptr)
    {
        return false;
    }
    float fillConstants[8] = {};
    BuildFillConstants(fillConstants, sourceX, 1.0f, sourceVScale, fillH, fillV, imageMoveY, sourceY0);

    g_context->CopyResource(g_xrAtlasSource, src);
    g_context->UpdateSubresource(g_xrFillCb, 0, nullptr, fillConstants, 0, 0);

    XrSwapchainImageAcquireInfo ai = {};
    ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    uint32_t index = 0;
    XrResult xr = g_fn.acquireSwapchainImage(g_eyes[eye].swapchain, &ai, &index);
    if (!XrSucceeded(xr)) return false;

    XrSwapchainImageWaitInfo wi = {};
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE;
    wi.timeout = XR_INFINITE_DURATION_VALUE;
    xr = g_fn.waitSwapchainImage(g_eyes[eye].swapchain, &wi);
    if (!XrSucceeded(xr))
    {
        XrSwapchainImageReleaseInfo ri = {};
        ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
        g_fn.releaseSwapchainImage(g_eyes[eye].swapchain, &ri);
        return false;
    }

    bool drew = false;
    if (index < g_eyes[eye].images.size() && g_eyes[eye].images[index] != nullptr)
    {
        ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* oldDsv = nullptr;
        D3D11_VIEWPORT oldVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        UINT oldVpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_PRIMITIVE_TOPOLOGY oldTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ID3D11InputLayout* oldLayout = nullptr;
        ID3D11VertexShader* oldVs = nullptr;
        ID3D11PixelShader* oldPs = nullptr;
        ID3D11ShaderResourceView* oldSrv = nullptr;
        ID3D11SamplerState* oldSampler = nullptr;
        ID3D11Buffer* oldPsCb = nullptr;
        ID3D11BlendState* oldBlend = nullptr;
        FLOAT oldBlendFactor[4] = {};
        UINT oldSampleMask = 0xffffffff;
        ID3D11DepthStencilState* oldDepth = nullptr;
        UINT oldStencilRef = 0;
        ID3D11RasterizerState* oldRasterizer = nullptr;
        D3D11_RECT oldScissor[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        UINT oldScissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

        g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
        g_context->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
        g_context->OMGetDepthStencilState(&oldDepth, &oldStencilRef);
        g_context->RSGetState(&oldRasterizer);
        g_context->RSGetViewports(&oldVpCount, oldVp);
        g_context->RSGetScissorRects(&oldScissorCount, oldScissor);
        g_context->IAGetPrimitiveTopology(&oldTopo);
        g_context->IAGetInputLayout(&oldLayout);
        g_context->VSGetShader(&oldVs, nullptr, nullptr);
        g_context->PSGetShader(&oldPs, nullptr, nullptr);
        g_context->PSGetShaderResources(0, 1, &oldSrv);
        g_context->PSGetSamplers(0, 1, &oldSampler);
        g_context->PSGetConstantBuffers(0, 1, &oldPsCb);

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(g_eyes[eye].width);
        vp.Height = static_cast<float>(g_eyes[eye].height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;

        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        // [DOWNCHAIN] bring a far-larger-than-eye source down to within one bilinear step FIRST, so
        // the fill draw below never minifies by more than 2x. No-op at every ratio <= 2x. Halving
        // preserves aspect, so the fill shader's UV math is unaffected either way.
        ID3D11ShaderResourceView* fillSrv = DownsampleWithin2x(g_eyes[eye].width, g_eyes[eye].height);
        g_context->OMSetBlendState(g_xrAtlasBlend, blendFactor, 0xffffffff);
        g_context->OMSetDepthStencilState(g_xrAtlasDepth, 0);
        g_context->ClearRenderTargetView(g_xrAtlasEyeRtv[eye], clear);
        g_context->OMSetRenderTargets(1, &g_xrAtlasEyeRtv[eye], nullptr);
        g_context->RSSetState(g_xrAtlasRasterizer);
        g_context->RSSetViewports(1, &vp);
        g_context->IASetInputLayout(nullptr);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_xrAtlasVs, nullptr, 0);
        g_context->PSSetShader(g_xrFillPs, nullptr, 0);
        g_context->PSSetShaderResources(0, 1, &fillSrv);
        g_context->PSSetSamplers(0, 1, &g_xrAtlasSampler);
        g_context->PSSetConstantBuffers(0, 1, &g_xrFillCb);
        g_context->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrv = nullptr;
        g_context->PSSetShaderResources(0, 1, &nullSrv);
        g_context->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
        g_context->OMSetDepthStencilState(oldDepth, oldStencilRef);
        g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
        g_context->RSSetState(oldRasterizer);
        g_context->RSSetViewports(oldVpCount, oldVp);
        g_context->RSSetScissorRects(oldScissorCount, oldScissor);
        g_context->IASetPrimitiveTopology(oldTopo);
        g_context->IASetInputLayout(oldLayout);
        g_context->VSSetShader(oldVs, nullptr, 0);
        g_context->PSSetShader(oldPs, nullptr, 0);
        g_context->PSSetShaderResources(0, 1, &oldSrv);
        g_context->PSSetSamplers(0, 1, &oldSampler);
        g_context->PSSetConstantBuffers(0, 1, &oldPsCb);

        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) if (oldRtv[i]) oldRtv[i]->Release();
        if (oldDsv) oldDsv->Release();
        if (oldLayout) oldLayout->Release();
        if (oldVs) oldVs->Release();
        if (oldPs) oldPs->Release();
        if (oldSrv) oldSrv->Release();
        if (oldSampler) oldSampler->Release();
        if (oldPsCb) oldPsCb->Release();
        if (oldBlend) oldBlend->Release();
        if (oldDepth) oldDepth->Release();
        if (oldRasterizer) oldRasterizer->Release();

        g_context->CopyResource(g_eyes[eye].images[index], g_xrAtlasEyeTex[eye]);
        drew = true;
    }

    XrSwapchainImageReleaseInfo ri = {};
    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(g_eyes[eye].swapchain, &ri);
    return drew;
}

bool BlitAtlasHalfToEye(int eye, int sourceHalf, float sourceVScale = 1.0f, float fillH = 1.0f, float fillV = 1.0f, float imageMoveY = 0.0f) noexcept
{
    if (eye < 0 || eye > 1 || sourceHalf < 0 || sourceHalf > 1 ||
        g_eyes[eye].swapchain == nullptr || g_context == nullptr || !g_xrAtlasBlitReady ||
        g_xrAtlasEyeTex[eye] == nullptr || g_xrAtlasEyeRtv[eye] == nullptr)
    {
        LogLine("[XRATLAS_BLIT] eye" + std::to_string(eye) +
                " sourceHalf=" + std::to_string(sourceHalf) + " precondition failed.");
        return false;
    }

    XrSwapchainImageAcquireInfo ai = {};
    ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    uint32_t index = 0;
    XrResult xr = g_fn.acquireSwapchainImage(g_eyes[eye].swapchain, &ai, &index);
    if (!XrSucceeded(xr))
    {
        LogLine("[XRATLAS_BLIT] eye" + std::to_string(eye) + " acquire failed: " + FmtXr(xr));
        return false;
    }

    XrSwapchainImageWaitInfo wi = {};
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE;
    wi.timeout = XR_INFINITE_DURATION_VALUE;
    xr = g_fn.waitSwapchainImage(g_eyes[eye].swapchain, &wi);
    if (!XrSucceeded(xr))
    {
        LogLine("[XRATLAS_BLIT] eye" + std::to_string(eye) + " wait failed: " + FmtXr(xr));
        XrSwapchainImageReleaseInfo ri = {};
        ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
        g_fn.releaseSwapchainImage(g_eyes[eye].swapchain, &ri);
        return false;
    }

    bool drew = false;
    if (index < g_eyes[eye].images.size() && g_eyes[eye].images[index] != nullptr)
    {
        ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* oldDsv = nullptr;
        D3D11_VIEWPORT oldVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        UINT oldVpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_PRIMITIVE_TOPOLOGY oldTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        ID3D11InputLayout* oldLayout = nullptr;
        ID3D11VertexShader* oldVs = nullptr;
        ID3D11PixelShader* oldPs = nullptr;
        ID3D11ShaderResourceView* oldSrv = nullptr;
        ID3D11SamplerState* oldSampler = nullptr;
        ID3D11Buffer* oldPsCb = nullptr;
        ID3D11BlendState* oldBlend = nullptr;
        FLOAT oldBlendFactor[4] = {};
        UINT oldSampleMask = 0xffffffff;
        ID3D11DepthStencilState* oldDepth = nullptr;
        UINT oldStencilRef = 0;
        ID3D11RasterizerState* oldRasterizer = nullptr;
        D3D11_RECT oldScissor[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        UINT oldScissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;

        g_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
        g_context->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
        g_context->OMGetDepthStencilState(&oldDepth, &oldStencilRef);
        g_context->RSGetState(&oldRasterizer);
        g_context->RSGetViewports(&oldVpCount, oldVp);
        g_context->RSGetScissorRects(&oldScissorCount, oldScissor);
        g_context->IAGetPrimitiveTopology(&oldTopo);
        g_context->IAGetInputLayout(&oldLayout);
        g_context->VSGetShader(&oldVs, nullptr, nullptr);
        g_context->PSGetShader(&oldPs, nullptr, nullptr);
        g_context->PSGetShaderResources(0, 1, &oldSrv);
        g_context->PSGetSamplers(0, 1, &oldSampler);
        g_context->PSGetConstantBuffers(0, 1, &oldPsCb);

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(g_eyes[eye].width);
        vp.Height = static_cast<float>(g_eyes[eye].height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;

        const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g_context->ClearRenderTargetView(g_xrAtlasEyeRtv[eye], clear);
        g_context->OMSetBlendState(g_xrAtlasBlend, blendFactor, 0xffffffff);
        g_context->OMSetDepthStencilState(g_xrAtlasDepth, 0);
        g_context->OMSetRenderTargets(1, &g_xrAtlasEyeRtv[eye], nullptr);
        g_context->RSSetState(g_xrAtlasRasterizer);
        g_context->RSSetViewports(1, &vp);
        g_context->IASetInputLayout(nullptr);
        g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_context->VSSetShader(g_xrAtlasVs, nullptr, 0);
        const bool fillActive = sourceVScale < 0.999f ||
                                std::fabs(fillH - 1.0f) > 0.002f ||
                                std::fabs(fillV - 1.0f) > 0.002f ||
                                std::fabs(imageMoveY) > 0.001f;
        if (fillActive && g_xrFillPs != nullptr && g_xrFillCb != nullptr)
        {
            float fill[8] = {};
            // CENTERED vertical band (2026-07-08 crosshair fix, pairs with the centered P1 world
            // viewport in render_hook): the sub-height world now sits in the MIDDLE of the half, so the
            // submit crop samples the middle band - world center, UI center and eye center coincide.
            float bandV = sourceVScale;
            if (bandV < 0.05f) bandV = 0.05f;
            if (bandV > 1.0f) bandV = 1.0f;
            BuildFillConstants(fill, sourceHalf == 0 ? 0.0f : 0.5f, 0.5f, sourceVScale, fillH, fillV, imageMoveY,
                               0.5f * (1.0f - bandV));
            g_context->UpdateSubresource(g_xrFillCb, 0, nullptr, fill, 0, 0);
            g_context->PSSetShader(g_xrFillPs, nullptr, 0);
            g_context->PSSetConstantBuffers(0, 1, &g_xrFillCb);
        }
        else
        {
            g_context->PSSetShader(g_xrAtlasPs[sourceHalf], nullptr, 0);
        }
        g_context->PSSetShaderResources(0, 1, &g_xrAtlasSrv);
        g_context->PSSetSamplers(0, 1, &g_xrAtlasSampler);
        g_context->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrv = nullptr;
        g_context->PSSetShaderResources(0, 1, &nullSrv);
        g_context->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
        g_context->OMSetDepthStencilState(oldDepth, oldStencilRef);
        g_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
        g_context->RSSetState(oldRasterizer);
        g_context->RSSetViewports(oldVpCount, oldVp);
        g_context->RSSetScissorRects(oldScissorCount, oldScissor);
        g_context->IASetPrimitiveTopology(oldTopo);
        g_context->IASetInputLayout(oldLayout);
        g_context->VSSetShader(oldVs, nullptr, 0);
        g_context->PSSetShader(oldPs, nullptr, 0);
        g_context->PSSetShaderResources(0, 1, &oldSrv);
        g_context->PSSetSamplers(0, 1, &oldSampler);
        g_context->PSSetConstantBuffers(0, 1, &oldPsCb);

        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) if (oldRtv[i]) oldRtv[i]->Release();
        if (oldDsv) oldDsv->Release();
        if (oldLayout) oldLayout->Release();
        if (oldVs) oldVs->Release();
        if (oldPs) oldPs->Release();
        if (oldSrv) oldSrv->Release();
        if (oldSampler) oldSampler->Release();
        if (oldPsCb) oldPsCb->Release();
        if (oldBlend) oldBlend->Release();
        if (oldDepth) oldDepth->Release();
        if (oldRasterizer) oldRasterizer->Release();

        g_context->CopyResource(g_eyes[eye].images[index], g_xrAtlasEyeTex[eye]);
        drew = true;
    }
    else
    {
        LogLine("[XRATLAS_BLIT] eye" + std::to_string(eye) + " invalid image index=" +
                std::to_string(index) + " images=" + std::to_string(g_eyes[eye].images.size()));
    }

    XrSwapchainImageReleaseInfo ri = {};
    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(g_eyes[eye].swapchain, &ri);
    return drew;
}

bool BlitAtlasHalfToEye(int eye) noexcept
{
    return BlitAtlasHalfToEye(eye, eye);
}

bool ClearEyeDiagnostic(int eye, const float color[4]) noexcept
{
    if (eye < 0 || eye > 1 || g_eyes[eye].swapchain == nullptr || g_context == nullptr ||
        g_xrAtlasEyeTex[eye] == nullptr || g_xrAtlasEyeRtv[eye] == nullptr) return false;

    XrSwapchainImageAcquireInfo ai = {};
    ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    uint32_t index = 0;
    XrResult xr = g_fn.acquireSwapchainImage(g_eyes[eye].swapchain, &ai, &index);
    if (!XrSucceeded(xr))
    {
        LogLine("[XRATLAS_BLIT] diagnostic eye" + std::to_string(eye) + " acquire failed: " + FmtXr(xr));
        return false;
    }

    XrSwapchainImageWaitInfo wi = {};
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE;
    wi.timeout = XR_INFINITE_DURATION_VALUE;
    xr = g_fn.waitSwapchainImage(g_eyes[eye].swapchain, &wi);
    if (!XrSucceeded(xr))
    {
        LogLine("[XRATLAS_BLIT] diagnostic eye" + std::to_string(eye) + " wait failed: " + FmtXr(xr));
        XrSwapchainImageReleaseInfo ri = {};
        ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
        g_fn.releaseSwapchainImage(g_eyes[eye].swapchain, &ri);
        return false;
    }

    bool cleared = false;
    if (index < g_eyes[eye].images.size() && g_eyes[eye].images[index] != nullptr)
    {
        g_context->ClearRenderTargetView(g_xrAtlasEyeRtv[eye], color);
        g_context->CopyResource(g_eyes[eye].images[index], g_xrAtlasEyeTex[eye]);
        cleared = true;
    }

    XrSwapchainImageReleaseInfo ri = {};
    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(g_eyes[eye].swapchain, &ri);
    return cleared;
}

bool CopyCamCb96AtlasToEyes(ID3D11Texture2D* backBuffer) noexcept
{
    if (!XrAtlasSubmitEnabled()) return false;
    static bool s_logged = false;
    static bool s_failLogged = false;
    static bool s_markerLogged = false;
    if (!s_markerLogged)
    {
        s_markerLogged = true;
        LogLine("[XRATLAS_MARKER] copy xr=" +
                std::to_string(MarkerEnabled(L"MELEVR_ENABLE_CAMCB96_XR_ATLAS.txt") ? 1 : 0) +
                " liveAtlas=" +
                std::to_string(MarkerEnabled(L"MELEVR_ENABLE_CAMCB96_ATLAS.txt") ? 1 : 0) +
                " enabled=1");
    }
    const float leftFail[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    const float rightFail[4] = {0.0f, 1.0f, 1.0f, 1.0f};
    if (backBuffer == nullptr || !EnsureXrAtlasBlitResources(backBuffer))
    {
        ClearEyeDiagnostic(0, leftFail);
        ClearEyeDiagnostic(1, rightFail);
        if (!s_failLogged)
        {
            s_failLogged = true;
            LogLine("[XRATLAS_BLIT] forced diagnostic colors because atlas blit init/source failed; no whole-atlas fallback.");
        }
        return true;
    }

    g_context->CopyResource(g_xrAtlasSource, backBuffer);

    const bool copiedL = BlitAtlasHalfToEye(0);
    const bool copiedR = BlitAtlasHalfToEye(1);
    if (!(copiedL && copiedR))
    {
        ClearEyeDiagnostic(0, leftFail);
        ClearEyeDiagnostic(1, rightFail);
        if (!s_failLogged)
        {
            s_failLogged = true;
            LogLine("[XRATLAS_BLIT] forced diagnostic colors because atlas blit draw failed; no whole-atlas fallback.");
        }
        return true;
    }

    if (!s_logged)
    {
        s_logged = true;
        LogLine("[XRATLAS_BLIT] submitted CAMCB96 live atlas halves to full OpenXR eyes: L=left half R=right half src=" +
                std::to_string(g_bbWidth) + "x" + std::to_string(g_bbHeight) +
                " eye=" + std::to_string(g_eyes[0].width) + "x" + std::to_string(g_eyes[0].height) +
                " copied=" + std::to_string((copiedL && copiedR) ? 1 : 0));
    }
    // DIAGNOSTIC: show the LEFT eye's POST-CROP texture on the flat monitor. Clean stretched single view =
    // crop works (bug is downstream/submit). Full side-by-side = the crop shader itself is broken.
    // (Headset eyes already submitted above; this only changes which flat backbuffer is read.)
    if (g_xrAtlasEyeTex[0] != nullptr) g_context->CopyResource(backBuffer, g_xrAtlasEyeTex[0]);
    return true;
}

// ---- [BANDPROBE] head-locked dark-panel hunt (2026-07-12) ------------------------------------------
// Samples two 2px-wide full-height columns of the RAW game backbuffer (the two eye centers in SBS)
// every ~2s, finds the strongest horizontal light/dark edge, and logs it next to the injected head
// pitch + which rotation mechanism was live. If the "panel" is baked into the game's own render, this
// prints its exact row and its correlation with pitch across a whole session; if the backbuffer stays
// clean while the headset shows the band, the artifact is compositor-side. Read-only: one small
// CopySubresourceRegion + Map every 120 presents. Runs before any paints/warps touch the frame.
ID3D11Texture2D* g_bandStaging = nullptr;
UINT g_bandStagingH = 0;
DXGI_FORMAT g_bandStagingFmt = DXGI_FORMAT_UNKNOWN;

struct BandColumnStats
{
    float edgePct = 0.0f;    // strongest horizontal edge, % of backbuffer height from the top
    float above = 0.0f;      // mean luminance (0..1) in the window just above that edge
    float below = 0.0f;      // mean luminance just below it
    float darkPct = 0.0f;    // fraction of sampled rows darker than 0.06
};

// Per-row luminance of one 2px column (any 4-byte RGBA/BGRA order), bucketed over rows 5%..95% of the
// frame (skips the P1 stereo letterbox strips at 2.8%/97.2% so they can't fake the edge).
void BandProbeAnalyzeColumn(const BYTE* data, UINT rowPitch, UINT height, UINT colByteOffset,
                            BandColumnStats* out) noexcept
{
    constexpr int kBuckets = 128;
    float lum[kBuckets] = {};
    const UINT y0 = height / 20;            // 5%
    const UINT y1 = height - height / 20;   // 95%
    const UINT span = (y1 > y0) ? (y1 - y0) : 1;
    for (int b = 0; b < kBuckets; ++b)
    {
        const UINT rowStart = y0 + static_cast<UINT>((static_cast<uint64_t>(span) * b) / kBuckets);
        const UINT rowEnd = y0 + static_cast<UINT>((static_cast<uint64_t>(span) * (b + 1)) / kBuckets);
        float sum = 0.0f;
        int n = 0;
        for (UINT y = rowStart; y < rowEnd; y += 4)   // every 4th row is plenty
        {
            const BYTE* px = data + static_cast<size_t>(y) * rowPitch + colByteOffset;
            // 2 pixels x first 3 channel bytes; order-agnostic (R/B swap doesn't change the sum)
            sum += static_cast<float>(px[0] + px[1] + px[2] + px[4] + px[5] + px[6]) / (6.0f * 255.0f);
            ++n;
        }
        lum[b] = (n > 0) ? (sum / static_cast<float>(n)) : 0.0f;
    }

    constexpr int kWin = 5;
    float bestScore = -1.0f;
    int bestI = kWin;
    for (int i = kWin; i <= kBuckets - kWin; ++i)
    {
        float a = 0.0f, bl = 0.0f;
        for (int k = 1; k <= kWin; ++k) { a += lum[i - k]; bl += lum[i + k - 1]; }
        a /= kWin; bl /= kWin;
        float score = a - bl;
        if (score < 0.0f) score = -score;
        if (score > bestScore)
        {
            bestScore = score;
            bestI = i;
            out->above = a;
            out->below = bl;
        }
    }
    out->edgePct = 100.0f * (static_cast<float>(y0) + static_cast<float>(span) * bestI / kBuckets) /
                   static_cast<float>(height);
    int dark = 0;
    for (int b = 0; b < kBuckets; ++b)
        if (lum[b] < 0.06f) ++dark;
    out->darkPct = 100.0f * static_cast<float>(dark) / kBuckets;
}

void BandProbeTick(ID3D11Texture2D* backBuffer, const D3D11_TEXTURE2D_DESC& bb,
                   bool stereoActive, bool aerActive, bool forceFlatCine, bool menuOpen,
                   float yawDeg, float pitchDeg, int weaponMode,
                   bool headLookApplied, bool headAimApplied) noexcept
{
    static uint64_t s_tick = 0;
    if ((s_tick++ % 120ull) != 20ull) return;
    if (g_device == nullptr || g_context == nullptr || backBuffer == nullptr) return;

    switch (bb.Format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        break;
    default:
    {
        static bool s_fmtLogged = false;
        if (!s_fmtLogged)
        {
            s_fmtLogged = true;
            LogLine("[BANDPROBE] backbuffer format " + std::to_string(static_cast<int>(bb.Format)) +
                    " not 8-bit RGBA (HDR on?) - probe idle");
        }
        return;
    }
    }
    if (bb.Width < 64 || bb.Height < 64) return;

    if (g_bandStaging == nullptr || g_bandStagingH != bb.Height || g_bandStagingFmt != bb.Format)
    {
        if (g_bandStaging != nullptr) { g_bandStaging->Release(); g_bandStaging = nullptr; }
        D3D11_TEXTURE2D_DESC sd = {};
        sd.Width = 4;   // two 2px columns side by side
        sd.Height = bb.Height;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = bb.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_device->CreateTexture2D(&sd, nullptr, &g_bandStaging)) || g_bandStaging == nullptr)
        {
            g_bandStaging = nullptr;
            return;
        }
        g_bandStagingH = bb.Height;
        g_bandStagingFmt = bb.Format;
    }

    const UINT xL = bb.Width / 4;        // left-eye center in SBS (frame-quarter in mono - still content)
    const UINT xR = (bb.Width * 3) / 4;  // right-eye center
    D3D11_BOX box = {};
    box.top = 0;
    box.bottom = bb.Height;
    box.front = 0;
    box.back = 1;
    box.left = xL;
    box.right = xL + 2;
    g_context->CopySubresourceRegion(g_bandStaging, 0, 0, 0, 0, backBuffer, 0, &box);
    box.left = xR;
    box.right = xR + 2;
    g_context->CopySubresourceRegion(g_bandStaging, 0, 2, 0, 0, backBuffer, 0, &box);

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(g_context->Map(g_bandStaging, 0, D3D11_MAP_READ, 0, &mapped)) || mapped.pData == nullptr)
        return;
    BandColumnStats l = {}, r = {};
    BandProbeAnalyzeColumn(static_cast<const BYTE*>(mapped.pData), mapped.RowPitch, bb.Height, 0, &l);
    BandProbeAnalyzeColumn(static_cast<const BYTE*>(mapped.pData), mapped.RowPitch, bb.Height, 8, &r);
    g_context->Unmap(g_bandStaging, 0);

    char line[512] = {};
    sprintf_s(line,
              "[BANDPROBE] L(edge=%.1f%% above=%.3f below=%.3f dark=%.0f%%) "
              "R(edge=%.1f%% above=%.3f below=%.3f dark=%.0f%%) "
              "pitch=%.1f yaw=%.1f headLook=%d headAim=%d weapon=%d stereo=%d aer=%d flatCine=%d menu=%d bb=%ux%u",
              l.edgePct, l.above, l.below, l.darkPct,
              r.edgePct, r.above, r.below, r.darkPct,
              pitchDeg, yawDeg, headLookApplied ? 1 : 0, headAimApplied ? 1 : 0, weaponMode,
              stereoActive ? 1 : 0, aerActive ? 1 : 0, forceFlatCine ? 1 : 0, menuOpen ? 1 : 0,
              bb.Width, bb.Height);
    LogLine(line);
}

// SHARP STEREO SUBMIT (ME2 parity): copy each SBS half 1:1 into the half-size stereo eye swapchains via
// CopySubresourceRegion (no shader, no resample, no magnify) -> pixel-sharp. The render is already widened to
// the headset FOV (PostProcessPerspectiveFsv->ApplyTargetFovOverride), so a straight copy fills the headset.
bool CopySbsHalvesToStereoEyes(ID3D11Texture2D* backBuffer, bool swapEyes) noexcept
{
    if (!g_stereoEyesReady || backBuffer == nullptr || g_context == nullptr) return false;
    D3D11_TEXTURE2D_DESC bd = {};
    backBuffer->GetDesc(&bd);
    const uint32_t halfW = bd.Width / 2;
    if (halfW == 0 || bd.Height == 0) return false;
    bool ok = true;
    for (int eye = 0; eye < 2; ++eye)
    {
        EyeSwapchain& sw = g_stereoEyes[eye];
        if (sw.swapchain == nullptr) { ok = false; continue; }
        const int srcHalf = swapEyes ? (1 - eye) : eye;   // eye0=left half, eye1=right half (swap flips)
        XrSwapchainImageAcquireInfo ai = {}; ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
        uint32_t idx = 0;
        if (!XrSucceeded(g_fn.acquireSwapchainImage(sw.swapchain, &ai, &idx))) { ok = false; continue; }
        XrSwapchainImageWaitInfo wi = {}; wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE; wi.timeout = XR_INFINITE_DURATION_VALUE;
        if (XrSucceeded(g_fn.waitSwapchainImage(sw.swapchain, &wi)) && idx < sw.images.size() && sw.images[idx] != nullptr)
        {
            D3D11_BOX box = {};
            box.left = static_cast<UINT>(srcHalf) * halfW; box.right = box.left + halfW;
            box.top = 0; box.bottom = bd.Height; box.front = 0; box.back = 1;
            g_context->CopySubresourceRegion(sw.images[idx], 0, 0, 0, 0, backBuffer, 0, &box);
        }
        else ok = false;
        XrSwapchainImageReleaseInfo ri = {}; ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
        g_fn.releaseSwapchainImage(sw.swapchain, &ri);
    }
    return ok;
}

bool CopySbsBackbufferToEyes(ID3D11Texture2D* backBuffer, bool swapEyes, float sourceVScale) noexcept
{
    static bool s_logged = false;
    static bool s_failLogged = false;
    if (backBuffer == nullptr || !EnsureXrAtlasBlitResources(backBuffer))
    {
        if (!s_failLogged)
        {
            s_failLogged = true;
            LogLine("[P1STEREO_SUBMIT] failed to initialize SBS atlas blit resources; falling back.");
        }
        return false;
    }

    g_context->CopyResource(g_xrAtlasSource, backBuffer);
    const int leftHalf = swapEyes ? 1 : 0;
    const int rightHalf = swapEyes ? 0 : 1;
    const bool copiedL = BlitAtlasHalfToEye(0, leftHalf, sourceVScale, 1.0f, 1.0f, 0.0f);
    const bool copiedR = BlitAtlasHalfToEye(1, rightHalf, sourceVScale, 1.0f, 1.0f, 0.0f);
    if (!(copiedL && copiedR))
    {
        if (!s_failLogged)
        {
            s_failLogged = true;
            LogLine("[P1STEREO_SUBMIT] SBS half blit failed copiedL=" + std::to_string(copiedL ? 1 : 0) +
                    " copiedR=" + std::to_string(copiedR ? 1 : 0));
        }
        return false;
    }

    if (!s_logged)
    {
        s_logged = true;
        LogLine("[P1STEREO_SUBMIT] submitted game SBS halves to OpenXR eyes swap=" + std::to_string(swapEyes ? 1 : 0) +
                " src=" + std::to_string(g_bbWidth) + "x" + std::to_string(g_bbHeight) +
                " eye=" + std::to_string(g_eyes[0].width) + "x" + std::to_string(g_eyes[0].height) +
                " sourceVScale=" + std::to_string(sourceVScale));
    }
    return true;
}

void CopyGameFrameToEye(int eye, ID3D11Texture2D* backBuffer) noexcept
{
    CopyTextureToEye(eye, backBuffer);
}

bool CopyStereoPassCaptureToEyes() noexcept
{
    ID3D11Texture2D* left = nullptr;
    ID3D11Texture2D* right = nullptr;
    unsigned long long pairCount = 0;
    unsigned long long pairArm = 0;
    const bool havePair = MELEVR::D3DCapture::GetStereoPassPair(&left, &right, &pairCount, &pairArm);
    if (!havePair || left == nullptr || right == nullptr)
    {
        static unsigned long long s_noPairLogs = 0;
        ++s_noPairLogs;
        if (s_noPairLogs <= 8 || (s_noPairLogs % 120) == 1)
        {
            LogLine("[SYNCSTEREO] no captured stereo pair available yet: havePair=" +
                    std::to_string(havePair ? 1 : 0) +
                    " pairCount=" + std::to_string(pairCount) +
                    " left=" + std::to_string(left != nullptr ? 1 : 0) +
                    " right=" + std::to_string(right != nullptr ? 1 : 0));
        }
        if (left) left->Release();
        if (right) right->Release();
        return false;
    }

    static unsigned long long s_lastSubmittedPairCount = 0;
    static int s_holdFramesRemaining = 0;
    static unsigned long long s_streamedPairs = 0;
    // STARVATION VALVE (2026-07-17): was 3600 (a full MINUTE of re-showing the same stale pair at
    // 60fps). Every bug in the freeze family - frozen convos, the Eden Prime bomb disarm, the ESC
    // menu - was this hold amplifying a stopped replay into a hard-frozen headset while the monitor
    // kept playing. 45 presents (~0.75s) still bridges real hitches and load stalls; past that the
    // submit falls through to the live mono path (live backbuffer, head-tracked), so a stopped SFR
    // replay can only ever cost depth for a moment, never freeze the image.
    constexpr int kStereoHoldFrames = 45;
    if (pairCount == s_lastSubmittedPairCount)
    {
        g_sfrPairFreshThisFrame = false;   // [FREEZETAG] held pair: same pixels as last present
        if (s_holdFramesRemaining <= 0)
        {
            left->Release();
            right->Release();
            return false;
        }
    }
    else
    {
        g_sfrPairFreshThisFrame = true;    // [FREEZETAG] new pair this present
        g_sfrPairArmThisFrame = static_cast<uint64_t>(pairArm);
        s_lastSubmittedPairCount = pairCount;
        s_holdFramesRemaining = kStereoHoldFrames;
        ++s_streamedPairs;
        if (s_streamedPairs == 1 || (s_streamedPairs % 20) == 0)
        {
            LogLine("[SYNCSTEREO] begin finite captured Draw hold: pass0->L count=" +
                    std::to_string(pairCount) + " pass1->R count=" + std::to_string(pairCount) +
                    " frames=" + std::to_string(kStereoHoldFrames) +
                    " streamedPairs=" + std::to_string(s_streamedPairs));
        }
    }

    // Stereo 2: the captured passes are FULL 16:9 frames (one whole eye's render each, like AER/DIBR).
    // CopyTextureToEye applies the cinematic aspect-fit that SQUASHES a full frame into the squarish
    // eye - the exact "DIBR squashed" bug (2026-07-07), and 2026-07-14's "shepard is extremely
    // squashed" in Stereo 2. Full-frame copy maps whole->whole, aspect-correct.
    {
        const auto& sfrCfg = MELEVR::Config::Get();
        const int eyeForPass0 = sfrCfg.sfr2SwapEyes ? 1 : 0;
        const int eyeForPass1 = 1 - eyeForPass0;
        // [SFRCONV FINAL 2026-07-20] convergence applied HERE, at submit - the whole finished image
        // (world + UI together) shifts by conv/2 UV, pixel-identical to a render-side NDC shift for
        // the world but structurally incapable of splitting the UI (both passes rendered unshifted ->
        // HUD copies coincide; the UI rides to the convergence plane with the world).
        // [SWAPME2] the sign is bolted to the PASS, exactly like ME2 (calcview: t_replay ? -conv :
        // +conv, no swap term): pass0 always carries the left-signed shift, pass1 the right-signed
        // one. So with swap-eyes ON (which flips cameras + routing, keeping depth matched), the
        // convergence plane INVERTS - that inversion IS the visible effect ME2's swap toggle has.
        // Keying this on the destination eye instead made ME1's swap a perfect no-op ("this isn't it").
        const float convHalfU = sfrCfg.sfr2Convergence * 0.5f;
        // [RELIEF_SFR2] depth pop, AER-style: warp each full-frame pass by the published depth before
        // the eye copy (+- eyeSign per DESTINATION eye adds the pop on top of the real separation).
        // RenderReliefFull reuses one shared output texture, so warp+copy strictly one eye at a time.
        const MELEVR::Config::ReliefParams sfrRp = MELEVR::Config::ActiveRelief(sfrCfg);
        const bool sfrRelief = sfrCfg.reliefEnabled &&
                               (sfrRp.strength > 0.0001f || sfrRp.darkStrength > 0.0001f ||
                                sfrRp.unsharpStrength > 0.0001f) &&
                               MELEVR::D3DCapture::IsReliefDepthReady();
        ID3D11Texture2D* src0 = left;
        if (sfrRelief)
        {
            ID3D11Texture2D* warped = MELEVR::D3DCapture::RenderReliefFull(left, eyeForPass0 == 0 ? 1.0f : -1.0f);
            if (warped != nullptr) src0 = warped;
        }
        CopyTextureToEyeFullFrame(eyeForPass0, src0, -convHalfU);   // pass0 = left-signed conv (ME2 pass-fixed)
        ID3D11Texture2D* src1 = right;
        if (sfrRelief)
        {
            ID3D11Texture2D* warped = MELEVR::D3DCapture::RenderReliefFull(right, eyeForPass1 == 0 ? 1.0f : -1.0f);
            if (warped != nullptr) src1 = warped;
        }
        CopyTextureToEyeFullFrame(eyeForPass1, src1, +convHalfU);   // pass1 = right-signed conv (ME2 pass-fixed)
    }
    --s_holdFramesRemaining;

    if (!g_stereoPassSubmitLogged)
    {
        g_stereoPassSubmitLogged = true;
        LogLine("[SYNCSTEREO] submitted captured Draw passes to OpenXR eyes: pass0->L count=" +
                std::to_string(pairCount) + " pass1->R count=" + std::to_string(pairCount));
    }
    if (s_holdFramesRemaining == 0)
    {
        LogLine("[SYNCSTEREO] finite captured Draw hold complete; falling back to live mono path.");
    }
    left->Release();
    right->Release();
    return true;
}

void ReleaseTexture(ID3D11Texture2D*& t) noexcept
{
    if (t != nullptr)
    {
        t->Release();
        t = nullptr;
    }
}


// Copy the menu's ImGui texture into the menu quad swapchain (acquire/wait/copy/release).
bool CopyMenuToSwapchain(ID3D11Texture2D* src) noexcept
{
    if (g_menuSwapchain == nullptr || g_context == nullptr || src == nullptr || g_menuImages.empty()) return false;
    XrSwapchainImageAcquireInfo ai = {};
    ai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO_VALUE;
    uint32_t index = 0;
    if (!XrSucceeded(g_fn.acquireSwapchainImage(g_menuSwapchain, &ai, &index))) return false;
    XrSwapchainImageWaitInfo wi = {};
    wi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO_VALUE;
    wi.timeout = XR_INFINITE_DURATION_VALUE;
    g_fn.waitSwapchainImage(g_menuSwapchain, &wi);
    if (index < g_menuImages.size() && g_menuImages[index] != nullptr)
    {
        g_context->CopyResource(g_menuImages[index], src);
    }
    XrSwapchainImageReleaseInfo ri = {};
    ri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO_VALUE;
    g_fn.releaseSwapchainImage(g_menuSwapchain, &ri);
    return true;
}

// Head orientation -> yaw/pitch/roll degrees (OpenXR: Y-up, -Z forward, right-handed). Same convention as
// the parked build, so the proven free-look signs carry over.
void HeadEulerDegrees(const XrQuaternionf& q, float& yawDeg, float& pitchDeg, float& rollDeg) noexcept
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float fx = -2.0f * (x * z + w * y);            // forward = R*(0,0,-1)
    const float fy = -2.0f * (y * z - w * x);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    const float ux = 2.0f * (x * y - w * z);             // up = R*(0,1,0)
    const float uy = 1.0f - 2.0f * (x * x + z * z);
    constexpr float kRad2Deg = 57.2957795f;
    const float clampedFy = fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy);
    yawDeg = std::atan2(fx, -fz) * kRad2Deg;
    pitchDeg = std::asin(clampedFy) * kRad2Deg;
    rollDeg = std::atan2(-ux, uy) * kRad2Deg;
}

float WrapDeg(float d) noexcept
{
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}


// Return the head orientation with ROLL removed: same forward direction, but the right-vector forced
// horizontal (world XZ plane) so the horizon stays level. Built geometrically (forward + world-up ->
// basis -> quaternion) - NOT euler-roll-zero, which leaves a tilt on diagonal looks. The game renders
// upright (no roll) and every submission is tagged with this; the compositor then keeps the world level as the head tilts.
XrQuaternionf QuatRemoveRoll(const XrQuaternionf& q) noexcept
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    float fx = -2.0f * (x * z + w * y);
    float fy = -2.0f * (y * z - w * x);
    float fz = -(1.0f - 2.0f * (x * x + y * y));
    float bx = -fx, by = -fy, bz = -fz;                  // camera local +Z in world
    const float bl = std::sqrt(bx * bx + by * by + bz * bz);
    if (bl < 1e-6f) return q;
    bx /= bl; by /= bl; bz /= bl;
    float rx = bz, ry = 0.0f, rz = -bx;                  // right = cross(worldUp,(b)) -> horizontal
    const float rl = std::sqrt(rx * rx + rz * rz);
    if (rl < 1e-4f) return q;                            // looking near-straight up/down: keep original
    rx /= rl; rz /= rl;
    const float ux = by * rz - bz * ry;                  // up = cross(b, right)
    const float uy = bz * rx - bx * rz;
    const float uz = bx * ry - by * rx;
    // Rotation matrix R, columns X=right, Y=up, Z=b. R[row][col].
    const float R00 = rx, R01 = ux, R02 = bx;
    const float R10 = ry, R11 = uy, R12 = by;
    const float R20 = rz, R21 = uz, R22 = bz;
    XrQuaternionf out{};
    const float tr = R00 + R11 + R22;
    if (tr > 0.0f)
    {
        float s = std::sqrt(tr + 1.0f) * 2.0f;
        out.w = 0.25f * s; out.x = (R21 - R12) / s; out.y = (R02 - R20) / s; out.z = (R10 - R01) / s;
    }
    else if (R00 > R11 && R00 > R22)
    {
        float s = std::sqrt(1.0f + R00 - R11 - R22) * 2.0f;
        out.w = (R21 - R12) / s; out.x = 0.25f * s; out.y = (R01 + R10) / s; out.z = (R02 + R20) / s;
    }
    else if (R11 > R22)
    {
        float s = std::sqrt(1.0f + R11 - R00 - R22) * 2.0f;
        out.w = (R02 - R20) / s; out.x = (R01 + R10) / s; out.y = 0.25f * s; out.z = (R12 + R21) / s;
    }
    else
    {
        float s = std::sqrt(1.0f + R22 - R00 - R11) * 2.0f;
        out.w = (R10 - R01) / s; out.x = (R02 + R20) / s; out.y = (R12 + R21) / s; out.z = 0.25f * s;
    }
    const float ql = std::sqrt(out.x * out.x + out.y * out.y + out.z * out.z + out.w * out.w);
    if (ql > 1e-6f) { out.x /= ql; out.y /= ql; out.z /= ql; out.w /= ql; }
    return out;
}

constexpr float kDegToUU = 65536.0f / 360.0f;

// Convert the explore/head-look Euler expression to the render-hook units. This
// is deliberately shared by ordinary explore look and controller aim: with a
// controller ray driving ControlRotation, the rendered view must still follow
// the HMD, not be pinned at zero. No game-memory write occurs here.
struct HeadLookUU
{
    int32_t yaw = 0;
    int32_t pitch = 0;
};

HeadLookUU HeadLookFromAngles(float yawDeg, float pitchDeg,
                              const MELEVR::Config::VrConfig& cfg) noexcept
{
    const float yawSign = cfg.invertLookYaw ? +1.0f : -1.0f;
    const float pitchSign = cfg.invertLookPitch ? -1.0f : +1.0f;
    float dPitchDeg = pitchSign * pitchDeg * cfg.lookSensitivity;
    if (dPitchDeg > 85.0f) dPitchDeg = 85.0f;
    if (dPitchDeg < -85.0f) dPitchDeg = -85.0f;
    return {
        static_cast<int32_t>(yawSign * yawDeg * cfg.lookSensitivity * kDegToUU),
        static_cast<int32_t>(dPitchDeg * kDegToUU)
    };
}

// Normalized-lerp between two quaternions (shortest arc). Per-frame head deltas are tiny; nlerp is plenty.
XrQuaternionf NlerpQuat(const XrQuaternionf& a, XrQuaternionf b, float t) noexcept
{
    const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
    XrQuaternionf q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                    a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (n > 1e-6f) { q.x /= n; q.y /= n; q.z /= n; q.w /= n; }
    return q;
}

// Angular distance between two quaternions in degrees (for the speed-adaptive smoothing follow).
float QuatAngleDeg(const XrQuaternionf& a, const XrQuaternionf& b) noexcept
{
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (dot < 0.0f) dot = -dot;
    if (dot > 1.0f) dot = 1.0f;
    return 2.0f * std::acos(dot) * 57.29578f;
}

// Locate both eye views against a given reference space. Returns true (with out[2] filled) if valid.
bool LocateViewsIn(XrSpace space, XrTime displayTime, XrView out[2]) noexcept
{
    XrViewLocateInfo vli = {};
    vli.type = XR_TYPE_VIEW_LOCATE_INFO_VALUE;
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_VALUE;
    vli.displayTime = displayTime;
    vli.space = space;
    XrViewState vs = {};
    vs.type = XR_TYPE_VIEW_STATE_VALUE;
    XrView views[2] = {};
    views[0].type = XR_TYPE_VIEW_VALUE;
    views[1].type = XR_TYPE_VIEW_VALUE;
    uint32_t n = 0;
    const XrResult r = g_fn.locateViews(g_session, &vli, &vs, 2, &n, views);
    const bool ok = XrSucceeded(r) && n >= 2 &&
                    (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT_VALUE) != 0;
    if (ok) { out[0] = views[0]; out[1] = views[1]; }
    return ok;
}

// Locate the head against the re-origined app space and stash it for the projection submit.
bool LocateHead(XrTime displayTime) noexcept
{
    XrView views[2] = {};
    if (LocateViewsIn(g_appSpace, displayTime, views))
    {
        g_subViews[0] = views[0];
        g_subViews[1] = views[1];
        g_subViewsValid = true;
        return true;
    }
    g_subViewsValid = false;
    return false;
}

// Yaw-only quaternion (level - no pitch/roll), matching HeadEulerDegrees' yaw convention.
XrQuaternionf EulerYawToQuat(float yawDeg) noexcept
{
    const float h = yawDeg * (3.14159265f / 180.0f) * 0.5f;
    XrQuaternionf q{};
    q.x = 0.0f; q.y = -std::sin(h); q.z = 0.0f; q.w = std::cos(h);
    return q;
}

// Real recenter: re-origin g_appSpace to the current head's yaw + position (kept level), read from the
// never-moved base space. After this, "head forward" == g_appSpace forward, so locate + submit are centered.
bool DoRecenter(XrTime displayTime) noexcept
{
    XrView baseViews[2] = {};
    if (!LocateViewsIn(g_baseSpace, displayTime, baseViews)) return false;  // tracking not up yet - retry next frame

    float yawDeg = 0.0f, pitchDeg = 0.0f, rollDeg = 0.0f;
    HeadEulerDegrees(baseViews[0].pose.orientation, yawDeg, pitchDeg, rollDeg);

    XrReferenceSpaceCreateInfo rsci = {};
    rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO_VALUE;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL_VALUE;
    rsci.poseInReferenceSpace.orientation = EulerYawToQuat(yawDeg);   // face current yaw, level
    rsci.poseInReferenceSpace.position.x = (baseViews[0].pose.position.x + baseViews[1].pose.position.x) * 0.5f;
    rsci.poseInReferenceSpace.position.y = (baseViews[0].pose.position.y + baseViews[1].pose.position.y) * 0.5f;
    rsci.poseInReferenceSpace.position.z = (baseViews[0].pose.position.z + baseViews[1].pose.position.z) * 0.5f;

    XrSpace newSpace = nullptr;
    if (!XrSucceeded(g_fn.createReferenceSpace(g_session, &rsci, &newSpace)) || newSpace == nullptr) return false;
    if (g_appSpace != nullptr) g_fn.destroySpace(g_appSpace);
    g_appSpace = newSpace;
    LogLine("[M1] recenter - app space re-origined to head yaw=" + std::to_string(static_cast<int>(yawDeg)));
    return true;
}

// Treat a raw FRotator component as a signed angle near zero (the engine stores a slightly-down pitch as a
// large unsigned value; normalize before clamping).
int32_t NormalizeAngleUU(int32_t uu) noexcept
{
    uu &= 0xFFFF;
    if (uu >= 32768) uu -= 65536;
    return uu;
}


// ================= Head-aim driver (the ONE live game write; restored 2026-07-03) ===========================
// Core additive-with-stick model ported from bdc3c4c's DriveCameraWithHead, minus everything that needed the
// dead weapon-mode reads (TightAim edge-hold, transition suppress, manual-rebase, probes). On activation the
// head reference is latched; each frame last frame's injection is stripped off ControlRotation (leaving the raw
// stick), the new head delta is added, and it's written back. Deactivation subtracts the applied delta ONCE - but only if
// the controller is still live AND unchanged; on a controller transition all state is abandoned and nothing is written.
// Present-thread only; every write is preceded THAT FRAME by EnsureController + ControllerStable in RunFrame.

bool g_headAimActive = false;
float g_headRefYawDeg = 0.0f;
float g_headRefPitchDeg = 0.0f;
int32_t g_appliedHeadYawUU = 0;
int32_t g_appliedHeadPitchUU = 0;
// [AIMSEED v2] look->aim handoff RAMP (2026-07-16 round 3). The v1 one-shot seed stepped ControlRotation
// to the gaze in a single frame; the VIEW stayed glued (render offset zeroed the same frame) and the
// crosshair (drawn at ControlRotation) was right - but the pawn's ANIMATED weapon aim (torso/arm blend,
// what the fire trace actually uses) only follows CR *changes* per frame: it ate one partial step, the
// writes went quiet (head still = no new deltas), and it froze short -> bullets off from the crosshair
// until the head moves again. Fix: transfer the offset GRADUALLY - each frame move ~25% (min
// 150 UU) of the remainder from the render-side look offset into the CR injection; the two halves always
// sum to the full offset so the view never moves, while the game's aim blend gets a continuous signal it
// tracks all the way in (~0.3s for a big offset). rem = still rendered look-side; done = already in CR.
int32_t g_seedRemYawUU = 0;
int32_t g_seedRemPitchUU = 0;
int32_t g_seedDoneYawUU = 0;
int32_t g_seedDonePitchUU = 0;

// Deactivation/restore. restoreWrite = caller verified the controller is live (EnsureController) AND unchanged
// (ControllerStable) this frame; anything else clears state WITHOUT touching game memory (never write stale).
void ReleaseHeadAim(bool restoreWrite) noexcept
{
    if (g_headAimActive && restoreWrite && (g_appliedHeadYawUU != 0 || g_appliedHeadPitchUU != 0))
    {
        int32_t pUU = 0, yUU = 0, rUU = 0;
        if (MELEVR::HeadAim::ReadControlRotationUU(&pUU, &yUU, &rUU))
        {
            MELEVR::HeadAim::WriteControlRotationUU(pUU - g_appliedHeadPitchUU, yUU - g_appliedHeadYawUU, rUU);
        }
    }
    if (g_headAimActive)
    {
        LogLine(std::string("[HEADAIM] head aim OFF (") + (restoreWrite ? "injection removed" : "abandoned, no write") + ").");
    }
    g_headAimActive = false;
    g_appliedHeadYawUU = 0;
    g_appliedHeadPitchUU = 0;
    g_headRefYawDeg = 0.0f;
    g_headRefPitchDeg = 0.0f;
    g_seedRemYawUU = 0;      // [AIMSEED v2] abandon any in-flight handoff ramp
    g_seedRemPitchUU = 0;
    g_seedDoneYawUU = 0;
    g_seedDonePitchUU = 0;
}

// Head aims via ControlRotation, ADDITIVE with the stick (proven signs from bdc3c4c: +1/+1 on the
// HeadEulerDegrees yaw/pitch; the caller applies cfg invert as an input-sign flip).
void DriveAimWithHead(float headYawDeg, float headPitchDeg,
                       bool preserveRenderHandoff = true) noexcept
{
    if (!g_headAimActive)
    {
        // [AIMSEED] continuity handoff for the ordinary HMD head-aim path. A
        // controller ray is deliberately independent of the HMD: importing a
        // render-side head offset into the controller source would reintroduce
        // the fixed jump that source switching is meant to avoid.
        if (preserveRenderHandoff)
        {
            g_seedRemYawUU   = -MELEVR::RenderHook::HeadLookYawUU();
            g_seedRemPitchUU =  MELEVR::RenderHook::HeadLookPitchUU();
        }
        else
        {
            g_seedRemYawUU = 0;
            g_seedRemPitchUU = 0;
        }
        g_seedDoneYawUU = 0;
        g_seedDonePitchUU = 0;
        g_headRefYawDeg = headYawDeg;
        g_headRefPitchDeg = headPitchDeg;
        g_appliedHeadYawUU = 0;
        g_appliedHeadPitchUU = 0;
        g_headAimActive = true;
        LogLine(std::string("[HEADAIM] aim ON (") +
                (preserveRenderHandoff ? "head" : "controller") +
                " source, additive with stick)" +
                (g_seedRemYawUU != 0 || g_seedRemPitchUU != 0
                     ? " ramping in head-look offset yawUU=" + std::to_string(g_seedRemYawUU) +
                       " pitchUU=" + std::to_string(g_seedRemPitchUU) + "."
                     : "."));
    }

    // [AIMSEED v2] advance the handoff ramp: move ~25% of the remainder (min 150 UU ~ 0.8 deg, so the
    // tail doesn't crawl) from the render-side look offset into the CR injection this frame.
    const auto rampStep = [](int32_t& rem, int32_t& done) noexcept {
        if (rem == 0) return;
        constexpr int32_t kMinStep = 150;
        int32_t step = rem / 4;
        if (rem > 0) { if (step < kMinStep) step = (rem < kMinStep ? rem : kMinStep); }
        else         { if (step > -kMinStep) step = (rem > -kMinStep ? rem : -kMinStep); }
        done += step;
        rem -= step;
    };
    rampStep(g_seedRemYawUU, g_seedDoneYawUU);
    rampStep(g_seedRemPitchUU, g_seedDonePitchUU);

    const float offYawDeg = WrapDeg(headYawDeg - g_headRefYawDeg);
    const float offPitchDeg = headPitchDeg - g_headRefPitchDeg;
    const int32_t headYawUU = static_cast<int32_t>(offYawDeg * kDegToUU) + g_seedDoneYawUU;
    const int32_t headPitchUU = static_cast<int32_t>(offPitchDeg * kDegToUU) + g_seedDonePitchUU;

    int32_t pUU = 0, yUU = 0, rUU = 0;
    if (!MELEVR::HeadAim::ReadControlRotationUU(&pUU, &yUU, &rUU)) return;

    const int32_t stickYawUU = yUU - g_appliedHeadYawUU;       // strip last frame's injection
    const int32_t stickPitchUU = pUU - g_appliedHeadPitchUU;
    const int32_t newYawUU = stickYawUU + headYawUU;
    int32_t newPitchUU = NormalizeAngleUU(stickPitchUU) + headPitchUU;
    const int32_t kPitchLimitUU = static_cast<int32_t>(85.0f * kDegToUU);
    if (newPitchUU > kPitchLimitUU) newPitchUU = kPitchLimitUU;
    if (newPitchUU < -kPitchLimitUU) newPitchUU = -kPitchLimitUU;

    if (!MELEVR::HeadAim::WriteControlRotationUU(newPitchUU, newYawUU, rUU)) return;
    g_appliedHeadYawUU = headYawUU;
    g_appliedHeadPitchUU = headPitchUU;

    // Keep the source transition and the actual game-facing rotation visible
    // without flooding the runtime log. For controller aim, headYaw/headPitch
    // are the controller ray; for ordinary head aim they are the HMD angles.
    static uint64_t s_driveDiag = 0;
    if ((s_driveDiag++ % 60ull) == 0)
    {
        LogLine(std::string("[HEADAIM] drive source=") +
                (preserveRenderHandoff ? "head" : "controller") +
                " inputDeg=(" + std::to_string(headYawDeg) + "," +
                std::to_string(headPitchDeg) + ") deltaUU=(" +
                std::to_string(headYawUU) + "," + std::to_string(headPitchUU) +
                ") controlUU=(" + std::to_string(newYawUU) + "," +
                std::to_string(newPitchUU) + ")");
    }
}


// ================= AER (alternate-eye rendering) - ME2/ME3 full-rate model ================================
// Each primary-player render advances exactly one eye and publishes a stamp. Capture that fresh eye into
// history[eye], upload only that eye to its OpenXR swapchain, and submit both projection views. The other
// released swapchain image stays resident until that eye renders again, avoiding a second blocking XR wait.
// Per-eye separation lives in the rendered pixels (view-space camera-right shift), while each history image
// keeps the head pose it was rendered with so the compositor can reproject the held eye correctly.
ID3D11Texture2D* g_aerHist[2] = {nullptr, nullptr};
bool g_aerHistValid[2] = {false, false};
// Once an eye has landed in its OpenXR swapchain, the runtime keeps presenting that released image.
// Only the freshly rendered eye needs another acquire/wait/copy/release cycle. Tracking the initial
// fill separately allows catching up either eye after AER starts without re-uploading the stale eye forever.
bool g_aerEyeFilled[2] = {false, false};
// Per-eye render-time pose (ME2 parity, 2026-07-07): the head pose each eye's image was RENDERED at, captured
// when the eye is stamped. Submitting each eye at ITS pose (not one shared current pose) lets the compositor
// reproject the ~1-frame-old HELD eye to match the fresh eye -> kills the AER-only "shaky on turn". This is the
// "pose-tagging is sacred" rule: tag what you rendered, per eye.
XrPosef g_aerHistPose[2] = {};
bool g_aerHistPoseValid[2] = {false, false};
D3D11_TEXTURE2D_DESC g_aerHistDesc = {};
bool g_aerHistDescValid = false;
int g_aerRenderEye = 0;            // eye to render NEXT (armed each frame); flips on a fresh stamped capture
uint64_t g_aerLastSeq = 0;         // last render-stamp seq captured (cross-thread handshake)
uint64_t g_aerPresent = 0;
ULONGLONG g_aerHzWindowStartMs = 0;
unsigned int g_aerHzCaptures = 0;  // fresh single-eye captures in the current 2s window (pairs/sec = this/2)
int g_aerPrevCaptureEye = -1;
unsigned int g_aerSameEyeTwice = 0;
unsigned int g_aerSeqGapBad = 0;
uint64_t g_aerMaxSeqGap = 0;
unsigned int g_aerPresentsWindow = 0;
unsigned int g_aerNoRenderPresents = 0;
unsigned int g_aerNoStampPresents = 0;
// [AERBLACK] Last projection that successfully accompanied resident AER eye images. A transient
// GetBuffer/copy-path failure must hold this projection instead of ending a renderable XR frame
// with zero layers, which is a one-frame hard-black flash in both eyes.
XrCompositionLayerProjectionView g_aerLastProjViews[2] = {};
bool g_aerHaveLastProjection = false;
unsigned int g_aerFallbackFrames = 0;
unsigned int g_aerBlackFrames = 0;
ULONGLONG g_aerSubWindowMs = 0;

float AerEyeSign(int eye, bool swapEyes) noexcept
{
    const float base = (eye == 0) ? -1.0f : 1.0f;   // 0=L, 1=R
    return swapEyes ? -base : base;
}

void ResetAerHistory() noexcept
{
    for (int e = 0; e < 2; ++e)
    {
        if (g_aerHist[e] != nullptr) { g_aerHist[e]->Release(); g_aerHist[e] = nullptr; }
        g_aerHistValid[e] = false;
        g_aerEyeFilled[e] = false;
        g_aerHistPoseValid[e] = false;
    }
    g_aerHistDescValid = false;
    g_aerLastSeq = 0;
    g_aerRenderEye = 0;
    g_aerPrevCaptureEye = -1;
    g_aerSameEyeTwice = 0;
    g_aerSeqGapBad = 0;
    g_aerMaxSeqGap = 0;
    g_aerPresentsWindow = 0;
    g_aerNoRenderPresents = 0;
    g_aerNoStampPresents = 0;
    g_aerHaveLastProjection = false;
    g_aerFallbackFrames = 0;
    g_aerBlackFrames = 0;
    g_aerSubWindowMs = 0;
}

bool EnsureAerHistory(ID3D11Texture2D* src) noexcept
{
    if (g_device == nullptr || src == nullptr) return false;
    D3D11_TEXTURE2D_DESC d = {};
    src->GetDesc(&d);
    d.BindFlags = 0;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    d.Usage = D3D11_USAGE_DEFAULT;
    if (g_aerHistDescValid && g_aerHist[0] && g_aerHist[1] &&
        g_aerHistDesc.Width == d.Width && g_aerHistDesc.Height == d.Height && g_aerHistDesc.Format == d.Format)
    {
        return true;
    }
    ResetAerHistory();
    for (int e = 0; e < 2; ++e)
    {
        if (FAILED(g_device->CreateTexture2D(&d, nullptr, &g_aerHist[e])))
        {
            LogLine("[AER] history texture creation FAILED");
            ResetAerHistory();
            return false;
        }
    }
    g_aerHistDesc = d;
    g_aerHistDescValid = true;
    LogLine("[AER] history bank created " + std::to_string(d.Width) + "x" + std::to_string(d.Height));
    return true;
}

// ---- Display-locked cadence (the flicker fix) ----------------------------------------------------------
double g_displayPeriodSec = 1.0 / 120.0;   // headset frame period, locked after warmup / runtime query
double g_paceWarmAccumSec = 0.0;
int g_paceWarmCount = 0;
XrTime g_lastPredictedDisplayTime = 0;
bool g_paceLocked = false;
LARGE_INTEGER g_paceQpcFreq = {};
LARGE_INTEGER g_lastPaceQpc = {};
constexpr int kPaceWarmFrames = 90;

// Learn the headset's TRUE refresh. Order: pinned Hz override -> runtime XR_FB_display_refresh_rate -> measure.
void UpdateAerPaceWarmup(XrTime predictedDisplayTime, int pinnedHz) noexcept
{
    if (pinnedHz > 0)
    {
        const double p = 1.0 / static_cast<double>(pinnedHz);
        if (!g_paceLocked || std::fabs(p - g_displayPeriodSec) > 1e-6)
        {
            g_displayPeriodSec = p;
            g_paceLocked = true;
            g_refreshRateApplied = true;
            LogLine("[AERPACE] cadence: pinned " + std::to_string(pinnedHz) + "Hz -> display period locked");
        }
        return;
    }
    // Ask the runtime directly (correct even if the game runs below the display rate). FB ext fn ptr already resolved.
    if (!g_refreshRateApplied && g_fn.getDisplayRefreshRate != nullptr && g_session != nullptr)
    {
        float hz = 0.0f;
        if (XrSucceeded(g_fn.getDisplayRefreshRate(g_session, &hz)) && hz > 20.0f && hz < 1000.0f)
        {
            g_displayPeriodSec = 1.0 / static_cast<double>(hz);
            g_paceLocked = true;
            g_refreshRateApplied = true;
            // 2026-07-13: this string used to hardcode "paced to display/2" - it read as proof the hold was
            // engaging and fooled the flicker-recurrence rule-out. This line only means the refresh is KNOWN;
            // whether the hold actually engages is what [PACE] reports.
            LogLine("[AERPACE] cadence: runtime reports " + std::to_string(static_cast<int>(hz + 0.5f)) +
                    "Hz -> display period locked (no frame-delta guess).");
            return;
        }
        // query failed this frame - fall through to the frame-delta fallback
    }
    if (g_paceLocked) return;
    if (g_lastPredictedDisplayTime != 0 && predictedDisplayTime > g_lastPredictedDisplayTime)
    {
        const double dt = static_cast<double>(predictedDisplayTime - g_lastPredictedDisplayTime) * 1e-9;  // ns -> s
        if (dt > 0.004 && dt < 0.05) { g_paceWarmAccumSec += dt; ++g_paceWarmCount; }  // 250..20 Hz sane
    }
    g_lastPredictedDisplayTime = predictedDisplayTime;
    if (g_paceWarmCount >= kPaceWarmFrames && g_paceWarmAccumSec > 0.0)
    {
        const double hz = static_cast<double>(g_paceWarmCount) / g_paceWarmAccumSec;
        const double cands[] = {60.0, 72.0, 80.0, 90.0, 100.0, 120.0, 144.0};
        double best = 120.0, bestErr = 1e9;
        for (double c : cands) { const double e = (c > hz) ? (c - hz) : (hz - c); if (e < bestErr) { bestErr = e; best = c; } }
        g_displayPeriodSec = 1.0 / best;
        g_paceLocked = true;
        LogLine("[AERPACE] cadence: measured ~" + std::to_string(static_cast<int>(hz + 0.5)) +
                "Hz -> locked " + std::to_string(static_cast<int>(best + 0.5)) + "Hz display period.");
    }
}

// Hold the present to `periodsPerPresent` x display period so the cadence is a stable integer fraction of the
// headset refresh. Coarse Sleep(1) then a tight QPC spin for the last ~1.5 ms. Game stays unlocked.
// periodsPerPresent=2.0 for AER (each eye legitimately persists 2 display refreshes - the eye-swap IS half-rate
// by design). BUG FOUND 2026-07-12 (VD refresh measured 72Hz, not 120): stereoFramePacing was reusing
// this SAME 2x target unconditionally. At 72Hz that's 2*13.89ms=27.78ms -> a hard 36fps CAP - self-inflicted,
// below what that headset was already getting (48fps) WITHOUT this feature. At 120Hz, 2x lands near 60fps,
// which happens to roughly match the game's own natural cap, so it read as "smooth" and hid the bug entirely.
// Stereo renders BOTH eyes fresh every present (no eye persists anything) - it must lock 1:1 to the display,
// not halved. periodsPerPresent=1.0 for stereo.
void StereoPaceObserve(double workSec) noexcept;   // [STEREOPACE v2] defined with its state below

double g_lastPaceWorkSec = 0.0;   // [CINEPACE-ADAPT] most recent pre-hold work time, from the hold below

void PaceDisplayLockedAer(double periodsPerPresent, bool adaptiveObserve = false) noexcept
{
    if (!g_paceLocked) { g_lastPaceQpc.QuadPart = 0; return; }   // wait until the refresh is known
    if (g_paceQpcFreq.QuadPart == 0) QueryPerformanceFrequency(&g_paceQpcFreq);
    const double target = periodsPerPresent * g_displayPeriodSec;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (g_lastPaceQpc.QuadPart != 0)
    {
        double elapsed = static_cast<double>(now.QuadPart - g_lastPaceQpc.QuadPart) / static_cast<double>(g_paceQpcFreq.QuadPart);
        const double preHold = elapsed;
        // [STEREOPACE v2] the pre-hold elapsed IS the game's real work time (observable even while
        // pacing is engaged) - feed it to the adaptive lock when the stereo family is driving.
        if (adaptiveObserve) StereoPaceObserve(preHold);
        g_lastPaceWorkSec = preHold;   // [CINEPACE-ADAPT] same signal, read by the cine cap's miss counter
        while (target - elapsed > 0.0)
        {
            if (target - elapsed > 0.0015) Sleep(1);   // coarse sleep, then spin the last ~1.5 ms
            QueryPerformanceCounter(&now);
            elapsed = static_cast<double>(now.QuadPart - g_lastPaceQpc.QuadPart) / static_cast<double>(g_paceQpcFreq.QuadPart);
        }
        // [PACE] the instrument that cannot lie (2026-07-13): the flicker recurrence hid for a day because
        // g_paceLocked=true + a stale log string read as "pacing engaged" while the hold loop never ran once
        // (target below the game's own frame time = no-op). This reports the HOLD actually applied. avgHold
        // ~0.00ms while flicker is present = the pacer is configured but doing nothing; that's the tell.
        static double s_holdAccum = 0.0;
        static int s_holdCount = 0;
        s_holdAccum += (elapsed - preHold);
        if (++s_holdCount >= 600)
        {
            char pl[144];
            std::snprintf(pl, sizeof(pl), "[PACE] avgHold=%.2fms/present target=%.2fms (%.1f periods) over %d presents",
                          (s_holdAccum / s_holdCount) * 1000.0, target * 1000.0, periodsPerPresent, s_holdCount);
            LogLine(pl);
            s_holdAccum = 0.0;
            s_holdCount = 0;
        }
    }
    QueryPerformanceCounter(&g_lastPaceQpc);
}

// [STEREOPACE v2] ADAPTIVE display-period lock (2026-07-15). The 07-13 one-shot measure-and-freeze
// version had two field failures in its first day wired to Stereo 2:
//   (1) one session's single 120-present measurement landed in a heavy stretch (21.33ms = 2.56
//       periods) -> "no integer lock" FROZEN for the whole session = pacing silently off (the
//       configured-but-doing-nothing family the [PACE] instrument exists to expose).
//   (2) the next session locked 2 periods correctly, but transient sub-60 stretches at movement
//       start (frame work 17-25ms: streaming/shaders) overran the fixed 16.67ms hold -> presents
//       landed irregularly on 2-then-3 refresh boundaries; that IRREGULAR mix was the flicker felt
//       in-headset. A frozen lock cannot ride out a dip by construction.
// v2 keeps the external contract (returns periods to hold, 0 = stay out of the way) but:
//   - MEASURES CONTINUOUSLY while unlocked: every 120-present window re-evaluates; a heavy window
//     only delays the lock instead of freezing a decline for the session.
//   - Baseline sanity floor: a baseline N only locks if refresh/N >= ~44fps, so a load-window
//     can never freeze 72Hz into a 36fps cap (the original bug stays impossible).
//   - Cadence math is sanity-checked against the 72/80/90/120 ladder. Hinny that GOAT.
//   - Once baseline N0 locks, ADAPTS between N0 and N0+1: >=10% of a window overrunning the
//     N0-period slot (2ms grace) steps UP to N0+1 - a REGULAR N0+1 cadence reprojects smoothly,
//     the irregular mix does not. Recovery steps back DOWN when the observed work time clearly
//     fits N0 again, or via a timed probe with exponential backoff for setups where an internal
//     game limiter pins the observed work at the hold target (ambiguous ema). Never below N0;
//     never active without an integer baseline (72/90Hz free-run preserved).
double g_stereoNatAccumSec = 0.0;
int    g_stereoNatCount = 0;
LARGE_INTEGER g_stereoNatLastQpc = {};
int    g_stereoBaseN = 0;               // 0 = no baseline yet (measurement windows repeating)
int    g_stereoCurN = 0;                // active lock: g_stereoBaseN or g_stereoBaseN+1
int    g_stereoDeclineWindows = 0;      // consecutive no-lock windows (log throttle)
int    g_paceObsCount = 0;              // presents in the current observation window
int    g_paceObsMisses = 0;             // presents whose work overran the current slot
double g_paceWorkEmaSec = -1.0;         // EMA of pre-hold work time
ULONGLONG g_paceDownProbeAtMs = 0;      // earliest tick allowed for the next down-probe attempt
ULONGLONG g_paceProbeBackoffMs = 4000;  // doubles on a failed probe, caps at 60s; reset on success
ULONGLONG g_paceProbeArmedAtMs = 0;     // nonzero = down-probe live
int       g_paceProbeSkipLeft = 0;      // transition-echo frames to ignore at probe start
double    g_paceProbeEmaSec = -1.0;     // work EMA measured INSIDE the live probe
int       g_paceProbeCount = 0;         // judged presents inside the live probe

void StereoPaceObserve(double workSec) noexcept
{
    if (g_stereoBaseN <= 0 || g_stereoCurN <= 0 || g_displayPeriodSec <= 0.0) return;
    const double slot = g_stereoCurN * g_displayPeriodSec;
    g_paceWorkEmaSec = (g_paceWorkEmaSec < 0.0) ? workSec : (g_paceWorkEmaSec * 0.95 + workSec * 0.05);
    ++g_paceObsCount;
    const bool missed = workSec > slot + 0.002;
    if (missed) ++g_paceObsMisses;
    const ULONGLONG now = GetTickCount64();

    // Live down-probe. FIELD LESSON (2026-07-15 08:10 log): the first 1-3 frames after a target
    // step-down still arrive on the OLD slower rhythm (frames already in flight paced to the wider
    // slot) and read as 19-25ms "work" even with a 5ms ema - judging those killed EVERY probe and
    // parked the session at N0+1 with the GPU mostly idle. So: skip the echo frames, then require
    // TWO real misses (a lone hitch must not burn a good probe) before reverting.
    // Probe verdict is taken on the AVERAGE across a 60-present window, never on individual frames.
    // Judging single frames is what pinned a Low-resolution install at 60fps (2026-08-11, log-proven):
    // work averaged 5.5-9.7ms - comfortably inside the 8.33ms 1-period slot - but every probe died
    // within ~220ms on one or two spike frames reading 11-18ms, so the lock never came back down and
    // the pacer slept 8-13ms per present to hold 16.67ms. Spikes are exactly what a window average is
    // for; the echo skip still covers the queue drain right after the target changes.
    if (g_paceProbeArmedAtMs != 0)
    {
        if (g_paceProbeSkipLeft > 0)
        {
            --g_paceProbeSkipLeft;   // transition echo: not evidence either way
        }
        else
        {
            g_paceProbeEmaSec = (g_paceProbeEmaSec < 0.0) ? workSec
                                                          : (g_paceProbeEmaSec * 0.9 + workSec * 0.1);
            if (++g_paceProbeCount >= 60)
            {
                const double baseSlot = static_cast<double>(g_stereoBaseN) * g_displayPeriodSec;
                const bool fits = g_paceProbeEmaSec <= baseSlot + 0.002;
                char pl[208];
                if (fits)
                {
                    g_paceProbeBackoffMs = 4000;   // earned it: next probe is cheap again
                    std::snprintf(pl, sizeof(pl),
                                  "[STEREOPACE] down-probe held 60 presents (work ema %.2fms <= %.2fms slot) -> "
                                  "%d periods/present locked",
                                  g_paceProbeEmaSec * 1000.0, baseSlot * 1000.0, g_stereoBaseN);
                }
                else
                {
                    g_stereoCurN = g_stereoBaseN + 1;
                    g_paceDownProbeAtMs = now + g_paceProbeBackoffMs;
                    if (g_paceProbeBackoffMs < 16000) g_paceProbeBackoffMs *= 2;
                    std::snprintf(pl, sizeof(pl),
                                  "[STEREOPACE] down-probe averaged %.2fms over 60 presents (> %.2fms slot) -> "
                                  "back to %d periods, next probe in %llus",
                                  g_paceProbeEmaSec * 1000.0, baseSlot * 1000.0, g_stereoCurN,
                                  g_paceProbeBackoffMs / 1000ULL);
                }
                LogLine(pl);
                g_paceProbeArmedAtMs = 0;
                g_paceProbeEmaSec = -1.0;
                g_paceProbeCount = 0;
                g_paceObsCount = 0;
                g_paceObsMisses = 0;
                return;
            }
        }
    }

    if (g_paceObsCount < 120) return;
    const int misses = g_paceObsMisses;
    const double emaMs = g_paceWorkEmaSec * 1000.0;
    g_paceObsCount = 0;
    g_paceObsMisses = 0;

    // RETUNED 2026-08-11: was misses >= 12 (10%), which is exactly ME1's normal streaming-hitch
    // rate in busy areas - it stepped every such window to 2 periods (60fps) and the probe bug
    // above then kept it there. 117-119fps with ~10% overruns earlier the same day was
    // called smooth (tags + FREEZETAG absorb a late frame now), so the step-up is reserved for
    // genuinely heavy stretches: a quarter of the window.
    // Step UP only when the AVERAGE genuinely cannot hold the slot - never on a count of individual
    // overruns. Counting frames is what pinned a Low-resolution install at 60fps even after the probe
    // was fixed: at 2048x2124 a quarter of presents exceed an 8.33ms slot while the AVERAGE sits at
    // 5.5-9.7ms, so the count tripped, the target doubled to 16.67ms, and the pacer then slept 8-13ms
    // per present to enforce a 60fps cap the machine never needed. 15% headroom is the margin.
    if (g_stereoCurN == g_stereoBaseN && g_paceWorkEmaSec > slot * 1.15)
    {
        g_stereoCurN = g_stereoBaseN + 1;
        g_paceDownProbeAtMs = now + g_paceProbeBackoffMs;
        char pl[208];
        std::snprintf(pl, sizeof(pl),
                      "[STEREOPACE] work ema %.2fms cannot hold the %.2fms %d-period slot (%d/120 presents over) -> "
                      "stepping to %d periods (regular cadence beats an irregular mix)",
                      emaMs, slot * 1000.0, g_stereoBaseN, misses, g_stereoCurN);
        LogLine(pl);
        return;
    }
    // Probe attempts ALWAYS respect the spacing timer - the 08:10 log showed the ema fast-path
    // bypassing it and re-probing every ~3s (a 1-frame hitch per attempt). With the echo skip
    // above, the first correctly-spaced probe after a real recovery succeeds anyway.
    if (g_stereoCurN > g_stereoBaseN && g_paceProbeArmedAtMs == 0 && misses == 0 &&
        now >= g_paceDownProbeAtMs)
    {
        const double baseSlotMs = g_stereoBaseN * g_displayPeriodSec * 1000.0;
        g_stereoCurN = g_stereoBaseN;
        g_paceProbeArmedAtMs = now;
        g_paceProbeEmaSec = -1.0;
        g_paceProbeCount = 0;
        // 12 frames (~100ms) of transition cover: the presents right after the target changes still
        // arrive on the old rhythm and read as false overruns. Was 3, dialled when presents were
        // ~16.7ms; at ~8.3ms that covered only 25ms.
        g_paceProbeSkipLeft = 12;
        char pl[176];
        std::snprintf(pl, sizeof(pl),
                      "[STEREOPACE] recovery window (work ema %.2fms vs %.2fms baseline slot, 0 misses) -> probing %d periods",
                      emaMs, baseSlotMs, g_stereoCurN);
        LogLine(pl);
    }
}

double StereoLockPeriods() noexcept
{
    if (!g_paceLocked) return 0.0;   // refresh not known yet: no hold, keep measuring later
    if (g_stereoBaseN > 0) return static_cast<double>(g_stereoCurN);
    if (g_paceQpcFreq.QuadPart == 0) QueryPerformanceFrequency(&g_paceQpcFreq);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (g_stereoNatLastQpc.QuadPart != 0)
    {
        const double dt = static_cast<double>(now.QuadPart - g_stereoNatLastQpc.QuadPart) /
                          static_cast<double>(g_paceQpcFreq.QuadPart);
        if (dt > 0.004 && dt < 0.05) { g_stereoNatAccumSec += dt; ++g_stereoNatCount; }   // 250..20fps sane
    }
    g_stereoNatLastQpc = now;
    if (g_stereoNatCount >= 120 && g_stereoNatAccumSec > 0.0)
    {
        const double naturalSec = g_stereoNatAccumSec / static_cast<double>(g_stereoNatCount);
        const double ratio = naturalSec / g_displayPeriodSec;
        const double nearest = std::floor(ratio + 0.5);
        g_stereoNatAccumSec = 0.0;   // v2: measurement windows repeat until one locks
        g_stereoNatCount = 0;
        const double fpsAtN = (nearest >= 1.0) ? 1.0 / (nearest * g_displayPeriodSec) : 0.0;
        char sl[208];
        if (nearest >= 1.0 && std::fabs(ratio - nearest) <= 0.25 && fpsAtN >= 44.0)
        {
            g_stereoBaseN = static_cast<int>(nearest);
            g_stereoCurN = g_stereoBaseN;
            std::snprintf(sl, sizeof(sl),
                          "[STEREOPACE] natural present %.2fms = %.2f display periods -> LOCKING %d periods/present "
                          "(adaptive: steps to %d under sustained load)",
                          naturalSec * 1000.0, ratio, g_stereoBaseN, g_stereoBaseN + 1);
            LogLine(sl);
        }
        else
        {
            ++g_stereoDeclineWindows;
            if (g_stereoDeclineWindows <= 3 || (g_stereoDeclineWindows % 30) == 0)
            {
                std::snprintf(sl, sizeof(sl),
                              "[STEREOPACE] natural present %.2fms = %.2f display periods -> no lock this window "
                              "(#%d%s), measuring continues (no fps throttle)",
                              naturalSec * 1000.0, ratio, g_stereoDeclineWindows,
                              (nearest >= 1.0 && fpsAtN < 44.0) ? ", below 44fps floor" : "");
                LogLine(sl);
            }
        }
    }
    return (g_stereoBaseN > 0) ? static_cast<double>(g_stereoCurN) : 0.0;
}

// AER submit: capture the freshly-rendered eye into history, submit BOTH eyes (one ~1 frame old). Returns
// true if it submitted to the eye swapchains. Separation is in the pixels (armed in the lean block); both
// eyes share the latched renderTagPose at the projection-layer build, so no per-eye pose divergence.
bool SubmitAerFrame(const MELEVR::Config::VrConfig& cfg, ID3D11Texture2D* backBuffer,
                    const XrPosef& renderPose, bool fillSwapchains) noexcept
{
    if (backBuffer == nullptr) return false;
    ++g_aerPresent;
    if (!EnsureAerHistory(backBuffer))
    {
        if (fillSwapchains)
        {
            CopyTextureToEyeFullFrame(0, backBuffer);   // un-squash (see below); history alloc failed this frame
            CopyTextureToEyeFullFrame(1, backBuffer);
        }
        return true;
    }

    // [AERSHAKE, ported from ME2/ME3] FIFO consume: the OLDEST unconsumed build - the one whose
    // pixels this present actually shows - never "the latest stamp", whose eye can belong to a
    // build still in flight (the full-rate shake).
    MELEVR::RenderHook::RenderStamp stamp = {};
    bool aerResynced = false;
    const bool newFrame = MELEVR::RenderHook::ConsumeRenderStamp(g_aerLastSeq, &stamp.seq, &stamp.eye, &aerResynced) &&
                          stamp.eye >= 0 && stamp.eye <= 1;
    ++g_aerPresentsWindow;
    if (!fillSwapchains) ++g_aerNoRenderPresents;
    if (!newFrame) ++g_aerNoStampPresents;
    if (newFrame)
    {
        // With the FIFO, a resync (fell a whole ring behind: mode switch, long menu, load) is the
        // only legitimate discontinuity; anything else here is a real bug again.
        if (aerResynced)
        {
            ++g_aerSeqGapBad;
            if (stamp.seq - g_aerLastSeq > g_aerMaxSeqGap) g_aerMaxSeqGap = stamp.seq - g_aerLastSeq;
        }
        if (stamp.eye == g_aerPrevCaptureEye) ++g_aerSameEyeTwice;
        g_aerPrevCaptureEye = stamp.eye;

        // [RELIEF M1.3] depth pop for AER: warp the fresh full-frame eye by the live depth before it enters
        // history (so the held eye stays warped for its 2 display frames = zero per-frame cost). eyeSign
        // opposite per eye adds binocular disparity on top of AER's base separation. Off / no depth -> raw.
        ID3D11Texture2D* aerSrc = backBuffer;
        // [RELIEF_AER] AER shares Stereo's relief values and shader (RenderReliefPass); only mode/eyeSign
        // differ. So when depth pop "doesn't work in AER" the warp is almost certainly not RUNNING - it
        // falls through to the raw backbuffer silently when depth isn't ready. Say so out loud.
        const MELEVR::Config::ReliefParams rpAer = MELEVR::Config::ActiveRelief(cfg);   // AER's own set
        const bool reliefWanted = cfg.reliefEnabled &&
            (rpAer.strength > 0.0001f || rpAer.darkStrength > 0.0001f || rpAer.unsharpStrength > 0.0001f);
        const bool depthReady = MELEVR::D3DCapture::IsReliefDepthReady();
        bool warpRan = false;
        if (reliefWanted && depthReady)
        {
            const float eyeSign = (stamp.eye == 0) ? 1.0f : -1.0f;
            ID3D11Texture2D* warped = MELEVR::D3DCapture::RenderReliefFull(backBuffer, eyeSign);
            if (warped != nullptr) { aerSrc = warped; warpRan = true; }
        }
        if (reliefWanted)
        {
            static ULONGLONG s_lastReliefLog = 0;
            const ULONGLONG nowRelief = GetTickCount64();
            if (nowRelief - s_lastReliefLog >= 1000)
            {
                s_lastReliefLog = nowRelief;
                char rl[192];
                std::snprintf(rl, sizeof(rl),
                              "[RELIEF_AER] wanted=1 depthReady=%d warpRan=%d eye=%d strength=%.4f conv=%.4f",
                              depthReady ? 1 : 0, warpRan ? 1 : 0, stamp.eye, rpAer.strength,
                              rpAer.autoConverge ? -1.0f : rpAer.convergence);
                LogLine(rl);
            }
        }
        g_context->CopyResource(g_aerHist[stamp.eye], aerSrc);
        g_aerHistValid[stamp.eye] = true;
        g_aerHistPose[stamp.eye] = renderPose;   // tag THIS eye with the head pose it was rendered at (anti turn-shake)
        g_aerHistPoseValid[stamp.eye] = true;
        g_aerLastSeq = stamp.seq;
        // [AERSHAKE] no arming: the build picks its own eye from seq parity now. g_aerRenderEye is
        // still passed to SetAerState for compatibility but no longer steers anything.

        // [AERHZ] cadence meter: fresh single-eye captures/sec (pairs/sec = half). Read this to PROVE the
        // cadence is locked (stable ~headset/2) rather than eyeballing flicker.
        const ULONGLONG now = GetTickCount64();
        if (g_aerHzWindowStartMs == 0) { g_aerHzWindowStartMs = now; g_aerHzCaptures = 0; }
        ++g_aerHzCaptures;
        const ULONGLONG elapsed = now - g_aerHzWindowStartMs;
        if (elapsed >= 2000)
        {
            const double capHz = (static_cast<double>(g_aerHzCaptures) * 1000.0) / static_cast<double>(elapsed);
            char line[360] = {};
            sprintf_s(line, "[AERHZ] captureHz=%.1f pairHz=%.1f captures=%u sameEyeTwice=%u seqGapBad=%u maxSeqGap=%llu presents=%u noRender=%u noStamp=%u windowMs=%llu halfEyeUU=%.2f",
                      capHz, capHz * 0.5, g_aerHzCaptures, g_aerSameEyeTwice, g_aerSeqGapBad,
                      static_cast<unsigned long long>(g_aerMaxSeqGap),
                      g_aerPresentsWindow, g_aerNoRenderPresents, g_aerNoStampPresents,
                      static_cast<unsigned long long>(elapsed), cfg.aerHalfEyeUU);
            LogLine(line);
            g_aerHzWindowStartMs = now;
            g_aerHzCaptures = 0;
            g_aerSameEyeTwice = 0;
            g_aerSeqGapBad = 0;
            g_aerMaxSeqGap = 0;
            g_aerPresentsWindow = 0;
            g_aerNoRenderPresents = 0;
            g_aerNoStampPresents = 0;
        }
    }

    // [AERGAP] The game rendered and stamped an eye even when OpenXR said not to render.
    // Consume the handshake and refresh history, but do no swapchain work on that frame.
    if (!fillSwapchains) return true;

    // AER UN-SQUASH (2026-07-03): full-fill the eye instead of aspect-preserving. At 4K the game renders a
    // ~square-FOV view stretched into the 16:9 backbuffer; CopyTextureToEye preserved that wide aspect and
    // kept it stretched (the squash). CopyTextureToEyeFullFrame maps the whole frame to the whole eye, which
    // compresses the horizontal stretch back to correct. Submit-only, deterministic - the safe fix per the
    // handoff (NOT the render-side square viewport, which froze).
    if (g_aerHistValid[0] && g_aerHistValid[1])
    {
        // AER blink fix (ME3 a86b963): uploading both eyes every Present forces two OpenXR
        // swapchain waits on the render thread. The stale eye is already resident in its
        // released swapchain image, so upload only the eye rendered on this frame.
        if (newFrame && CopyTextureToEyeFullFrame(stamp.eye, g_aerHist[stamp.eye]))
            g_aerEyeFilled[stamp.eye] = true;

        // Bootstrap/catch-up safeguard. This normally runs only once per eye after AER starts
        // (or after a history reset) and also retries honestly if an OpenXR copy did not land.
        for (int e = 0; e < 2; ++e)
        {
            if (!g_aerEyeFilled[e] && CopyTextureToEyeFullFrame(e, g_aerHist[e]))
                g_aerEyeFilled[e] = true;
        }
    }
    else
    {
        // Before both histories exist, show the current frame in both eyes. Do not mark either
        // eye filled: once a true stereo pair exists, the catch-up path uploads both real histories.
        CopyTextureToEyeFullFrame(0, backBuffer);
        CopyTextureToEyeFullFrame(1, backBuffer);
    }
    return true;
}

// Per-Present frame loop: locate head -> drive the game's view rotation (look-around) -> copy the rendered
// frame into both eyes -> submit a WORLD-LOCKED projection layer (roll removed so tilt keeps the world
// level), FOV matched to the render. Both eyes = same image (no stereo). Lean is M1b.

// Copy the config's per-element overrides into the Pchud element state (types are layout-twins; the copy
// keeps vr_config free of pchud dependencies). Shared by RunFrame and FlatHudTick.
void PushElementConfigs(const MELEVR::Config::VrConfig& cfg) noexcept
{
    // Individual element overrides. Only radarMC + BottomUI are exposed in the menu (both STATIC, safe);
    // the rest of the array stays default = untouched. Groups were dropped 2026-07-06 (the master slider's
    // center-pivot fix covers "resize the whole HUD in place"; radar + weapon bar get their own controls).
    // Per-mode HUD layout: stereo draws the UI per-eye at half width, so it has its own element set.
    const bool useStereoUi = cfg.stereoEnabled;
    // Stereo + DIBR each own a HUD element set; AER/Mono share the normal set.
    const auto& hudSrc = cfg.dibrEnabled ? cfg.hudElemsDibr : (useStereoUi ? cfg.hudElemsStereo : cfg.hudElems);
    MELEVR::Pchud::HudElemState hud[MELEVR::Pchud::kHudElemCount];
    for (int i = 0; i < MELEVR::Pchud::kHudElemCount; ++i)
    {
        const auto& s = hudSrc[i];
        hud[i] = { s.on, s.hide, s.offX, s.offY, s.scaleX, s.scaleY };
    }
    MELEVR::Pchud::SetHudElements(hud, MELEVR::Pchud::kHudElemCount);
    MELEVR::Pchud::SetSubtitleFontIndex(cfg.subtitleFontIndex);

    MELEVR::Pchud::HudElemState convo[MELEVR::Pchud::kConvoElemCount];
    for (int i = 0; i < MELEVR::Pchud::kConvoElemCount; ++i)
    {
        const auto& s = cfg.convoElems[i];
        convo[i] = { s.on, s.hide, s.offX, s.offY, s.scaleX, s.scaleY };
    }
    MELEVR::Pchud::SetConvoElements(convo, MELEVR::Pchud::kConvoElemCount);
}

// [LIVEGUI] kill switch: MELEVR_DISABLE_LIVEGUI.txt next to the exe restores the pre-fix behavior
// (every gameMode-7 state gets the flat menu treatment). Same 2s cached poll as the REFLRATE marker.
bool LiveGuiMarkerDisabled() noexcept
{
    static ULONGLONG s_nextPoll = 0;
    static bool s_disabled = false;
    const ULONGLONG now = GetTickCount64();
    if (now >= s_nextPoll)
    {
        s_nextPoll = now + 2000;
        bool disabled = false;
        wchar_t exePath[MAX_PATH] = {};
        const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len > 0 && len < MAX_PATH)
        {
            wchar_t* slash = wcsrchr(exePath, L'\\');
            if (slash != nullptr)
            {
                *(slash + 1) = L'\0';
                wchar_t marker[MAX_PATH] = {};
                if (swprintf_s(marker, L"%sMELEVR_DISABLE_LIVEGUI.txt", exePath) > 0)
                {
                    disabled = GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES;
                }
            }
        }
        s_disabled = disabled;
    }
    return s_disabled;
}

// ===========================================================================================
// CINE POLICY (2026-08-02). Two versions of "is this a conversation/cutscene, and how should it
// present it". Everything else in RunFrame consumes the
// CineDecision and never learns which version produced it.
//
//   v1 = the shipped 2026-07-18 [CINEVR2] behaviour, moved here verbatim. Camera-mode class
//        name is the primary signal, the engine mode byte is a second opinion, and a 600-frame
//        hold rides out the read-fail storms. Cine gets its own fill pair and an adaptive
//        render scale.
//   v2 = ME2/ME3's model ([CINELATCH] + [GAMEPLAYVR] + [DEADVETO] + [VRCINE FILL]). The engine
//        mode byte is AUTHORITATIVE IN BOTH DIRECTIONS, a dead scene can never render in VR,
//        the cine window is the gameplay window, and there is no render scaler.
//
// Why v2 exists: ME2 and ME3 arrived at this shape by paying for the alternatives in-headset.
// The three findings that matter, all reproduced in their source comments:
//   1. Per-shot signals must never drive whole-scene presentation. ME3's cameras change CLASS
//      per shot ([CONVOLATCH]); ME2's raw FOV crosses the cine thresholds between a wide master
//      and a close-up ([CINELATCH]). Both flapped the presentation mid-scene. ME1 v1 keys on
//      the camera-mode class name, which is the same class of signal.
//   2. The mode byte must also CLEAR the latch. ME2's Archangel bug: a cutscene ended, the
//      engine returned to mode 0, but the FOV-based latch could not un-latch, so 31 seconds of
//      gameplay rendered with no second eye. v1 clears on modes 0-4 already, but only after the
//      camera-name test has had its say; v2 makes the byte win outright.
//   3. A dead scene has no 3D in it, so VR presentation is never correct there, whatever the
//      mode byte says ([DEADVETO]). Prerendered video declared at the fill FOV renders
//      vertically stretched, in stereo.
// ===========================================================================================

struct CineInputs
{
    int         gameMode      = -1;      // USFXGameModeManager::CurrentMode, -1 = unreadable
    bool        haveCineMode  = false;   // camera-mode class name read succeeded
    const char* cineModeName  = "";
    bool        binkActive    = false;   // Bink decoding within the last 250ms
    bool        sceneAlive    = true;    // a 3D perspective view is rendering (see [DEADVETO])
    bool        inMenuMode    = false;
    bool        inGalaxyMode  = false;
    bool        inMovie       = false;
    bool        liveWorldGui  = false;
    bool        manualFlat    = false;   // g_flatCineCopyActive
    bool        sfr2CfgOn     = false;   // Stereo 2 is the active mode (serves tracking-off cine)
};

struct CineDecision
{
    bool  inConversation   = false;
    bool  inCutscene       = false;
    bool  vrCineActive     = false;
    bool  forceFlatCine    = false;
    float fillH            = 1.0f;   // FOV-fill multipliers to use THIS frame
    float fillV            = 1.0f;
    float cineZoom         = 1.0f;   // lens zoom while a VR cine renders (1.0 = none)
};

// Presentation: context -> the flags the rest of RunFrame consumes.
static void CineFinishDecision(CineDecision& d, const CineInputs& in,
                               const MELEVR::Config::VrConfig& cfg) noexcept
{
    const bool cinematicContext = d.inConversation || d.inCutscene;
    bool vrCineOn = (d.inConversation && cfg.cineVrConvo) ||
                    (d.inCutscene && cfg.cineVrCutscene);
    // [DEADVETO] a dead scene is prerendered video: no depth exists, so VR presentation is never
    // right. ME2 landed on this for its gm 5/6 startup logos; ME3 for its gm-8 movies.
    if (!in.sceneAlive) vrCineOn = false;

    d.forceFlatCine = in.manualFlat ||
                      (cinematicContext && !vrCineOn) ||
                      (in.inMenuMode && cfg.menuFlat && !in.liveWorldGui) ||
                      (in.inGalaxyMode && cfg.galaxyFlat) ||
                      in.inMovie;
    // A VR cine is served by the SFR pair (Stereo 2). Other modes fall back to the flat screen:
    // an AER quad pair is two different frames = shimmer, and mono has no depth to show anyway.
    if (cinematicContext && vrCineOn && !in.sfr2CfgOn) d.forceFlatCine = true;
    d.vrCineActive = cinematicContext && vrCineOn && !d.forceFlatCine;

    // [VRCINE FILL] render AND declare the same window gameplay uses. ME2's reasoning, from its
    // own source: the director's per-shot framing FOV is not the mod's to honour in a headset.
    // Honouring it put the whole scene in a ~20-deg window inside a ~100-deg view, and the window
    // RESIZED on every camera cut because each shot carries its own FOV. A constant window means
    // shot cuts change the picture, not the screen. There is no cine-scoped fill pair any more.
    d.fillH = cfg.vrFillH;
    d.fillV = cfg.vrFillV;
    if (d.vrCineActive)
    {
        float zoom = cfg.cineZoom;   // ME2's single CineScreenZoom, both contexts
        if (!(zoom > 0.5f && zoom < 2.51f)) zoom = 1.0f;
        d.cineZoom = zoom;
    }
    // [CINERIP 2026-08-09] THERE IS NO BAND CROP ANY MORE, and this is where the note explaining
    // why used to live. The history is worth keeping because it is the trap:
    //
    // LE1 constrains the cine CAMERA to 16:9. Against the old SQUARE render target (6144x6144) the
    // engine therefore drew a wide short band inside a square frame, so the submit cropped that
    // band and stretched it ~1.78x vertically to refill the eye. Disarming the crop while leaving
    // the square target shipped as "squashed vertically" (2026-08-02), which was read as proof the
    // crop was load-bearing. It was not. It was a correction for a self-inflicted cause,
    // and the correction needed its own correction ([CINEUI] pre-compressed every UI draw by the
    // inverse) because one whole-image transform cannot serve both the 3D and the 2D.
    //
    // The target is 16:9 now, which is what ME2 and ME3 have always had. The camera's own 16:9
    // constraint fills the buffer exactly, the picture lands 1:1, and the entire tower - crop,
    // stretch, UI counter-squash, per-frame aspect probe - was deleted rather than ported. If a
    // cine ever looks squashed again, the question is what changed the TARGET aspect. Do not
    // reintroduce a crop.
}

// ME2/ME3's model. The engine mode byte decides, in both directions.
static CineDecision DecideCine(const CineInputs& in,
                               const MELEVR::Config::VrConfig& cfg) noexcept
{
    CineDecision d;
    // [CINELATCH]/[GAMEPLAYVR]. ME2's rule, one for one:
    //   5 Conversation / 6 Cinematic  -> ARE a cine, whatever any per-shot signal says. Latched.
    //   0-4 (default/vehicle/wheels/command) -> ARE gameplay. Latch cleared, unconditionally.
    //   anything else (7 GUI, 8 Movie, 9 Galaxy, -1 unreadable) -> HOLD the previous state.
    // Holding rather than clearing on 7/8/9 is deliberate and is ME2's shape: a hybrid cutscene
    // that drops into a Bink segment reports Movie for a while and comes back. Clearing there
    // would flap the whole presentation mid-scene, which is the exact failure this model exists
    // to remove. The video itself is handled by [DEADVETO] in CineFinishDecision and by the
    // inMovie flat-force, so holding the CONTEXT costs nothing.
    static bool s_convo = false, s_cutscene = false;
    if (in.gameMode == 5)                          { s_convo = true;  s_cutscene = false; }
    else if (in.gameMode == 6)                     { s_convo = false; s_cutscene = true;  }
    else if (in.gameMode >= 0 && in.gameMode <= 4) { s_convo = false; s_cutscene = false; }
    else if (in.haveCineMode && !s_convo && !s_cutscene)
    {
        // Mode byte unreadable and nothing latched: fall back to ME1's second signal, the camera
        // class name. Only a DEFINITE name may open the latch here - Interpolate/Transition and
        // unknown names hold, because those are exactly the per-shot noise that must never drive
        // presentation. Once latched, the byte above owns the exit.
        if (std::strstr(in.cineModeName, "Conversation") != nullptr)    { s_convo = true; }
        else if (std::strstr(in.cineModeName, "Cinematic") != nullptr)  { s_cutscene = true; }
    }
    d.inConversation = s_convo;
    d.inCutscene     = s_cutscene;
    // [CINEFLAPFIX 2026-08-12] Holding the CONTEXT through gm 7/8 blips was not enough: the
    // PRESENTATION inputs still flapped straight through the latch. Measured in the opening
    // cutscene (five VR<->FLAT flips in three seconds, [VRSTATE]):
    //   - inMenuMode is raw gameMode==7 with no hysteresis, so a 0.36s gm-7 blip mid-cutscene
    //     took the menu-flat branch instantly;
    //   - inMovie enters after only 4 presents without a 3D view (~38ms at 105fps) whenever Bink
    //     is decoding - and the intro is a bink/engine hybrid with fades, so it tripped on blips
    //     and took 0.3s to recover. Enter-fast/exit-slow = guaranteed flapping.
    // While a cine context is LATCHED, both flat-forces must now be SUSTAINED before they act:
    // gm 7 for ~0.5s (a real pause menu opened during a cutscene easily holds that), movie for
    // ~0.3s (a real bink segment has no 3D for seconds, so it still flattens quickly - and the
    // [DEADVETO] scene-alive latch remains the independent backstop for real video). Outside a
    // latched cine nothing changes: boot logos, menus and movies flatten instantly as before.
    // Flipping VR<->flat every few hundred ms while the head moves is itself severe judder
    // (world-locked projection alternating with a head-locked quad), which is what the
    // reported bad head tracking in the opening cutscene actually was.
    {
        static int s_menuRun = 0, s_movieRun = 0;
        if (in.inMenuMode) { if (s_menuRun < 1000) ++s_menuRun; } else s_menuRun = 0;
        if (in.inMovie)    { if (s_movieRun < 1000) ++s_movieRun; } else s_movieRun = 0;
        const bool cineLatched = s_convo || s_cutscene;
        CineInputs adj = in;
        if (cineLatched)
        {
            constexpr int kMenuSustain  = 45;   // ~0.5s at ~90-105 presents/s
            constexpr int kMovieSustain = 30;   // ~0.3s
            if (s_menuRun  < kMenuSustain)  adj.inMenuMode = false;
            if (s_movieRun < kMovieSustain) adj.inMovie    = false;
        }
        CineFinishDecision(d, adj, cfg);
    }
    return d;
}

void RunFrame(IDXGISwapChain* gameSwapChain) noexcept
{
    // Conversation / cutscene MONO override. Detect the cinematic camera mode at the TOP of the frame so a "flat"
    // context renders clean mono with NO residual shift. forceFlatCine below suppresses the stereo submit and the
    // FOV-fill (the things that stretch the game's animated cinematic framing).
    // CINEMATIC AUTO-FLAT DETECTION RETIRED 2026-07-03 (strip-firstperson-livecode): it read the game's
    // weapon/camera mode via GameCamera::ReadWeaponMode every frame. With the FP/GameCamera machinery gone,
    // there is no mode read; gameplay renders normally in VR. forceFlatCine now defaults FALSE and is driven
    // ONLY by the manual g_flatCineCopyActive flag (a menu/manual flat toggle can still set it if one exists).
    const MELEVR::Config::VrConfig& cfg = MELEVR::Config::Get();
    // CONVO/CUTSCENE AUTO-DETECT (restored 2026-07-07). Read the active camera-mode class name: conversations =
    // BioCameraBehaviorConversation, cutscenes = SFXCameraMode_Cinematic. This drives forceFlatCine (mono) ONLY
    // when the per-context toggle is on (convoFlat/cutsceneFlat), so "VR conversations/cutscenes" is a real
    // choice again. The manual g_flatCineCopyActive flag still forces flat if ever set. This is the detection
    // that was retired when game_camera left the build; now read through head_aim's validated controller.
    char cineModeName[64] = {};
    const bool haveCineMode = MELEVR::HeadAim::GetCameraModeNameSEH(cineModeName, sizeof(cineModeName));
    const bool binkActiveNow = MELEVR::D3DCapture::LastBinkFrameAgeMs() < 250ull;   // movie frames decoding
    // ENGINE GAME-MODE BYTE (USFXGameModeManager::CurrentMode). 0 Default(gameplay), 1 Vehicle, 2 PowerWheel,
    // 3 WeaponWheel, 4 Command, 5 Conversation, 6 Cinematic, 7 GUI menu, 8 Movie, 9 Galaxy. -1 = unreadable.
    // Read FIRST (2026-07-08): it is a SECOND, independent cine signal - the camera-mode name read fails in
    // storms during cutscenes (unpossession), which made mono/crop "work sometimes, flip other times".
    const int  gameMode     = MELEVR::HeadAim::ReadGameModeSEH();
    // [LIVEGUI 2026-07-17] gameMode 7 is AMBIGUOUS: real front-end/pause menus AND live in-world GUI
    // overlays (the Eden Prime bomb disarm) both report it, and the disarm got the menu treatment =
    // the "headset freezes during bomb disposal" bug. TWO discriminators are now FALSIFIED in-headset:
    //   - perspective-freshness (reverted 096b7a9): the main-menu flyby is a live perspective scene;
    //   - pawn + Pauser + TimeSeconds-advancing (same-day attempt): the in-game ESC menu keeps Pauser
    //     null, keeps the sim clock advancing AND keeps rendering the visually-frozen world (SFR pairs
    //     kept streaming), so it fingerprinted as "live" and the menu broke.
    // VERDICT IS DISABLED (always static -> menus flatten exactly like the shipped behavior) until the
    // real discriminator is proven from the [LIVEGUI2] telemetry below: one run that opens the ESC
    // menu and then runs the bomb disarm gives both fingerprints (dilation, per-tick delta,
    // bPlayersOnly, pauser, perspective age, SFR pair production). Fix the verdict from DATA only.
    bool liveWorldGui = false;
    {
        static uint64_t s_lgTick = 0;            // mode-7 presents observed
        static float    s_lgLastTs = 0.0f;       // TimeSeconds at the previous telemetry line
        static float    s_lgLastRts = 0.0f;      // RealTimeSeconds at the previous telemetry line
        if (gameMode == 7)
        {
            const uint64_t tick = s_lgTick++;
            if ((tick % 60) == 0)   // entry + ~1/s at 60fps
            {
                MELEVR::HeadAim::WorldTelemetry wt = {};
                const bool ok = MELEVR::HeadAim::ReadWorldTelemetrySEH(&wt);
                const uint64_t perspAge = MELEVR::RenderHook::PresentsSinceLastPerspective();
                char line[288];
                std::snprintf(line, sizeof(line),
                              "[LIVEGUI2] mode7 tick=%llu ok=%d pawn=%d pauser=%d playersOnly=%d/%d "
                              "dilation=%.4f dt=%.5f ts=%.3f dTs=%.3f rts=%.3f dRts=%.3f perspAge=%llu",
                              static_cast<unsigned long long>(tick), ok ? 1 : 0,
                              wt.hasPawn ? 1 : 0, wt.pauserSet ? 1 : 0,
                              wt.playersOnly ? 1 : 0, wt.playersOnlyPending ? 1 : 0,
                              wt.timeDilation, wt.deltaSeconds,
                              wt.timeSeconds, wt.timeSeconds - s_lgLastTs,
                              wt.realTimeSeconds, wt.realTimeSeconds - s_lgLastRts,
                              static_cast<unsigned long long>(perspAge));
                LogLine(line);
                s_lgLastTs = wt.timeSeconds;
                s_lgLastRts = wt.realTimeSeconds;
            }
        }
        else
        {
            s_lgTick = 0;   // next mode-7 entry logs immediately (tick 0)
        }
    }
    // [SFR] gate feed MOVED below the cine decision ([SFRGATE-LATCH 2026-08-12]): it used to be fed
    // the RAW mode byte here, which flaps 7/8/-1 mid-cutscene - the presentation held (CINEFLAPFIX)
    // but SfrModeAllowsReplay returns false for all three, so every blip killed the SECOND EYE pass
    // for a burst of presents. Pairs died while the screen stayed in VR cine = mono/held flashes =
    // the residual cutscene judder that survived every tag fix. The publish now happens after
    // DecideCine with the LATCHED context folded in, so the replay gate and the presentation can
    // never disagree mid-scene again.
    const bool inMenuMode   = (gameMode == 7);                      // 7 = GUI (full-screen front-end menu)
    const bool inGalaxyMode = (gameMode == 9);                      // 9 = Galaxy map (flat mono, or VR w/ zoom)
    // PRERENDERED MOVIE / 2D-only detection (2026-07-07: movies are too zoomed in). A prerendered
    // movie (and loading / main-menu screens) renders NO 3D perspective view, so the "presents since the last
    // 3D view" gap grows. When flat 2D is on screen, force mono so the stereo split / DIBR warp stop zooming +
    // distorting the video. Hysteresis: enter after a few flat presents, leave the instant a 3D view returns.
    // (Moved above the cine policy 2026-08-02: forceFlatCine consumes inMovie, and the policy owns
    // forceFlatCine now. The detector reads only Bink age + perspective freshness, so it has no
    // dependency on the convo/cutscene state and the move is order-safe.)
    bool inMovie = false;
    {
        // BINK GROUND TRUTH (2026-07-07, v3): the gap-only heuristic confused cutscene FADES (no 3D briefly,
        // no Bink) with movies and flapped on movie precache blips (stray 3D view mid-movie) - and while the
        // movie flag was on it hijacked the CUTSCENE mono width (the "slider stopped working"). Now: a movie is
        // Bink actively decoding (<250ms since a DoFrame) AND the 3D scene absent for a few presents. Cutscene
        // fades have no Bink -> never trip. Galaxy-map style panel videos keep 3D alive -> never trip. Once in,
        // stay while Bink keeps decoding (a 1-frame 3D blip can't flap it); exit the moment Bink stops or 3D
        // renders sustained.
        static int s_movieOn = 0;
        static int s_3dRun = 0;
        static int s_gm8BinkRun = 0;
        static int s_lastLogged = -1;
        const unsigned long long binkAge = MELEVR::D3DCapture::LastBinkFrameAgeMs();
        const bool binkActive = binkActiveNow;
        const uint64_t flatPresents = MELEVR::RenderHook::PresentsSinceLastPerspective();
        if (flatPresents <= 1) ++s_3dRun; else s_3dRun = 0;   // consecutive presents WITH a live 3D view
        // [MOVIE8] gm 8 + Bink actively decoding = prerendered video, whatever the 3D heartbeat says.
        // The endgame biks keep a live perspective scene rendering BEHIND the video (measured
        // 2026-08-12: a full minute of gm 8 + bink decoding with the scene never going dead), so the
        // flatPresents gate opens late or never and the video plays through the VR fill path -
        // exactly the failure ME2 measured on its startup biks ([STARTMOVIE]: "keeps a live scene
        // rendering behind its prerendered videos"). The mode byte is authoritative in this model:
        // when the engine says Movie AND video frames are arriving, believe it. Sustained a few
        // presents so a one-frame gm blip cannot latch it. In-engine gm-8 staged cutscenes
        // (ME2's Archangel class) decode no Bink, so they are unaffected.
        if (gameMode == 8 && binkActive) { if (s_gm8BinkRun < 1000) ++s_gm8BinkRun; }
        else s_gm8BinkRun = 0;
        const bool movie8 = s_gm8BinkRun >= 8;
        if (!binkActive)                          s_movieOn = 0;   // no decode -> not a movie (fades, gameplay)
        else if (s_movieOn == 0 && (flatPresents >= 4 || movie8)) s_movieOn = 1;   // no 3D, or the engine says Movie
        else if (s_movieOn == 1 && s_3dRun >= 30 && !movie8)      s_movieOn = 0;   // sustained real 3D -> movie over
        inMovie = cfg.movieFlatEnabled && s_movieOn != 0;
        if (s_movieOn != s_lastLogged)
        {
            LogLine("[MOVIE] inMovie=" + std::to_string(inMovie ? 1 : 0) +
                    " s_movieOn=" + std::to_string(s_movieOn) +
                    " binkAgeMs=" + std::to_string(binkAge > 1000000ull ? 1000000ull : binkAge) +
                    " flatPresents=" + std::to_string(flatPresents) +
                    " gm=" + std::to_string(gameMode) +
                    " movie8=" + std::to_string(movie8 ? 1 : 0) +
                    " enabled=" + std::to_string(cfg.movieFlatEnabled ? 1 : 0));
            s_lastLogged = s_movieOn;
            // [LOADCRASH] intermittent GPU-driver crash (nvwgf2umx.dll) around load/loading-screen transitions,
            // 2026-07-12, doesn't reproduce reliably on relaunch. Movie start/end is the best available proxy
            // for "a level just started/finished streaming" (loading screens are Bink movies). Force-sample VRAM
            // budget + device-removed status right at the edge so if it IS VRAM pressure from level-load texture
            // streaming colliding with the backbuffer/eye-texture footprint, the numbers prove it instead of guessing.
            MELEVR::D3DCapture::LogVideoMemory(s_movieOn != 0 ? "movie-start" : "movie-end", true);
            MELEVR::D3DCapture::LogDeviceHealth(s_movieOn != 0 ? "movie-start" : "movie-end");
        }
    }
    // [DEADVETO] scene-alive latch (v2's third rule). ME2 and ME3 both derive this from a
    // CalcSceneView heartbeat; ME1 already publishes the same fact as "presents since the last
    // perspective view", so this is that signal with hysteresis on it. Latched BOTH ways over
    // ~0.5s so a real cutscene that follows a bik gets a brief flat lead-in instead of a flap,
    // and a one-frame blip inside a movie can never declare the scene alive.
    bool sceneAliveNow = true;
    {
        static int  s_aliveRun = 0, s_deadRun = 0;
        static bool s_alive = true;
        const uint64_t flatPresents = MELEVR::RenderHook::PresentsSinceLastPerspective();
        if (flatPresents <= 1) { ++s_aliveRun; s_deadRun = 0; }
        else                   { ++s_deadRun; s_aliveRun = 0; }
        if (s_aliveRun >= 30) s_alive = true;
        if (s_deadRun  >= 30) s_alive = false;
        sceneAliveNow = s_alive;
    }

    // ---- CINE POLICY DISPATCH. One bool, read once, so the version cannot change mid-frame. ----
    CineInputs cineIn;
    cineIn.gameMode     = gameMode;
    cineIn.haveCineMode = haveCineMode;
    cineIn.cineModeName = cineModeName;
    cineIn.binkActive   = binkActiveNow;
    cineIn.sceneAlive   = sceneAliveNow;
    cineIn.inMenuMode   = inMenuMode;
    cineIn.inGalaxyMode = inGalaxyMode;
    cineIn.inMovie      = inMovie;
    cineIn.liveWorldGui = liveWorldGui;
    cineIn.manualFlat   = g_flatCineCopyActive.load(std::memory_order_acquire);
    cineIn.sfr2CfgOn    = cfg.sfr2Enabled && !cfg.stereoEnabled && !cfg.aerEnabled && !cfg.dibrEnabled;
    const CineDecision cine = DecideCine(cineIn, cfg);
    const bool inConversationMode = cine.inConversation;
    const bool inCutsceneMode     = cine.inCutscene;
    const bool vrCineActive       = cine.vrCineActive;
    const bool forceFlatCine      = cine.forceFlatCine;
    // [SFRGATE-LATCH 2026-08-12] Feed the replay gate the LATCHED cine context, not the raw flapping
    // byte (see the note at the old call site above). While a convo/cutscene is latched, gm 7/8/-1
    // blips publish as 5/6 so the second-eye pass keeps rendering; the latch clears instantly on
    // gm 0-4, so gameplay publishes exactly as before.
    // BINK GUARD (same day): NEVER fold the latch while Bink is decoding or a movie is on screen.
    // The first shipped version did, which kept the replay - and therefore perspective renders -
    // alive straight through prerendered videos. The movie detector works by noticing perspective
    // renders have STOPPED (flatPresents), so movies stopped being detected, never went flat, and
    // played through the VR fill path: fully zoomed in, subtitles out of view. Raw byte wins the
    // moment video is involved; the latch fold is for camera-mode blips inside REAL 3D scenes only.
    {
        int effectiveGm = gameMode;
        if (!binkActiveNow && !inMovie)
        {
            if (inConversationMode)  effectiveGm = 5;
            else if (inCutsceneMode) effectiveGm = 6;
        }
        MELEVR::RenderHook::PublishGameModeForSfr(effectiveGm, cfg.cineVrConvo, cfg.cineVrCutscene, liveWorldGui);
    }
    // [VRSTATE] ME2's transition log, ported. One UNCAPPED line per CHANGE of the whole flat/VR
    // decision. ME2 added it after the capped per-marker logs burned their budget at boot and went
    // silent for the exact mid-mission events that needed explaining. Transitions are rare, so this
    // keeps the shipped log short AND guarantees it can never be silent while presentation changes.
    {
        static unsigned s_lastKey = 0xFFFFFFFFu;
        const unsigned key = (static_cast<unsigned>(gameMode & 0xFF)) |
                             (inConversationMode ? 1u << 8  : 0) |
                             (inCutsceneMode     ? 1u << 9  : 0) |
                             (forceFlatCine      ? 1u << 10 : 0) |
                             (vrCineActive       ? 1u << 11 : 0) |
                             (sceneAliveNow      ? 1u << 13 : 0) |
                             (inMovie            ? 1u << 14 : 0);
        if (key != s_lastKey)
        {
            s_lastKey = key;
            LogLine(std::string("[VRSTATE] gm ") + std::to_string(gameMode) +
                    (sceneAliveNow ? " alive" : " dead") +
                    (inMovie ? " movie" : "") +
                    (inConversationMode ? " convo" : "") +
                    (inCutsceneMode ? " cutscene" : "") +
                    " -> " + (forceFlatCine ? "FLAT panel"
                              : vrCineActive ? (cfg.cineHeadTracking ? "VR cine (tracked)" : "VR cine (locked)")
                              : "VR"));
        }
    }
    // [CINEVR2 2026-07-18, final shape] VR cine is OPT-IN per context (EXPERIMENTAL,
    // default OFF = the comfortable flat screen). ON = the context renders through the active mode's
    // plain gameplay pipeline with half-gameplay eye separation and an honest zoom. Movies (Bink
    // video, incl. segments inside hybrid cutscenes), menus and the galaxy map keep their flat
    // screens regardless. The decision itself lives in DecideCine (one function, ME2/ME3 model).
    const bool cinematicContext = inConversationMode || inCutsceneMode;
    {
        // TOGGLE-CHANGE tripwire: logs the INSTANT any flat/mono toggle changes value in the live config,
        // so every menu click is visible in the log with a timestamp. Paired with [FLATWHY].
        static int s_lastToggles = -1;
        const int toggles = (cfg.cineVrConvo ? 2 : 0) | (cfg.menuFlat ? 4 : 0) |
                            (cfg.galaxyFlat ? 8 : 0) | (cfg.movieFlatEnabled ? 16 : 0) |
                            (cfg.fpEnabled ? 32 : 0) | (cfg.cineVrCutscene ? 64 : 0) |
                            0;
        if (toggles != s_lastToggles)
        {
            LogLine("[CFGCHG] cineVrConvo=" + std::to_string(cfg.cineVrConvo ? 1 : 0) +
                    " cineVrCutscene=" + std::to_string(cfg.cineVrCutscene ? 1 : 0) +
                    " menuFlat=" + std::to_string(cfg.menuFlat ? 1 : 0) +
                    " galaxyFlat=" + std::to_string(cfg.galaxyFlat ? 1 : 0) +
                    " movieFlat=" + std::to_string(cfg.movieFlatEnabled ? 1 : 0) +
                    " fpEnabled=" + std::to_string(cfg.fpEnabled ? 1 : 0));
            s_lastToggles = toggles;
        }
    }
    {
        // WHY-flat diagnostic: logs the SOURCE forcing flat whenever the source set changes.
        static int s_lastWhy = -1;
        const int why = (g_flatCineCopyActive.load(std::memory_order_acquire) ? 1 : 0) |
                        ((inConversationMode && !cfg.cineVrConvo) ? 2 : 0) |
                        ((inCutsceneMode && !cfg.cineVrCutscene) ? 4 : 0) |
                        ((inMenuMode && cfg.menuFlat) ? 8 : 0) |
                        ((inGalaxyMode && cfg.galaxyFlat) ? 16 : 0) |
                        (inMovie ? 32 : 0);
        if (why != s_lastWhy)
        {
            LogLine(std::string("[FLATWHY] sources:") +
                    ((why & 1) ? " manual" : "") + ((why & 2) ? " conversation" : "") +
                    ((why & 4) ? " cutscene" : "") + ((why & 8) ? " menu" : "") +
                    ((why & 16) ? " galaxy" : "") + ((why & 32) ? " movie" : "") +
                    ((why == 0) ? " none" : ""));
            s_lastWhy = why;
        }
    }
    {
        // Menu/galaxy context tripwire: log ONLY on state edges, so the log shows that mode==7/9
        // fires exactly on open/close and never lingers into gameplay (no mono-bleed).
        static int s_lastCtx = -2;
        const int ctx = inMenuMode ? 7 : (inGalaxyMode ? 9 : 0);
        if (ctx != s_lastCtx)
        {
            LogLine("[MENUCTX] inMenu=" + std::to_string(inMenuMode ? 1 : 0) +
                    " inGalaxy=" + std::to_string(inGalaxyMode ? 1 : 0) +
                    " gameMode=" + std::to_string(gameMode) +
                    " menuFlat=" + std::to_string(cfg.menuFlat ? 1 : 0) +
                    " galaxyFlat=" + std::to_string(cfg.galaxyFlat ? 1 : 0) +
                    " forceFlat=" + std::to_string(forceFlatCine ? 1 : 0));
            s_lastCtx = ctx;
        }
    }
    {
        static char s_lastCine[64] = {1, 0};   // force first log
        if (std::strcmp(s_lastCine, cineModeName) != 0)
        {
            LogLine(std::string("[CINECTX] mode='") + (cineModeName[0] ? cineModeName : "?") +
                    "' convo=" + std::to_string(inConversationMode ? 1 : 0) +
                    " cutscene=" + std::to_string(inCutsceneMode ? 1 : 0) +
                    " forceFlat=" + std::to_string(forceFlatCine ? 1 : 0));
            std::strncpy(s_lastCine, cineModeName, sizeof(s_lastCine) - 1);
            s_lastCine[sizeof(s_lastCine) - 1] = '\0';
        }
    }
    // Cinematic flat: clear any leftover per-eye view shift so the mono quad has no residual offset.
    if (forceFlatCine)
    {
        MELEVR::RenderHook::SetAerState(false, 0, 0.0f, false);
        MELEVR::RenderHook::SetViewOffset(0.0f, 0.0f, 0.0f);
    }

    // Display-locked cadence (the flicker fix): hold the present to a stable integer fraction of the headset
    // refresh BEFORE waitFrame. AER: display/2 (each eye legitimately persists 2 display refreshes - that's
    // the eye-swap's own design, proven fix). STEREO: the game free-runs its own ~60fps limiter
    // unsynchronized with the headset, so the phase drifts -> periodic beat = flicker during sustained
    // head motion.
    // HISTORY (both prior constants were wrong at some refresh rates): display/2 capped a 72Hz headset at
    // 36fps (an earlier finding); its replacement 1.0 set an 8.3ms target at 120Hz - below
    // the game's own ~16.7ms frame, so the hold NEVER engaged = Bug 4's flicker silently back the next
    // morning while the logs still read "paced". Stereo now measures its natural cadence and locks to the
    // integer period count it actually occupies (120Hz->2, 60Hz->1), or deliberately declines when no
    // integer lock exists (72/90Hz -> free-run, no throttle). See StereoLockPeriods.
    {
        const MELEVR::Config::VrConfig& paceCfg = MELEVR::Config::Get();
        const bool aerPaceActive = paceCfg.aerEnabled && !paceCfg.stereoEnabled && !paceCfg.dibrEnabled &&
                                   !forceFlatCine && paceCfg.aerFramePacing;
        const bool stereoPaceActive = paceCfg.stereoEnabled && !paceCfg.aerEnabled && !paceCfg.dibrEnabled &&
                                      !forceFlatCine && paceCfg.stereoFramePacing;
        // Stereo 2 (2026-07-15): same cadence physics as Stereo - every present carries a FRESH full
        // render for both eyes, free-running on the game's own ~60fps limiter, so the same phase-drift
        // beat flickers during sustained head motion. Mode 4 was simply never added to this gate when
        // Stereo 2 became a first-class mode (it rode Mono's pipeline, which never paced). Shares
        // stereoFramePacing (default on) and the measured StereoLockPeriods ratio - no new levers.
        const bool sfr2PaceActive = paceCfg.sfr2Enabled && !forceFlatCine && paceCfg.stereoFramePacing;
        // [CINEPACE60 2026-08-12] STAGED CUTSCENES pace to display/2 (60fps at 120Hz), always.
        // The flyby judder timeline finally lined up: cutscenes were confirmed smooth on 08-11
        // while the engine's own SmoothFrameRate capped the game at 60; the judder came back "100%"
        // the same day that cap was removed for gameplay fps. Cutscene cameras animate on the game
        // tick and were authored against <=60fps - at ~119 the camera cannot produce a unique pose
        // per frame, so the CONTENT steps, which no tag/pacing/capture fix can mask (every delivery
        // metric was verified clean while it juddered). Every flat playthrough ever watched these
        // scenes at <=60. Gameplay keeps the full rate.
        // [CINEPACE-CONVO 2026-08-12] Convo cine paces too. "Conversations (static shots) keep the
        // full rate" was wrong: one cinematic flips gm 5<->6 mid-scene, and the 12:58 run measured
        // the gm-5 stretches free-running 113-118 presents/s against the 120Hz refresh (gm-6 held a
        // clean 60 and was reported smooth in the same scene). 118 != 120 means a frame doubles at
        // an irregular beat - the same unpaced-cadence judder the stereo pacer exists to kill, and
        // the reason ME2 (pacing always-on) never shows it. vrCineActive is bit-identical to the
        // old inCutsceneMode gate for cutscenes (a flat cine already forces forceFlatCine) and
        // simply adds the convo lane; flat-presented cine and gameplay are untouched.
        const bool cinePace60 = paceCfg.cineFpsCap && vrCineActive && !forceFlatCine;
        // [CINEPACE-ADAPT 2026-08-12] The fixed 60fps cap has no answer for shots the GPU cannot
        // hold at 60: the comm-room screen segment measured avgMs=16.67 maxMs=17.32 headroom=-0%
        // (pinned exactly at the budget; ordinary cutscene shots run 8-12ms) - frames landing a
        // hair late slip a refresh, the compositor alternates 2-and-3-period cadence, and that
        // IRREGULAR mix is felt as judder from the moment the heavy shot cuts in. This is the
        // stereo pacer's documented failure (2): a regular N+1 cadence reprojects smoothly, the
        // mix does not. So the cine cap now steps 2 -> 3 periods (locked 40fps) when the observed
        // work misses the 2-period slot in >=6 of a 60-present window (2ms grace, the stereo
        // pacer's own margin), and back to 2 after a clean window. 12-present echo skip on cine
        // entry so load transients (maxMs spikes at scene starts) cannot trip the first window.
        static int s_cpObs = 0, s_cpMiss = 0, s_cpSkip = 12, s_cpPeriods = 2;
        if (!cinePace60) { s_cpObs = 0; s_cpMiss = 0; s_cpSkip = 12; s_cpPeriods = 2; }
        if (cinePace60)
        {
            if (g_paceLocked && g_lastPaceWorkSec > 0.0)
            {
                if (s_cpSkip > 0) { --s_cpSkip; }
                else
                {
                    ++s_cpObs;
                    if (g_lastPaceWorkSec > 2.0 * g_displayPeriodSec - 0.002) ++s_cpMiss;
                    if (s_cpObs >= 60)
                    {
                        if (s_cpPeriods == 2 && s_cpMiss >= 6)
                        {
                            s_cpPeriods = 3;
                            LogLine("[CINEPACE] work missed the 2-period slot " + std::to_string(s_cpMiss) +
                                    "/60 -> holding 3 periods (regular cadence)");
                        }
                        else if (s_cpPeriods == 3 && s_cpMiss <= 2)
                        {
                            s_cpPeriods = 2;
                            LogLine("[CINEPACE] work fits the 2-period slot again (" + std::to_string(s_cpMiss) +
                                    "/60 misses) -> back to 2 periods");
                        }
                        s_cpObs = 0; s_cpMiss = 0;
                    }
                }
            }
            PaceDisplayLockedAer(static_cast<double>(s_cpPeriods));
        }
        else if (aerPaceActive) PaceDisplayLockedAer(paceCfg.fullRefreshPacing ? 1.0 : 2.0);
        else if (stereoPaceActive || sfr2PaceActive)
        {
            const double lockPeriods = StereoLockPeriods();
            // adaptiveObserve feeds the pre-hold work time back to [STEREOPACE v2] so the lock can
            // step up during sustained load and probe back down when headroom returns.
            if (lockPeriods > 0.0) PaceDisplayLockedAer(lockPeriods, true);
        }
    }

    XrFrameWaitInfo fwi = {};
    fwi.type = XR_TYPE_FRAME_WAIT_INFO_VALUE;
    XrFrameState fs = {};
    fs.type = XR_TYPE_FRAME_STATE_VALUE;
    if (!XrSucceeded(g_fn.waitFrame(g_session, &fwi, &fs))) return;

    // Learn the headset's true refresh (runtime FB query first, else measured) so the pace above locks correctly.
    // Runs for AER always (pacing is core there) and for stereo when its opt-in pacing is on.
    // [CINEPACE60-ARM 2026-08-12] ...and whenever the cutscene cap could ever be asked to hold. The
    // 08-12 retest proved [CINEPACE60] had never run once: PaceDisplayLockedAer refuses to hold until
    // g_paceLocked, only this warmup sets it, and the gate below never included the cine cap - so in
    // Stereo 2 with stereoFramePacing=0 (the shipped default) the refresh was never learned and every
    // cutscene free-ran at 118-120 presents/s while [CINEJIT] showed a perfectly clean delivery. The
    // exact "configured but doing nothing" family the [PACE] instrument documents. The refresh is a
    // session property, so learn it up front rather than only inside a cutscene (warmup needs frames).
    {
        const MELEVR::Config::VrConfig& paceCfg = MELEVR::Config::Get();
        const bool aerMode = paceCfg.aerEnabled && !paceCfg.stereoEnabled && !paceCfg.dibrEnabled;
        const bool stereoPace = paceCfg.stereoEnabled && !paceCfg.aerEnabled && !paceCfg.dibrEnabled &&
                                paceCfg.stereoFramePacing;
        const bool sfr2Pace = paceCfg.sfr2Enabled && paceCfg.stereoFramePacing;   // Stereo 2 shares the lock
        const bool cinePaceArm = paceCfg.cineFpsCap && paceCfg.sfr2Enabled;       // [CINEPACE60] needs the refresh too
        if ((aerMode || stereoPace || sfr2Pace || cinePaceArm) && !forceFlatCine)
            UpdateAerPaceWarmup(fs.predictedDisplayTime, paceCfg.aerFramePacingHz);
    }

    XrFrameBeginInfo fbi = {};
    fbi.type = XR_TYPE_FRAME_BEGIN_INFO_VALUE;
    g_fn.beginFrame(g_session, &fbi);

    // --- Options menu: lazy-init (Present thread), Insert toggle ------------------------------------
    if (!g_menuInited)
    {
        MELEVR::Menu::Init(g_device, g_context, kMenuW, kMenuH);
        MELEVR::Menu::InstallInputBlock();   // hook XInput so the game gets a neutral pad while the menu is open
        DXGI_SWAP_CHAIN_DESC scd = {};
        if (gameSwapChain != nullptr && SUCCEEDED(gameSwapChain->GetDesc(&scd)) && scd.OutputWindow != nullptr)
        {
            MELEVR::Menu::SetGameWindow(scd.OutputWindow);   // subclass the window to swallow mouse+kb while open
        }
        MELEVR::Pchud::Install();   // PCHUD Scaleform HUD move/scale hook (idempotent; Tick retries if not ready)
        g_menuInited = true;
    }
    const bool insDown = (cfg.menuKey > 0) && ((GetAsyncKeyState(cfg.menuKey) & 0x8000) != 0);
    if (insDown && !g_insertKeyPrev) MELEVR::Menu::Toggle();
    g_insertKeyPrev = insDown;
    // First-run: pop the menu open once so new users land on the welcome / HDR-off notice.
    {
        static bool s_firstRunForced = false;
        if (!s_firstRunForced && g_menuInited && !cfg.firstRunDone) { s_firstRunForced = true; MELEVR::Menu::SetOpen(true); }
    }
    const bool menuOpen = MELEVR::Menu::IsOpen();   // RenderFrame self-polls mouse + the real pad - no FeedGamepad needed

    g_recenterKey = cfg.recenterKey;
    // PCHUD Scaleform HUD move/scale (own feature, off by default). Push the live values + drive the apply.
    // Per-mode layout: stereo draws the UI per-eye (half width) so it uses its own *Stereo geometry set.
    const bool useStereoUi = cfg.stereoEnabled;
    const bool useDibrUi = cfg.dibrEnabled;   // DIBR owns its own PCHUD master layout (Stereo owns its own; AER/Mono share normal)
    MELEVR::Pchud::SetTransform(cfg.pchudEnabled,
                                useDibrUi ? cfg.pchudScaleXDibr  : (useStereoUi ? cfg.pchudScaleXStereo  : cfg.pchudScaleX),
                                useDibrUi ? cfg.pchudScaleYDibr  : (useStereoUi ? cfg.pchudScaleYStereo  : cfg.pchudScaleY),
                                useDibrUi ? cfg.pchudOffsetXDibr : (useStereoUi ? cfg.pchudOffsetXStereo : cfg.pchudOffsetX),
                                useDibrUi ? cfg.pchudOffsetYDibr : (useStereoUi ? cfg.pchudOffsetYStereo : cfg.pchudOffsetY));
    MELEVR::Pchud::SetConvoTransform(cfg.convoEnabled,
                                useDibrUi ? cfg.convoScaleXDibr  : (useStereoUi ? cfg.convoScaleXStereo  : cfg.convoScaleX),
                                useDibrUi ? cfg.convoScaleYDibr  : (useStereoUi ? cfg.convoScaleYStereo  : cfg.convoScaleY),
                                useDibrUi ? cfg.convoOffsetXDibr : (useStereoUi ? cfg.convoOffsetXStereo : cfg.convoOffsetX),
                                useDibrUi ? cfg.convoOffsetYDibr : (useStereoUi ? cfg.convoOffsetYStereo : cfg.convoOffsetY));
    // Designer UI (mission/boss scripted overlays - e.g. Saren's charge counter): one set of values, no
    // per-mode split - this is an occasional overlay, not a persistent per-mode HUD layout.
    MELEVR::Pchud::SetDesignerUiTransform(cfg.designerUiEnabled, cfg.designerUiScaleX, cfg.designerUiScaleY,
                                          cfg.designerUiOffsetX, cfg.designerUiOffsetY);
    MELEVR::Pchud::SetLaserUiTransform(cfg.laserUiEnabled, cfg.laserUiScaleX, cfg.laserUiScaleY,
                                       cfg.laserUiOffsetX, cfg.laserUiOffsetY);   // [LASERUI]
    MELEVR::Pchud::SetSubtitleMode(cfg.subtitleForce, cfg.subtitleMode);
    MELEVR::Pchud::SetSubtitleRedraw(cfg.subtitleRedraw, cfg.subtitleHideOriginal,
                                     useDibrUi ? cfg.subtitlePosXFracDibr : (useStereoUi ? cfg.subtitlePosXFracStereo : cfg.subtitlePosXFrac),
                                     useDibrUi ? cfg.subtitlePosYFracDibr : (useStereoUi ? cfg.subtitlePosYFracStereo : cfg.subtitlePosYFrac),
                                     useDibrUi ? cfg.subtitleScaleXDibr   : (useStereoUi ? cfg.subtitleScaleXStereo   : cfg.subtitleScaleX),
                                     useDibrUi ? cfg.subtitleScaleYDibr   : (useStereoUi ? cfg.subtitleScaleYStereo   : cfg.subtitleScaleY));
    MELEVR::Pchud::SetNativeSubtitleMove(cfg.nativeSubtitleEnabled, cfg.nativeSubtitlePosXFrac, cfg.nativeSubtitlePosYFrac,
                                         cfg.nativeSubtitleScaleX, cfg.nativeSubtitleScaleY, cfg.nativeSubtitleFontSize);
    PushElementConfigs(cfg);
    MELEVR::Pchud::Tick();
    // Push the live depth-map tuning to the greyscale viz each frame (Enhancements > Depth map).
    MELEVR::D3DCapture::SetDepthMapTuning(cfg.depthMapNear, cfg.depthMapFar, cfg.depthMapFlip, cfg.depthMapGamma);
    // DIBR auto-convergence: lock the zero-disparity plane onto the SUBJECT (screen-center) depth each frame, so the
    // thing you're looking at sits on the screen plane and barely warps -> no disocclusion ghost on it (that lateral
    // silhouette double). The background still takes the disparity. Low-passed so the depth doesn't pulse as the
    // subject depth jitters. With auto off, the manual Convergence slider is used.
    static float s_dibrAutoConv = 0.985f;
    {
        float pcC = 0.0f, ptlC = 0.0f, pbrC = 0.0f, ptrC = 0.0f;
        MELEVR::D3DCapture::GetDepthProbe(&pcC, &ptlC, &pbrC, &ptrC);
        // low-pass the subject depth for BOTH DIBR and relief auto-convergence (either may want it)
        // Relief auto-converge is per-mode now; run the low-pass if EITHER set wants it (it is just a filter).
        if ((cfg.dibrAutoConverge || cfg.reliefAutoConverge || cfg.reliefAutoConvergeAer) && pcC > 0.0f)
        {
            // [RELIEFCONV] 2026-07-15: "you can see the depth converging" while moving. The EMA's
            // first-frame step on a big subject-depth jump (walk toward/past something) is large enough to
            // watch the pop plane slide. SLEW CLAMP on top of the EMA: convergence keeps tracking during
            // motion (the requirement) but per-frame movement is bounded to an imperceptible glide - a big retarget
            // becomes a slow drift instead of a visible catch-up. 0.0004 raw/frame ~ full typical subject
            // jump (0.01-0.02 raw) spread over 0.4-0.8s.
            float step = 0.08f * (pcC - s_dibrAutoConv);
            constexpr float kMaxConvStepPerFrame = 0.0004f;
            if (step > kMaxConvStepPerFrame) step = kMaxConvStepPerFrame;
            else if (step < -kMaxConvStepPerFrame) step = -kMaxConvStepPerFrame;
            s_dibrAutoConv += step;
            // 1/s instrument: if converging is still visible, the log names whether the probe is jumping
            // (subject changes) or stalling-then-snapping (readback starvation during heavy frames).
            static ULONGLONG s_convLogMs = 0;
            const ULONGLONG convNow = GetTickCount64();
            if (convNow - s_convLogMs >= 1000)
            {
                s_convLogMs = convNow;
                char b[128];
                std::snprintf(b, sizeof(b), "[RELIEFCONV] probeC=%.5f conv=%.5f step=%+.5f", pcC, s_dibrAutoConv, step);
                LogLine(b);
            }
        }
    }
    const float dibrEffConv = cfg.dibrAutoConverge ? s_dibrAutoConv : cfg.depthWarpConv;
    // Push the depth-WARP effect tuning (the stereo strength/convergence/sign + far-only hold the grid warp uses).
    MELEVR::D3DCapture::SetDibrWarp(cfg.depthWarpGain, dibrEffConv, cfg.depthWarpFlip);
    // [RELIEF M0]: the dev probe also enables depth capture in Stereo/AER, purely to
    // MEASURE that the draw-gated capture works in those modes (SBS depth in stereo / per-frame full depth
    // in AER). No warp, no visual change (greyscale is a separate toggle), DIBR behavior untouched - the
    // submit chain checks stereo/AER BEFORE IsDibrStereoReady, so enabling capture can't hijack the submit.
    // [RELIEF M1] enable depth capture in stereo whenever the relief warp is active (strength>0), the dev
    // probe is on, or the greyscale is shown. The warp reads the published depth; SetReliefWarp pushes the
    // live tunables. Convergence: reuse the DIBR auto-conv low-pass (tracks the subject) or the manual slider.
    // Stereo and AER keep SEPARATE depth-pop values (they warp differently). ActiveRelief picks the live set.
    const MELEVR::Config::ReliefParams rp = MELEVR::Config::ActiveRelief(cfg);

    // [RELIEF_XITION] Crash root-caused 2026-07-11 from a user log: depth pop + a convo/cutscene transition
    // (forceFlatCine flips) -> the depth pipeline visibly stalls mid-transition (copy rate dropped ~900/s to
    // ~75/s, probe went asymmetric: L=0.35594 R=0.17328), then a crash landed ~850ms after the mode flipped
    // BACK to stereo (badAddr 0x100000108 - same 0x100000000 base garbage pointer as an earlier crash, just a
    // different field offset - the same corrupted reference surfacing twice). Same class as the already-known
    // "mismatched/unpopulated depth buffer during a transition" driver crash, just landing in the game's own
    // script VM this time instead of nvwgf2umx.dll. Detect the forceFlatCine edge (either direction) and
    // suspend depth capture/warp for a cooldown window so the render-target state has time to settle before
    // relief touches it again. Does NOT touch cfg.reliefEnabled - this is a transient safety suspension, same
    // pattern as the existing "depth pop always boots off" rule, not a permanent disable.
    static bool s_relXitionPrevFlat = false;
    static ULONGLONG s_relXitionCooldownUntilMs = 0;
    // An earlier reported crash was also after a cutscene, but not immediately - not the
    // sub-second gap measured in tonight's log. Widened from 2000ms to give real margin for a
    // slower-to-surface version of the same bug rather than tuning strictly to one measurement.
    constexpr ULONGLONG kRelXitionCooldownMs = 5000;
    // [CLEANVRCINE] cine no longer flips forceFlatCine, but the cine ENTER/EXIT render-target churn
    // (deferred-context scene swap) is the same transition the cooldown was built for - key the edge
    // on flat-or-cine so depth capture still stands down across convo/cutscene boundaries.
    const bool relXitionNow = forceFlatCine || cinematicContext;
    if (relXitionNow != s_relXitionPrevFlat)
    {
        s_relXitionCooldownUntilMs = GetTickCount64() + kRelXitionCooldownMs;
        LogLine(std::string("[RELIEF_XITION] flat/cine ") + (s_relXitionPrevFlat ? "1->0" : "0->1") +
                " - suspending depth capture for " + std::to_string(kRelXitionCooldownMs) + "ms");
        // [LOADCRASH] 2026-07-12: a DIFFERENT nvwgf2umx.dll crash (0xC0000005 reading -1/0x0, not the
        // 0x100000000-base garbage pointer above) happened with depth pop OFF, during SFXCameraMode_Interpolate
        // (a load/cinematic transition - forceFlatCine flips exactly here). Doesn't reproduce reliably on
        // relaunch. Sample VRAM + device-removed status at this same edge (independent of depth capture) so
        // if it's VRAM pressure from level-streaming colliding with the backbuffer/eye-texture footprint, or
        // an actual TDR, the numbers show it instead of another guess.
        MELEVR::D3DCapture::LogVideoMemory(forceFlatCine ? "cine-enter" : "cine-exit", true);
        MELEVR::D3DCapture::LogDeviceHealth(forceFlatCine ? "cine-enter" : "cine-exit");
    }
    // BUG (2026-07-18, found from an 11,553-line RELIEF_XITION spam over one ~80min session):
    // this MUST track relXitionNow (the actual comparison value), not forceFlatCine alone. Writing
    // forceFlatCine here left the tracked state permanently stuck outside any VR convo/cutscene (where
    // cinematicContext=true but forceFlatCine=false), so relXitionNow != s_relXitionPrevFlat was TRUE
    // EVERY FRAME for the whole scene - the expensive LogVideoMemory/LogDeviceHealth pair (already
    // proven elsewhere in this file to cause gameplay hitches at even 1/sec) fired at full framerate
    // for the entire duration of every VR convo/cutscene, and depth-pop stayed force-suspended the
    // whole time too. This is almost certainly the low-fps report from that release.
    s_relXitionPrevFlat = relXitionNow;
    const bool reliefXitionCooling = GetTickCount64() < s_relXitionCooldownUntilMs;

    const bool reliefWarpActive = !reliefXitionCooling &&
        (cfg.stereoEnabled || cfg.aerEnabled || cfg.sfr2Enabled) && cfg.reliefEnabled &&
        (rp.strength > 0.0001f || rp.darkStrength > 0.0001f || rp.unsharpStrength > 0.0001f);   // darken/unsharp alone keep the pass (and depth capture) alive
    const bool reliefVizOn = MELEVR::D3DCapture::GetDepthVizShow();
    const bool reliefProbeActive = !reliefXitionCooling && (cfg.reliefProbe || reliefWarpActive || reliefVizOn) &&
                                   (cfg.stereoEnabled || cfg.aerEnabled || cfg.sfr2Enabled);
    MELEVR::D3DCapture::SetDepthMapEnabled(cfg.dibrEnabled || reliefProbeActive);
    {
        const float reliefConv = rp.autoConverge ? s_dibrAutoConv : rp.convergence;
        MELEVR::D3DCapture::SetReliefWarp(rp.strength, reliefConv, rp.flip ? -1.0f : 1.0f,
                                          rp.curve, rp.edgeGuard, rp.nearFreeze,
                                          rp.darkStrength, rp.darkRadius,
                                          rp.unsharpStrength, rp.unsharpRadius);
        // greyscale viz window: manual (park it to read the depth range) or auto-fit.
        MELEVR::D3DCapture::SetDepthVizAutoFit(!cfg.reliefVizManual);
        if (cfg.reliefVizManual)
            MELEVR::D3DCapture::SetDepthMapTuning(cfg.depthMapNear, cfg.depthMapFar, cfg.depthMapFlip, cfg.depthMapGamma);
    }
    if (reliefProbeActive)
    {
        static ULONGLONG s_reliefLastMs = 0;
        static unsigned  s_reliefLastCopies = 0;
        const ULONGLONG reliefNow = GetTickCount64();
        if (s_reliefLastMs == 0) s_reliefLastMs = reliefNow;
        else if (reliefNow - s_reliefLastMs >= 2000)
        {
            unsigned dw = 0, dh = 0, copies = 0;
            MELEVR::D3DCapture::GetDepthCaptureStats(&dw, &dh, &copies);
            float pc2 = 0, ptl2 = 0, pbr2 = 0, ptr2 = 0, plc = 0, prc = 0;
            MELEVR::D3DCapture::GetDepthProbe(&pc2, &ptl2, &pbr2, &ptr2);
            MELEVR::D3DCapture::GetDepthProbeLR(&plc, &prc);
            const double capHz = (copies >= s_reliefLastCopies)
                ? (static_cast<double>(copies - s_reliefLastCopies) * 1000.0 / static_cast<double>(reliefNow - s_reliefLastMs))
                : 0.0;
            // [RELIEF M0.3] draw-table snapshot SORTED desc (top 6): the real scene depth = the highest
            // draw count. And the attribution meter: if draws>>attrib, the depth is bound by a path not
            // don't hook (the stereo undercount tell).
            void* tblRes[6] = {}; unsigned tblDraws[6] = {};
            const int tblN = MELEVR::D3DCapture::GetDepthDrawTableSnapshot(tblRes, tblDraws, 6);
            char tblStr[224] = "";
            for (int ti = 0; ti < tblN; ++ti)
            {
                char one[40];
                sprintf_s(one, "%s%p:%u", ti ? " " : "", tblRes[ti], tblDraws[ti]);
                strcat_s(tblStr, one);
            }
            unsigned drTot = 0, drAttr = 0;
            MELEVR::D3DCapture::GetDrawAttribStats(&drTot, &drAttr);
            char reliefBuf[512];
            sprintf_s(reliefBuf, "[RELIEF] mode=%s depth=%ux%u copies=%u (%.1f/s) draws=%u attrib=%u probe C=%.5f L=%.5f R=%.5f TL=%.5f BR=%.5f tbl=[%s]",
                      cfg.stereoEnabled ? "stereo" : "aer", dw, dh, copies, capHz, drTot, drAttr, pc2, plc, prc, ptl2, pbr2, tblStr);
            LogLine(reliefBuf);
            s_reliefLastMs = reliefNow;
            s_reliefLastCopies = copies;
        }
    }
    // Same-frame stereo (P1 layout + SBS submit). forceFlatCine (menu/galaxy/movie flat) drops it;
    // [CLEANVRCINE] conversations/cutscenes stay ON it - cine renders like gameplay.
    const bool stereoActive = cfg.stereoEnabled && !forceFlatCine;
    // AER: alternate-eye geometric stereo (mutually exclusive with stereo/DIBR; stays on in cine too).
    const bool aerActive = cfg.aerEnabled && !cfg.stereoEnabled && !cfg.dibrEnabled && !forceFlatCine;
    if (!aerActive) ResetAerHistory();   // free the history bank + reset the toggle whenever AER isn't the active mode
    if (!stereoActive) MELEVR::RenderHook::SetP1LayoutStereo(false, 0.0f, false, 1.0f, 1.0f);
    // Stereo 2 (SFR): same-frame double render. Mutually exclusive like the others; renders through the
    // mono pipeline (full-frame passes), so it must never coexist with P1 SBS stereo.
    const bool sfr2Active = cfg.sfr2Enabled && !cfg.stereoEnabled && !cfg.aerEnabled && !cfg.dibrEnabled &&
                            !forceFlatCine;
    // [CINESEP] Cine separation scale. This was a BAKED 0.5 - every VR convo/cutscene rendered at
    // half gameplay disparity with no knob - on the theory that close-range face shots at full
    // separation are an eye workout. Measured against ME2 on 2026-08-09: ME2 has NO cine separation
    // scaling anywhere in its source, so its cine renders at the same tuned half-eye as gameplay.
    // Halving ME1's is why ME1 cine reads flatter than ME2's side by side. Now cfg.cineSepScale,
    // default 1.0 = ME2 parity, on a slider so the old 0.5 is one drag away if the strain was real.
    // Render-only; submit pose tags stay at true headset IPD.
    const float cineSepMul = vrCineActive ? cfg.cineSepScale : 1.0f;
    // [SWAPME2 2026-07-20] ME2-parity swap-eyes: ME2 flips the RENDER cameras when swap is on
    // (calcview: `if (swapEyes) he = -he`) AND reroutes images at submit - each eye still gets a
    // correctly-matched viewpoint, the toggle just exchanges which eye receives which render pass
    // (temporal-freshness/pass artifacts). ME1 previously only rerouted at submit = pseudoscopic
    // depth inversion ("world inside-out"), which is why swap felt nothing like ME2's.
    MELEVR::RenderHook::SetSfrEnabled(sfr2Active, cfg.sfr2HalfEyeUU * cineSepMul *
                                                      (cfg.sfr2SwapEyes ? -1.0f : 1.0f));
    // [SFRCONV] publish convergence alongside separation. Scaled by the SAME cine multiplier so a
    // reduced-separation cine keeps a matched fusion-plane pull (at the 1.0 default this is a no-op).
    MELEVR::RenderHook::SetSfrConvergence(cfg.sfr2Convergence * cineSepMul);
    // [CINEVR2] Stereo 2 in a VR convo/cutscene: the game renders cine through a DIFFERENT post
    // chain (deferred-context scene, no gameplay composite shader), so the normal SFR pass-0 capture
    // boundary never fires there. Arm the cine boundary (backbuffer-batch trigger in d3d_capture) so
    // the pair keeps publishing instead of freezing on the last gameplay image.
    MELEVR::D3DCapture::SetSfrCineBoundary(sfr2Active && cinematicContext);
    // [NOLETTERBOX] while a VR cine is live in Stereo 2, clear LE1's cine-camera 16:9 aspect
    // constraint each frame at Draw entry (render_hook), so the cine renders FULL-FRAME exactly
    // like gameplay - which is what LE2/LE3 cine always did and why they never needed a band crop,
    // a UI squash, or an aspect probe. Flat cine (movies) keeps engine-default behaviour.
    MELEVR::RenderHook::SetCineUnconstrain(sfr2Active && vrCineActive);
    // Per-eye UI dup runs on the game draw thread; give it a cheap flag for "SBS stereo is the active
    // submit path" so it only fires in stereo (not AER/mono/DIBR, which render one full-frame eye).
    MELEVR::D3DCapture::SetStereoUiActive(stereoActive);
    MELEVR::D3DCapture::SetStereoUiYShift(cfg.stereoUiYShift);

    // (The render-side view offset = static camera shift + screen distance + LEAN is computed below, after
    //  the head pose is located, so lean can use the live head position.)

    // --- Recenter (re-origin the app space to current head) ----------------------------------------
    const bool rDown = cfg.recenterKeyEnabled && (g_recenterKey > 0) && ((GetAsyncKeyState(g_recenterKey) & 0x8000) != 0);
    if ((rDown && !g_recenterKeyPrev) || MELEVR::Menu::ConsumeRecenterRequest() ||
        MELEVR::Menu::ConsumeRecenterCombo() || !g_haveRecentered)
    {
        if (DoRecenter(fs.predictedDisplayTime))
        {
            g_haveRecentered = true;
            g_recenterReLatchAim = true;
            g_smoothPoseReset = true;   // app space changed: old-space smoothed quat + tag history are invalid
            g_tagHistReset = true;
        }
    }
    g_recenterKeyPrev = rDown;

    // --- First-person toggle key (default 'K', rebindable) -----------------------------------------
    {
        static bool s_fpKeyPrev = false;
        const int fpKey = cfg.fpToggleKey;
        const bool fpKeyDown = cfg.fpToggleKeyEnabled && (fpKey > 0) && ((GetAsyncKeyState(fpKey) & 0x8000) != 0);
        if (fpKeyDown && !s_fpKeyPrev)
        {
            MELEVR::Config::VrConfig& mutCfg = MELEVR::Config::Get();
            mutCfg.fpEnabled = !mutCfg.fpEnabled;
            LogLine(std::string("[FPCAM] toggle key -> fpEnabled=") + (mutCfg.fpEnabled ? "1" : "0"));
        }
        s_fpKeyPrev = fpKeyDown;
    }

    // --- Depth-pop toggle key (default 'P', rebindable) --------------------------------------------
    // Depth pop (relief warp) boots OFF every launch (AER flicker + startup-crash safety), so this is the
    // opt-in-per-session hotkey. Toggles reliefEnabled; only Stereo/AER actually warp, harmless elsewhere.
    {
        static bool s_depthPopKeyPrev = false;
        const int dpKey = cfg.depthPopKey;
        const bool dpKeyDown = cfg.depthPopKeyEnabled && (dpKey > 0) && ((GetAsyncKeyState(dpKey) & 0x8000) != 0);
        if (dpKeyDown && !s_depthPopKeyPrev)
        {
            MELEVR::Config::VrConfig& mutCfg = MELEVR::Config::Get();
            mutCfg.reliefEnabled = !mutCfg.reliefEnabled;
            LogLine(std::string("[RELIEF] toggle key -> reliefEnabled=") + (mutCfg.reliefEnabled ? "1" : "0"));
        }
        s_depthPopKeyPrev = dpKeyDown;
    }

    // --- Profile-switch hotkeys (one VK per profile slot, global map) -------------------------------
    // Press a mapped key -> live-load that profile (same as clicking its menu button). Edge-detected so a
    // held key fires once; a key already loading the ACTIVE profile is a no-op. Menu-open state is ignored
    // deliberately: switching profiles from the pad/menu still works, and the keys are opt-in per slot.
    if (MELEVR::Config::ProfileHotkeysEnabled())
    {
        static bool s_profKeyPrev[8] = {false, false, false, false, false, false, false, false};
        const int pc = MELEVR::Config::ProfileCount();
        for (int i = 0; i < pc && i < 8; ++i)
        {
            const int vk = MELEVR::Config::ProfileHotkey(i);
            const bool down = (vk > 0) && ((GetAsyncKeyState(vk) & 0x8000) != 0);
            if (down && !s_profKeyPrev[i] && i != MELEVR::Config::ActiveProfileIndex())
            {
                MELEVR::Config::LoadProfile(i);
                LogLine(std::string("[PROFILEHOTKEY] key -> loaded profile ") +
                        MELEVR::Config::ProfileName(i));
            }
            s_profKeyPrev[i] = down;
        }
    }

    // --- Head pose + look-around drive (centered against the re-origined space) ---------------------
    const bool headValid = LocateHead(fs.predictedDisplayTime);

    // UNIFIED head-orientation smoothing (2026-07-12, the last render-vs-tag mismatch): the old per-axis
    // Euler low-pass smoothed only the angles fed to SetHeadLook, while the submit tag stayed RAW - so at
    // low turn speeds (smoothing engaged) the pixels lagged the tag, the compositor over-corrected, and the
    // image wobbled/flickered until the filter converged ("flicker before stabilizing" on slow/ending turns;
    // fast turns pass through 1:1 = smooth, which is exactly what was reported). Fix: smooth the QUAT
    // once, speed-adaptively, and derive BOTH the render angles AND the submit tag from this one pose -
    // mismatch is structurally zero at every speed and every smoothing setting.
    static XrQuaternionf s_smoothQuat = IdentityPose().orientation;
    static bool s_smoothInit = false;
    if (headValid)
    {
        const XrQuaternionf& rawQ = g_subViews[0].pose.orientation;
        const MELEVR::Config::VrConfig& smCfg = MELEVR::Config::Get();
        if (!s_smoothInit || g_smoothPoseReset || smCfg.headLookSmoothing <= 0.001f)
        {
            s_smoothQuat = rawQ;
            s_smoothInit = true;
            g_smoothPoseReset = false;
        }
        else
        {
            // [ME2SMOOTH 2026-08-12] Literal ME2 adaptive curve. ME1's 0.4-deg cutoff disabled
            // smoothing at ordinary 40-85 deg/s cine turns (0.67-1.42 deg per 60-Hz sample), exactly
            // where [LOOKSTEP] measured the lurches. ME2 keeps smoothing through 2 deg/sample and is
            // the known-smooth reference under the same paced-cine conditions.
            const float baseFollow = 1.0f - smCfg.headLookSmoothing * 0.95f;
            const float speedDeg = QuatAngleDeg(s_smoothQuat, rawQ);
            const float ramp = (speedDeg > 2.0f) ? 1.0f : (speedDeg * 0.5f);
            const float follow = baseFollow + (1.0f - baseFollow) * ramp;
            s_smoothQuat = NlerpQuat(s_smoothQuat, rawQ, follow);
        }
    }
    else
    {
        s_smoothInit = false;
    }
    const XrQuaternionf headQuatForFrame =
        (headValid && s_smoothInit) ? s_smoothQuat : (headValid ? g_subViews[0].pose.orientation
                                                                : IdentityPose().orientation);

    // Stage 1: controller input frame (same thread/timebase as the head pose).
    // Publishes the right-ray aim source and virtual-pad snapshot; no-op when
    // not ready. M1 consumes the ray below at the existing DriveAimWithHead
    // site; virtual-pad synthesis remains deferred to M2 (vr_menu XInput hook).
    if (MELEVR::XrInput::IsReady())
        MELEVR::XrInput::OnFrame(g_appSpace, fs.predictedDisplayTime, headQuatForFrame);

    bool headLookApplied = false;
    bool headAimApplied = false;
    int weaponModeForLog = -2;
    char weaponModeNameForLog[64] = {};
    float yawDegForLog = 0.0f;
    float pitchDegForLog = 0.0f;

    // [INVMAT] push the gated inverse-slot fix state (menu toggle, default OFF - probes always run).
    MELEVR::RenderHook::SetInverseFix(cfg.invMatrixFix);

    // Head tracking is READ-ONLY (strip-firstperson-livecode 2026-07-03) with ONE hard-gated write exception:
    // HEAD-AIM. When cfg.combatHeadAim is on and EVERY gate passes (headValid && !menuOpen &&
    // EnsureController && ControllerStable && !forceFlatCine, re-checked each frame), the HMD drives the
    // game's ControlRotation additively with the stick (DriveAimWithHead) and render-side head-look is parked
    // at zero (no double rotation). Any gate failing mid-flight releases: subtract the applied delta once IF
    // the controller is live+unchanged, otherwise abandon without writing. Everything else (weapon-mode read,
    // menu-open ControlRotation pin, probes) stays dead - the menu camera-freeze holds via the input block.
    // Controller gates computed ONCE per frame (ControllerStable mutates quarantine state - must not
    // run twice), shared by head-aim, its release, and the first-person tick. Zero traffic when nothing
    // needs it.
    const bool ctrlNeeded = cfg.combatHeadAim || g_headAimActive ||
                            cfg.fpEnabled || MELEVR::HeadAim::FirstPersonOwnsAny();
    const bool ctrlLive = ctrlNeeded && MELEVR::HeadAim::EnsureController();
    const bool ctrlStable = ctrlLive && MELEVR::HeadAim::ControllerStable();

    // RECENTER RE-LATCH: a recenter this frame re-origined the VIEW (g_appSpace) but not the head-aim
    // reference. Strip the accumulated ControlRotation injection now (returns aim to the raw game/stick
    // value) and reset the reference; the head-aim branch below re-activates fresh against the new forward,
    // so aim re-zeros. This is what makes recenter actually rescue a stuck pitch/yaw once decoupled pitch/
    // yaw has removed the stick escape hatch. restoreWrite honors the live+stable controller (no stale write).
    if (g_recenterReLatchAim)
    {
        if (g_headAimActive) ReleaseHeadAim(ctrlLive && ctrlStable);
        g_recenterReLatchAim = false;
    }
    // FIRST PERSON tick runs UNCONDITIONALLY (2026-07-08 fix: it lived inside the else-if below, so
    // "disable head tracking in convo/cutscene" - or just opening the Insert menu - froze it out and
    // the head-hide CLEAR never ran on cine entry = headless Shepard in conversations). It self-gates on
    // fpEnabled and releases/restores through menus, convos and cutscenes.
    if (cfg.fpEnabled || MELEVR::HeadAim::FirstPersonOwnsAny())
        MELEVR::HeadAim::FirstPersonTick(ctrlLive, ctrlStable, gameMode);

    // HEAD TRACKING IS PARKED IN EVERY CINE (2026-08-02). ME2's locked-cine rule, now the only
    // cine behaviour: the head does not turn the director's camera. The shot renders as framed and
    // the pair is anchored to the current head at submit, so a cine is a stable 3D picture you sit
    // inside. This also removes the render-vs-tag pose-mismatch class the tracked variant produced.
    if (menuOpen || (cinematicContext && (forceFlatCine || !cfg.cineHeadTracking)))
    {
        // [CINEVR2] park head-look/aim when the Insert menu is open, when cine is on the flat
        // screen (VR toggle off / movie segment), or when cine head tracking is switched off -
        // the scripted shot renders untouched in all three. VR cine with tracking ON skips this
        // branch entirely: you look around inside the scene like gameplay.
        if (g_headAimActive)
        {
            ReleaseHeadAim(ctrlLive && ctrlStable);
        }
        MELEVR::RenderHook::SetHeadLook(0, 0, false);
    }
    else if (gameMode == 1)
    {
        // MAKO default: stick aims, head-look parked so the crosshair matches the cannon. Usable, shippable.
        // Experimental Mako head-aim is deliberately one variable only: ControlRotation via DriveAimWithHead,
        // with render-side head-look parked and NO turret bone / relative / ProcessEvent writes stacked on top.
        if (cfg.makoHeadAim && cfg.combatHeadAim && headValid && !forceFlatCine && ctrlLive && ctrlStable)
        {
            float yawDeg = 0.0f, pitchDeg = 0.0f, rollDeg = 0.0f;
            HeadEulerDegrees(g_subViews[0].pose.orientation, yawDeg, pitchDeg, rollDeg);
            yawDegForLog = yawDeg;
            pitchDegForLog = pitchDeg;

            const float aimYaw = cfg.invertAimYaw ? -yawDeg : yawDeg;
            const float aimPitch = cfg.invertAimPitch ? -pitchDeg : pitchDeg;
            DriveAimWithHead(aimYaw, aimPitch);
            headAimApplied = true;

            static uint64_t s_makoAimLog = 0;
            if ((s_makoAimLog++ % 60ull) == 0)
            {
                LogLine("[MAKOAIM] ControlRotation-only head aim active head=(" +
                        std::to_string(yawDeg) + "," + std::to_string(pitchDeg) + ")");
            }
        }
        else if (g_headAimActive)
        {
            ReleaseHeadAim(ctrlLive && ctrlStable);
        }
        MELEVR::RenderHook::SetHeadLook(0, 0, false);
    }
    else if (headValid)
    {
        // Angles come from the UNIFIED smoothed quat (see block after LocateHead): the same pose that becomes
        // the submit tag. The old per-axis Euler low-pass lived here and smoothed ONLY these angles while the
        // tag stayed raw - that render-vs-tag mismatch was the "flicker before stabilizing" on slow/ending
        // turns. Smoothing now happens once, upstream, on the quat; render and tag can never disagree again.
        float yawDeg = 0.0f, pitchDeg = 0.0f, rollDeg = 0.0f;
        HeadEulerDegrees(headQuatForFrame, yawDeg, pitchDeg, rollDeg);
        yawDegForLog = yawDeg;
        pitchDegForLog = pitchDeg;


        // WEAPON-STATE SWITCH (restored 2026-07-07): head-AIM only when the weapon is OUT; unarmed falls through
        // to head-LOOK (render-side 6DOF free look). This is the auto-switch that broke when the weapon-mode
        // reader left with game_camera. 1 = weapon out, 0 = unarmed, -1 = unreadable (treat as unarmed = 6DOF).
        const int weaponMode = MELEVR::HeadAim::ReadWeaponModeSEH();
        weaponModeForLog = weaponMode;
        const bool weaponOut = (weaponMode == 1);

        // HEAD-AIM (the single live-write exception). Gates re-checked EVERY frame; EnsureController's fast
        // path revalidates slot identity at the instant of use, ControllerStable holds a 90-frame quarantine
        // after any controller transition. Only touched when the toggle is on or a release is owed - the
        // feature fully off means zero object-table traffic.
        bool aimDriven = false;
        // (ctrlLive/ctrlStable hoisted above the head-tracking gate 2026-07-08 - shared with the FP
        // tick, which must run even when convo/cutscene head-tracking-disable or the menu skips this block.)
        if (cfg.combatHeadAim || g_headAimActive)
        {
            // STORM = sprint (SFXCameraMode_CombatStorm / ExploreStorm). Head-aim writes ControlRotation,
            // but the sprint does NOT steer off it 1:1 - head-aiming while storming sent the character
            // running opposite to the look (2026-07-15 + re-confirmed 2026-07-16: "look left, view
            // goes left but the character moves right"). You can't fire mid-sprint anyway, so storm uses
            // free head-LOOK instead (render-side view rotation only, ControlRotation untouched) and the
            // [MOVEFIX] stick rotation (vr_menu XInput hook) steers the sprint to follow the head. This
            // block was first built 2026-07-15 and discarded because the stick rotation half was missing -
            // look-only storm still FEELS inverted (view turns, run heading doesn't). The pair is the fix.
            // STORM DETECTION WITH HYSTERESIS. The game flaps Combat<->CombatStorm rapidly during ONE
            // sustained sprint (log: CombatStorm->Interpolate->Combat->Interpolate->CombatStorm every 1-3s
            // while the stick is held). Switching head-aim<->head-look on every flap snapped the view, so this
            // LATCH storm off the STICK (the thing that's actually steady during a sprint) and ignore the
            // mode oscillation: enter on the storm camera or a hard forward stick, hold while the stick stays
            // pushed, drop only after it's released for a beat. The stick also bridges the ~1s Combat->Storm
            // blend so the switch is instant. Stationary ADS (stick ~neutral) never trips this, so aim + the
            // ADS-snap fix are untouched.
            const int stickMag = MELEVR::Menu::LeftStickMagnitude();
            const bool modeStorm = (std::strstr(cineModeName, "Storm") != nullptr);
            const bool inInterp  = (std::strstr(cineModeName, "Interpolate") != nullptr);
            static bool s_stormLatched = false;
            static int  s_stormReleaseFrames = 0;
            // ENTER only on the storm CAMERA, or the sprint-start transition blend (Interpolate + hard stick).
            // Plain movement/walking stays in Combat (never Interpolate-to-storm), so a high stick alone must
            // NOT enter storm - that flipped aim<->look every time you just walked (2026-07-15 regression).
            // A definitive AIMING camera (ADS/scope/cover-aim) while latched = the player is aiming, not
            // sprinting - drop the latch INSTANTLY so head-aim owns the crosshair even with the stick still
            // pushed (walking-ADS out of a sprint). The game never enters these modes mid-storm on its own.
            const bool modeAiming = (std::strstr(cineModeName, "TightAim") != nullptr) ||
                                    (std::strstr(cineModeName, "Sniper") != nullptr) ||
                                    (std::strstr(cineModeName, "HipAimCover") != nullptr);
            // [DRAWFIX 2026-07-18] Drawing the weapon WHILE MOVING ran the draw camera blend
            // (Interpolate) with the stick pushed hard - indistinguishable, to the enter condition
            // below, from a sprint-start blend. The latch false-entered, then HELD (stick stays
            // pushed while walking), so head-aim stayed excluded: view head-rotated, ControlRotation
            // stuck at body-forward = "weapon aim wrong until you stop moving" (FP).
            // Fix: the unarmed->drawn edge is an AIM-INTENT signal - clear the latch and suppress the
            // INTERPOLATE-based enter for the blend's duration (~2s). The real-storm enter (the actual
            // Storm camera) is deliberately NOT suppressed, so a genuine combat sprint still latches
            // instantly, and the mid-sprint Combat<->Storm flap hysteresis is untouched (no 0->1 draw
            // edge occurs mid-combat - the weapon is already out).
            static int s_weaponPrevForLatch = 0;
            static int s_drawGraceFrames = 0;
            if (weaponOut && s_weaponPrevForLatch != 1)
            {
                if (s_stormLatched) LogLine("[HEADAIM] storm latch cleared by weapon draw (aim intent).");
                s_stormLatched = false;
                s_stormReleaseFrames = 0;
                s_drawGraceFrames = 120;   // ride out the draw blend's Interpolate frames
            }
            s_weaponPrevForLatch = weaponMode;
            if (s_drawGraceFrames > 0) --s_drawGraceFrames;
            if (modeAiming)
            {
                s_stormLatched = false;
                s_stormReleaseFrames = 0;
            }
            else if (modeStorm || (inInterp && stickMag > 24000 && s_drawGraceFrames == 0))
            {
                s_stormLatched = true;
                s_stormReleaseFrames = 0;
            }
            else if (s_stormLatched)
            {
                // Once sprinting, HOLD through the game's Combat<->CombatStorm mode flaps while the stick stays
                // pushed; drop only after it's released for a beat.
                if (stickMag < 12000) { if (++s_stormReleaseFrames > 12) s_stormLatched = false; }
                else s_stormReleaseFrames = 0;
            }
            const bool isStorm = s_stormLatched;
            // Mako is handled in the gameMode==1 branch above. Keep this guard so a clean mode-byte read never
            // falls through into the normal on-foot aim path. When controller aim is requested, require a
            // currently valid right-hand aim pose before entering this branch; otherwise fall back to the
            // ordinary head-look path instead of pinning the camera while the controller is lost.
            const bool controllerAimActive = cfg.controllerAim && MELEVR::XrInput::AimActive();
            static bool s_controllerAimSource = false;
            if (controllerAimActive != s_controllerAimSource)
            {
                // DriveAimWithHead is additive and keeps a previous source's
                // reference/injection. Remove that injection before latching
                // the new source, otherwise enabling the controller can add a
                // one-frame fixed offset (the reported ~45 degree jump).
                if (g_headAimActive) ReleaseHeadAim(ctrlLive && ctrlStable);
                s_controllerAimSource = controllerAimActive;
                LogLine(std::string("[HEADAIM] source -> ") +
                        (controllerAimActive
                             ? "controller (HMD camera decoupled)"
                             : "head"));
            }
            const bool controllerAimReady = !cfg.controllerAim || controllerAimActive;
            const bool aimGatesPass = cfg.combatHeadAim && weaponOut && !isStorm && !forceFlatCine &&
                                      ctrlLive && ctrlStable && gameMode != 1 && controllerAimReady;
            if (aimGatesPass)
            {
                // Head or right-controller ray aims via the same sanctioned
                // ControlRotation path (1:1, no sensitivity scaling). If the
                // controller frame is unavailable, GetAimDeg leaves the head
                // source in place: fail-safe is the old head-aim behaviour.
                float aimSrcYaw = yawDeg, aimSrcPitch = pitchDeg;
                // The source gate above already verified a valid controller
                // frame; GetAimDeg still owns the output write/fail-safe check.
                const bool aimSourceAvailable = MELEVR::XrInput::GetAimDeg(&aimSrcYaw, &aimSrcPitch);
                yawDegForLog = aimSrcYaw;
                pitchDegForLog = aimSrcPitch;
                const float aimYaw = cfg.invertAimYaw ? -aimSrcYaw : aimSrcYaw;
                float aimPitch = cfg.invertAimPitch ? -aimSrcPitch : aimSrcPitch;
                // Controller aim must not import the HMD render offset into
                // its ControlRotation handoff. Head aim keeps the established
                // render-to-CR continuity ramp.
                DriveAimWithHead(aimYaw, aimPitch, !controllerAimActive);
                // Native head aim normally leaves orientation following the aim
                // (SetHeadLook 0), so the camera center and reticle stay aligned.
                // Controller aim is the deliberate exception: its branch below
                // keeps the render-side HMD look active while ControlRotation
                // follows the right ray. combatCamHold (below, in the view-offset
                // block) remains independent and only addresses the camera arc.
                // [AIMSEED v2] during a head-look->aim handoff, the untransferred
                // remainder is still rendered as head-look so the two halves sum
                // to the full offset while ControlRotation glides to the gaze.
                // It also keeps [MOVEFIX] consistent via HeadLookYawUU.
                if (aimSourceAvailable)
                {
                    // Controller aim changes ControlRotation for the game's
                    // weapon/character aim, but it must not turn the HMD view.
                    // ControlRotation can also affect the game's base camera,
                    // so cancel only this frame's controller injection in the
                    // render-side look offset. The signs match the existing
                    // head-aim handoff: +ControlRotation yaw is equivalent to
                    // -render-look yaw, while pitch has the same sign.
                    HeadLookUU look = HeadLookFromAngles(yawDeg, pitchDeg, cfg);
                    look.yaw += g_appliedHeadYawUU;
                    look.pitch -= g_appliedHeadPitchUU;
                    MELEVR::RenderHook::SetHeadLook(look.yaw, look.pitch, true);
                }
                else if (g_seedRemYawUU != 0 || g_seedRemPitchUU != 0)
                {
                    MELEVR::RenderHook::SetHeadLook(-g_seedRemYawUU, g_seedRemPitchUU, true);
                }
                else
                {
                    MELEVR::RenderHook::SetHeadLook(0, 0, false);   // aim owns the rotation - no render-side look on top
                }
                headAimApplied = true;
                aimDriven = true;
            }
            else if (g_headAimActive)
            {
                // Restore cleanly (remove the injected head offset so ControlRotation returns to the game's
                // stick-driven value); head-look then owns the view. Fires on storm entry (once per sprint,
                // thanks to the hysteresis above) and on stale/changed controller (abandon, no write).
                ReleaseHeadAim(ctrlLive && ctrlStable);
            }
        }

        if (!aimDriven)
        {
            if (cfg.headLookEnabled)
            {
                // Explore free look-around in CalcSceneView (render-side only).
                const HeadLookUU look = HeadLookFromAngles(yawDeg, pitchDeg, cfg);
                MELEVR::RenderHook::SetHeadLook(look.yaw, look.pitch, true);
                headLookApplied = true;
            }
            else
            {
                MELEVR::RenderHook::SetHeadLook(0, 0, false);
            }
        }

        // [FPSTORM] diagnostic (2026-07-15, restored with the fix 2026-07-16): storm head-tracking path.
        // aim=0 look=1 during a storm = the exclusion is working; moveYawUU is the [MOVEFIX] stick-rotation
        // angle the XInput hook is applying (0 = movement not being steered). One glance at a sprint repro
        // says which half (view path / movement steer) misbehaves. Fires only for storm/blend modes; throttled.
        if (std::strstr(cineModeName, "Storm") != nullptr || std::strstr(cineModeName, "Interpolate") != nullptr)
        {
            static uint64_t s_fpStormLog = 0;
            if ((s_fpStormLog++ % 20ull) == 0)
                LogLine(std::string("[FPSTORM] mode='") + cineModeName +
                        "' weaponMode=" + std::to_string(weaponMode) +
                        " aim=" + std::to_string(headAimApplied ? 1 : 0) +
                        " look=" + std::to_string(headLookApplied ? 1 : 0) +
                        " stick=" + std::to_string(MELEVR::Menu::LeftStickMagnitude()) +
                        " moveYawUU=" + std::to_string(cfg.moveFollowsHead ? MELEVR::RenderHook::HeadLookYawUU() : 0) +
                        " fp=" + std::to_string(cfg.fpEnabled ? 1 : 0));
        }
    }
    else
    {
        // No valid head pose: hold still. Release any in-flight aim injection (controller-verified).
        if (g_headAimActive)
        {
            const bool ctrlLive = MELEVR::HeadAim::EnsureController();
            ReleaseHeadAim(ctrlLive && MELEVR::HeadAim::ControllerStable());
        }
        MELEVR::RenderHook::SetHeadLook(0, 0, false);
    }

    // --- Head orientation + position (roll removed; shared by both eyes' pose tag) -------------------
    XrQuaternionf headOrient = IdentityPose().orientation;
    XrVector3f headPos = {0.0f, 0.0f, 0.0f};
    // [CINEVR2 zoom v2] when the cine zoom is engaged, the game RENDERS the narrowed (tighter) shot
    // but the submit DECLARES the unzoomed fill window - real lens magnification, constant window,
    // and the UI overlay (wheel/subtitles, drawn full-frame after the 3D) keeps its exact unzoomed
    // size and shape. v1 declared the narrowed window honestly and was rejected in testing: it
    // stretched the conversation wheel, because the cropped window takes the headset's
    // TALL aspect (measured 71x73 deg) and warps everything drawn into it.
    float cineZoomDeclH = 0.0f, cineZoomDeclV = 0.0f;
    bool cineZoomDeclare = false;
    if (headValid)
    {
        // Tag from the SAME unified smoothed quat the render angles came from ("tag what you rendered") -
        // NOT the raw sample. This is what makes the smoothing invisible to the compositor at every speed.
        headOrient = QuatRemoveRoll(headQuatForFrame);
        headPos.x = (g_subViews[0].pose.position.x + g_subViews[1].pose.position.x) * 0.5f;
        headPos.y = (g_subViews[0].pose.position.y + g_subViews[1].pose.position.y) * 0.5f;
        headPos.z = (g_subViews[0].pose.position.z + g_subViews[1].pose.position.z) * 0.5f;

        const XrFovf& leftFov = g_subViews[0].fov;
        const XrFovf& rightFov = g_subViews[1].fov;
        // Conversations AND cutscenes use the SAME cinematic fill multipliers (decoupled from gameplay VR Fill H/V,
        // which otherwise drags the cinematic black bars + subtitle/wheel UI around). The convo controls work for
        // cutscenes too; the separate cutscene fill was removed because it didn't.
        // Cinematics (cutscenes/conversations) use the SAME fill as gameplay now. Gameplay renders square (1:1) and
        // looks correct; the old cinematic 0.75/0.50 fill forced the cutscene's render WIDE = the squash. Everything
        // gets gameplay's fill -> cinematics render square like gameplay -> no squash.
        if (forceFlatCine)
        {
            // Flat cine/menu/movie only: FOV-fill override off = the game's NATIVE projection.
            // A VR cine now takes the [VRCINE FILL] path below (render AND declare the headset window).
            MELEVR::RenderHook::SetTargetFovHalfAngles(0.0f, 0.0f, false);
        }
        else
        {
            // [CINEVR2] cine-scoped VR Fill: during a VR convo/cutscene the fill multipliers come
            // from the CINE pair (confirmed the fill mechanism shapes the cine window exactly
            // right, then asked for it cine-scoped so gameplay keeps its own tuning). Identical
            // code path to gameplay fill - same ClampVrFill, same everything.
            // [VRCINE FILL] the fill pair comes from the cine policy now. v1 hands back the cine-
            // the GAMEPLAY pair, so the window is constant across gameplay, every shot cut and
            // every scene. There is no cine-scoped fill any more.
            const float fillH = cine.fillH;
            const float fillV = cine.fillV;
            float targetHalfH = std::fmax(std::fmax(-leftFov.angleLeft, leftFov.angleRight),
                                          std::fmax(-rightFov.angleLeft, rightFov.angleRight)) *
                                ClampVrFill(fillH);
            float targetHalfV = std::fmax(std::fmax(leftFov.angleUp, -leftFov.angleDown),
                                          std::fmax(rightFov.angleUp, -rightFov.angleDown)) *
                                ClampVrFill(fillV);
            // [CINEVR2 zoom v2] cine lens zoom: RENDER the narrowed FOV (tan/zoom = a real tighter
            // shot), DECLARE the (cine-fill-sized) unzoomed window at submit - uniform magnification
            // of the 3D scene while the full-frame UI overlay stays bit-identical to zoom=1. The
            // declared/rendered gap was directly tested for the old doubling and falsified as its
            // cause (2026-07-17, in-headset); the real causes are all fixed now.
            if (vrCineActive)
            {
                const float zoom = cine.cineZoom;   // validated in the cine policy
                if (std::fabs(zoom - 1.0f) > 0.005f)
                {
                    cineZoomDeclH = targetHalfH;   // declare THESE (the cine-fill, unzoomed window)...
                    cineZoomDeclV = targetHalfV;
                    cineZoomDeclare = true;
                    targetHalfH = std::atan(std::tan(targetHalfH) / zoom);   // ...render these
                    targetHalfV = std::atan(std::tan(targetHalfV) / zoom);
                }
            }
            MELEVR::RenderHook::SetTargetFovHalfAngles(targetHalfH, targetHalfV, cfg.vrFovFillEnabled);
        }
    }
    else
    {
        MELEVR::RenderHook::SetTargetFovHalfAngles(0.0f, 0.0f, false);
    }

    float halfH = 0.0f, halfV = 0.0f;
    const bool haveRenderFov = MELEVR::RenderHook::GetRenderFovHalfAngles(&halfH, &halfV);
    // Keep compositor FOV matched to the rendered projection. VR Fill H/V changes the render hook's target
    // projection before CalcSceneView, then this submit path reports that same rendered FOV to OpenXR.

    // Flat cinematics are rendered as an active 16:9 image. Declare the same FOV the render hook
    // actually produced; widening this back to the full eye texture re-stretches the letterboxed band.

    {
        static int s_xrDeclLogCount = 0;
        static char s_xrDeclLastMode[64] = {};
        const bool modeChanged = std::strcmp(s_xrDeclLastMode, cineModeName) != 0;
        const bool cinematicMode =
            std::strstr(cineModeName, "Conversation") != nullptr ||
            std::strstr(cineModeName, "Cinematic") != nullptr;
        if (modeChanged || cinematicMode || s_xrDeclLogCount < 12 || (s_xrDeclLogCount % 180) == 0)
        {
            const float hDeg = haveRenderFov ? halfH * 2.0f * 57.29578f : 0.0f;
            const float vDeg = haveRenderFov ? halfV * 2.0f * 57.29578f : 0.0f;
            const float aspect = (haveRenderFov && std::tan(halfH) > 0.0001f) ? (std::tan(halfV) / std::tan(halfH)) : 0.0f;
            LogLine(std::string("[XRDECL] mode='") + (cineModeName[0] != '\0' ? cineModeName : "?") +
                    "' forceFlat=" + (forceFlatCine ? "1" : "0") +
                    " haveRenderFov=" + (haveRenderFov ? "1" : "0") +
                    " halfH=" + std::to_string(halfH) +
                    " halfV=" + std::to_string(halfV) +
                    " hFovDeg=" + std::to_string(hDeg) +
                    " vFovDeg=" + std::to_string(vDeg) +
                    " aspect=" + std::to_string(aspect));
            std::strncpy(s_xrDeclLastMode, cineModeName, sizeof(s_xrDeclLastMode) - 1);
            s_xrDeclLastMode[sizeof(s_xrDeclLastMode) - 1] = '\0';
        }
        ++s_xrDeclLogCount;
    }

    XrPosef renderTagPose = IdentityPose();
    renderTagPose.orientation = headOrient;
    renderTagPose.position = headPos;
    XrFovf renderTagFov = {};
    if (haveRenderFov)
    {
        renderTagFov.angleLeft = -halfH;
        renderTagFov.angleRight = halfH;
        renderTagFov.angleUp = halfV;
        renderTagFov.angleDown = -halfV;
    }
    else if (headValid)
    {
        renderTagFov = g_subViews[0].fov;
    }
    else
    {
        renderTagFov.angleLeft = -0.45f;
        renderTagFov.angleRight = 0.45f;
        renderTagFov.angleUp = 0.45f;
        renderTagFov.angleDown = -0.45f;
    }

    // Galaxy-only readability zoom. Conversations/cutscenes in full VR intentionally skip the former
    // cinematic submit-FOV override: their declared FOV must match what both gameplay-path eyes rendered.
    if (!forceFlatCine && headValid && inGalaxyMode)
    {
        const float zoom = cfg.galaxyVrZoom;
        const float zoomLo = 0.25f;
        if (zoom > zoomLo && zoom < 4.0f && std::fabs(zoom - 1.0f) > 0.001f)
        {
            renderTagFov.angleLeft  = std::atan(std::tan(renderTagFov.angleLeft)  * zoom);
            renderTagFov.angleRight = std::atan(std::tan(renderTagFov.angleRight) * zoom);
            renderTagFov.angleUp    = std::atan(std::tan(renderTagFov.angleUp)    * zoom);
            renderTagFov.angleDown  = std::atan(std::tan(renderTagFov.angleDown)  * zoom);
        }
    }

    // [XRFOVGUARD / STEAMVR 2026-07-20] declared-FOV floor + last-good hold. renderTagFov feeds EVERY
    // projection-layer declaration below, and during loads/transitions ME1 renders degenerate
    // projections (the log shows declared halfH walking 27deg -> 24deg -> 0.0000). Meta's compositor
    // IGNORES declared FOV (the [LINKFOV] discovery) so this passed silently for weeks; SteamVR honors
    // it and displays a zero-solid-angle layer as NOTHING - "only the SteamVR waiting room". Never
    // declare thinner than ~4deg; hold the last healthy declaration instead (the compositor keeps
    // showing the last sane frame rather than the void).
    {
        static XrFovf s_lastGoodFov = {-0.45f, 0.45f, 0.45f, -0.45f};
        constexpr float kMinHalf = 0.035f;   // ~2 deg per side
        const bool fovDegenerate =
            !std::isfinite(renderTagFov.angleLeft) || !std::isfinite(renderTagFov.angleRight) ||
            !std::isfinite(renderTagFov.angleUp) || !std::isfinite(renderTagFov.angleDown) ||
            (renderTagFov.angleRight - renderTagFov.angleLeft) < 2.0f * kMinHalf ||
            (renderTagFov.angleUp - renderTagFov.angleDown) < 2.0f * kMinHalf;
        if (fovDegenerate)
        {
            static std::atomic<unsigned> s_degLogs{0};
            const unsigned n = s_degLogs.fetch_add(1, std::memory_order_relaxed);
            if (n < 8 || (n % 600) == 0)
                LogLine("[XRFOVGUARD] degenerate rendered FOV (L=" + std::to_string(renderTagFov.angleLeft) +
                        " R=" + std::to_string(renderTagFov.angleRight) +
                        " U=" + std::to_string(renderTagFov.angleUp) +
                        " D=" + std::to_string(renderTagFov.angleDown) + ") -> holding last good");
            renderTagFov = s_lastGoodFov;
        }
        else
        {
            s_lastGoodFov = renderTagFov;
        }
    }

    // --- Frame copy: flat mono ------------------------------------------------------------------------
    // The game's backbuffer goes to BOTH eyes (identical image). Depth is flat; immersion comes from the
    // head tracking + lean + the world-locked, roll-removed submit pose below.
    bool submitted = false;
    bool sfrPairSubmitted = false;  // [FREEZETAG] the captured-pair path fed the eyes this present (fresh OR held);
                                    // false when the hold expired and the submit fell through to the live mono
                                    // path - live pixels must never inherit a frozen tag.
    bool usedSharpStereo = false;   // Stage-1: true when the sharp 1:1 stereo path filled g_stereoEyes this frame
    float flatCineQuadAspect = 16.0f / 9.0f;
    if (aerActive && fs.shouldRender == 0 && gameSwapChain != nullptr)
    {
        // [AERGAP] OpenXR declined composition, but ME1 already rendered an eye before Present.
        // Advance the render handshake and preserve that eye in history so the next game frame
        // renders the opposite eye. No XR swapchain is acquired on this path.
        ID3D11Texture2D* gapBackBuffer = nullptr;
        if (SUCCEEDED(gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                               reinterpret_cast<void**>(&gapBackBuffer))) &&
            gapBackBuffer != nullptr)
        {
            SubmitAerFrame(cfg, gapBackBuffer, renderTagPose, false);
            gapBackBuffer->Release();
        }
    }
    if (fs.shouldRender != 0 && gameSwapChain != nullptr)
    {
        ID3D11Texture2D* backBuffer = nullptr;
        if (SUCCEEDED(gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) &&
            backBuffer != nullptr)
        {
            D3D11_TEXTURE2D_DESC bbDesc = {};
            backBuffer->GetDesc(&bbDesc);
            g_lastBackbufferWidth.store(bbDesc.Width, std::memory_order_relaxed);
            g_lastBackbufferHeight.store(bbDesc.Height, std::memory_order_relaxed);
            // HDR watch: a float backbuffer (R16G16B16A16_FLOAT) means Windows/game HDR is ON, which gives the
            // blue/doubled headset image. Flag it so the menu can warn the user in-headset.
            g_hdrDetected.store(bbDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT, std::memory_order_relaxed);
            const float backbufferAspect = (bbDesc.Height > 0) ? (static_cast<float>(bbDesc.Width) / static_cast<float>(bbDesc.Height)) : 1.0f;
            const float eyeAspect = (g_eyes[0].height > 0) ? (static_cast<float>(g_eyes[0].width) / static_cast<float>(g_eyes[0].height)) : 1.0f;
            // [TARGETCHK 2026-08-09] One-shot: does the LIVE render target actually match the Stereo 2
            // resolution the request expects? It silently did NOT on 2026-08-09 - sfr2ResX/Y only
            // reaches the game when the mod writes GamerSettings.ini on a mode pick, and the real
            // target is read from GamerSettings at DLL attach (before MELEVR.ini is loaded at all).
            // So a config edit alone left a 6144x6144 SQUARE live while the source said 6144x3456,
            // and that cost a whole headset test. Cine correctness depends on this: LE1 constrains
            // the cine camera to 16:9, so a non-16:9 target letterboxes every cutscene, and the
            // 2026-08-09 rip deliberately removed the crop that used to correct for that.
            if (cfg.sfr2Enabled && bbDesc.Width > 0 && bbDesc.Height > 0 && cfg.sfr2ResY > 0)
            {
                static bool s_targetChecked = false;
                if (!s_targetChecked)
                {
                    s_targetChecked = true;
                    const float wantAspect = static_cast<float>(cfg.sfr2ResX) / static_cast<float>(cfg.sfr2ResY);
                    const bool matches = (bbDesc.Width == static_cast<unsigned>(cfg.sfr2ResX) &&
                                          bbDesc.Height == static_cast<unsigned>(cfg.sfr2ResY));
                    LogLine(std::string("[TARGETCHK] Stereo 2 target ") + (matches ? "OK" : "MISMATCH") +
                            ": live=" + std::to_string(bbDesc.Width) + "x" + std::to_string(bbDesc.Height) +
                            " aspect=" + std::to_string(backbufferAspect) +
                            " configured=" + std::to_string(cfg.sfr2ResX) + "x" + std::to_string(cfg.sfr2ResY) +
                            " aspect=" + std::to_string(wantAspect) +
                            (matches ? "" : "  <-- fix ResX/ResY in ME1/BioGame/Config/GamerSettings.ini;"
                                            " a non-16:9 target letterboxes every cinematic"));
                }
            }
            float stereoSourceVScale = 1.0f;
            if (eyeAspect > 0.3f && eyeAspect < 3.0f && backbufferAspect > 0.5f && backbufferAspect < 4.0f)
            {
                stereoSourceVScale = 0.5f * backbufferAspect / eyeAspect;
                if (stereoSourceVScale > 1.0f) stereoSourceVScale = 1.0f;
                if (stereoSourceVScale < 0.1f) stereoSourceVScale = 0.1f;
            }
            MELEVR::RenderHook::SetP1LayoutStereo(stereoActive,
                                                  cfg.stereoHalfEyeUU * cineSepMul,   // [CINEVR2] half sep in VR cine
                                                  cfg.stereoSwapEyes,
                                                  eyeAspect, backbufferAspect);

            // [BANDPROBE] sample the RAW backbuffer (before the relief paint / warp below can touch it).
            BandProbeTick(backBuffer, bbDesc, stereoActive, aerActive, forceFlatCine, menuOpen,
                          yawDegForLog, pitchDegForLog, weaponModeForLog, headLookApplied, headAimApplied);

            // [RELIEF M0] greyscale-in-headset: the flat-lab TryPresentDepthViz call runs AFTER the eye
            // copies (mirror-only). Painting here - before the copies - puts the captured depth in the
            // HEADSET: in stereo each eye shows its own SBS half (the verification visual), in AER the
            // full frame. Probe toggle + greyscale toggle both required; never fires in flat/mono/DIBR.
            if (reliefProbeActive && (stereoActive || aerActive) && MELEVR::D3DCapture::GetDepthVizShow())
                MELEVR::D3DCapture::PaintDepthVizNow(gameSwapChain);

            if (forceFlatCine)
            {
                // [CINERIP] the eye holds the frame as rendered; the 16:9 target already matches the
                // cine camera's own constraint, so the quad is simply the backbuffer's aspect.
                flatCineQuadAspect = backbufferAspect;
                const bool copiedL = CopyTextureToEyeFullFrame(0, backBuffer);
                const bool copiedR = CopyTextureToEyeFullFrame(1, backBuffer);
                submitted = copiedL || copiedR;
                static int s_flatMonoLogCount = 0;
                if (s_flatMonoLogCount < 12 || (s_flatMonoLogCount % 120) == 0)
                {
                    const float monoWidth = inMenuMode ? cfg.menuMonoQuadWidth
                                          : (inGalaxyMode ? cfg.galaxyMonoQuadWidth
                                          : (inConversationMode ? cfg.convoMonoQuadWidth : cfg.cutsceneMonoQuadWidth));  // movies use the cutscene width now
                    LogLine(std::string("[CINEMONO] mode='") + (cineModeName[0] != '\0' ? cineModeName : "?") +
                            "' fullFrame=1 headLockedQuad=1 src=" +
                            std::to_string(bbDesc.Width) + "x" + std::to_string(bbDesc.Height) +
                            " aspect=" + std::to_string(flatCineQuadAspect) +
                            " quadWidth=" + std::to_string(monoWidth));
                }
                ++s_flatMonoLogCount;
            }
            else if (stereoActive)
            {
                // [RELIEF M1] if the depth pop is on, warp the SBS backbuffer (per-eye depth-driven shift)
                // and submit THAT; else submit the raw backbuffer. strength 0 / depth-not-ready -> raw = plain
                // stereo, byte-identical. The warped tex is SBS-shaped, so CopySbsHalves handles it unchanged.
                ID3D11Texture2D* stereoSrc = backBuffer;
                if (reliefWarpActive && MELEVR::D3DCapture::IsReliefDepthReady())
                {
                    ID3D11Texture2D* warped = MELEVR::D3DCapture::RenderReliefSbs(backBuffer);
                    if (warped != nullptr) stereoSrc = warped;
                }
                if (!submitted)
                {
                    // Stage-1 sharp path: 1:1 copy into the half-size stereo swapchains (no resample blur).
                    usedSharpStereo = CopySbsHalvesToStereoEyes(stereoSrc, cfg.stereoSwapEyes);
                    submitted = usedSharpStereo;
                }
                if (!submitted)   // sharp swapchains not ready -> old shader-blit fallback into g_eyes
                    submitted = CopySbsBackbufferToEyes(stereoSrc, cfg.stereoSwapEyes, stereoSourceVScale);
                if (!submitted)
                {
                    CopyGameFrameToEye(0, backBuffer);
                    CopyGameFrameToEye(1, backBuffer);
                    submitted = true;
                }
            }
            else if (aerActive)
            {
                submitted = SubmitAerFrame(cfg, backBuffer, renderTagPose, true);
            }
            else if (MELEVR::D3DCapture::IsDibrStereoReady())
            {
                // UN-SQUASH (2026-07-07): DIBR eyes are FULL-FRAME (one whole eye's worth), like AER. Use the
                // full-frame copy (matches AER), NOT CopyTextureToEye - the latter applies cinematic aspect/fill
                // that squashes a full-frame source into the eye (the DIBR "squashed" bug).
                ID3D11Texture2D* left = MELEVR::D3DCapture::GetDibrLeftEye(backBuffer);
                CopyTextureToEyeFullFrame(0, left != nullptr ? left : backBuffer);   // left = real frame
                ID3D11Texture2D* right = MELEVR::D3DCapture::GetDibrRightEye(backBuffer);
                CopyTextureToEyeFullFrame(1, right != nullptr ? right : backBuffer); // right = depth-synthesized
                submitted = true;
            }
            else if (sfr2Active && CopyStereoPassCaptureToEyes())
            {
                // Stereo 2 (SFR): pass0 -> left eye, pass1 -> right eye. Only in mode 4, so plain Mono
                // can never pick up a stale captured pair.
                submitted = true;
                sfrPairSubmitted = true;   // [FREEZETAG] eyes hold the captured pair this present
            }
            else
            {
                // MONO (vrMode 0) = monoscopic VR: full VR camera + head tracking, one image in both eyes.
                // Use the AER-style FULL-FRAME copy, NOT CopyGameFrameToEye (-> CopyTextureToEye, which applies
                // the cinematic aspect-fit that SQUASHES a full 16:9 frame into the square eye = the "mono looks
                // squished" bug, 2026-07-08). CopyTextureToEyeFullFrame maps whole->whole, aspect-correct, and
                // keeps head tracking (this is still the normal VR projection submit, not the flat quad).
                CopyTextureToEyeFullFrame(0, backBuffer);
                CopyTextureToEyeFullFrame(1, backBuffer);
                submitted = true;
            }
            backBuffer->Release();
        }
    }

    // --- Render-side view offset: camera shift + screen distance + lean -------------------------------
    {
        constexpr float kMetersToUU = 50.0f;
        float right = cfg.cameraOffsetEnabled ? cfg.cameraOffsetRight : 0.0f;
        float up = cfg.cameraOffsetEnabled ? cfg.cameraOffsetUp : 0.0f;
        float fwd = cfg.screenDistanceEnabled ? -cfg.flatScreenDistance : 0.0f;   // bool gates it; +dist = farther
        if (forceFlatCine || vrCineActive)
        {
            // The MANUAL framing knobs stay parked in any cine: camera offset and screen distance are
            // the player's framing preferences and the shot belongs to the director. Lean is not one
            // of those - it is the player's own head - and it is added below.
            right = 0.0f;
            up = 0.0f;
            fwd = 0.0f;
        }
        // [LEANCINE 2026-08-22] Lean now applies in conversations and cutscenes too. It used to be
        // zeroed by the branch above along with the framing knobs, which is why leaning did nothing
        // the moment a scene started - and ME1 renders convos and cutscenes in VR by DEFAULT
        // (cineVrConvo / cineVrCutscene), so those are real stereo scenes where moving your head
        // should move your viewpoint. Head rotation being parked in a cine is a separate, deliberate
        // choice (the director aims the camera); translation is not rotation.
        // FLAT cine is deliberately excluded, and that is not an oversight: a flat cine is presented
        // as a WORLD-LOCKED quad, so leaning already gives real parallax against the panel for free
        // via OpenXR. Writing a camera offset there would move the world INSIDE a fixed panel, which
        // is the wrong thing and reads as the picture sliding around.
        if (cfg.leanEnabled && headValid && !menuOpen && !forceFlatCine)
        {
            // Forward/back (a dolly) reads ~3x weaker than sideways (parallax), so emphasize it to feel matched.
            constexpr float kFwdLeanEmphasis = 3.0f;
            right += headPos.x * cfg.leanGain;
            up += headPos.y * cfg.leanGain;
            fwd += -headPos.z * cfg.leanGain * kFwdLeanEmphasis;
        }
        // (The old cine view-space dolly is gone - it changed parallax/clipped into actors instead of
        // zooming. The whole VR-cine zoom family followed it out on 2026-07-18.)

        // EXPERIMENT (combatCamHold, step 2): when head aim is driving ControlRotation, the over-shoulder camera
        // ARCS around Shepard (position swings) = "the world spins around him". Cancel that arc with a view-space
        // translation opposite the swing, proportional to the yaw/pitch injected this frame (g_appliedHead*UU).
        // Orientation is untouched (follows aim -> reticle accurate); only the POSITION is held. gain ~= boom
        // radius (m) and isn't known exactly, so it's a tunable dialled in-headset; invert flips the direction.
        if (cfg.combatCamHold && headAimApplied && !forceFlatCine)
        {
            constexpr float kUUToRad = 6.2831853f / 65536.0f;
            const float yawRad = static_cast<float>(g_appliedHeadYawUU) * kUUToRad;
            const float pitchRad = static_cast<float>(g_appliedHeadPitchUU) * kUUToRad;
            const float s = cfg.combatCamHoldInvert ? -1.0f : 1.0f;
            // First-order arc: camera swings sideways (right) with yaw, up/down with pitch. r*theta for small theta.
            right += s * cfg.combatCamHoldGain * yawRad;
            up    += s * cfg.combatCamHoldGain * pitchRad;
        }

        const float rightUU = right * kMetersToUU;
        MELEVR::RenderHook::SetSyncStereoReplay(false, 0.0f);

        const float upUU = up * kMetersToUU;
        const float fwdUU = fwd * kMetersToUU;
        MELEVR::RenderHook::SetViewOffset(rightUU, upUU, fwdUU);
        // ME2/ME3 parity: arm AER independently of the generic lean/base offset. Only the primary
        // ULocalPlayer branch consumes this eye and publishes its render stamp.
        MELEVR::RenderHook::SetAerState(aerActive, g_aerRenderEye,
                                        cfg.aerHalfEyeUU * cineSepMul, cfg.aerSwapEyes);

        const uint64_t headLog = ++g_headControlLogCounter;
        if ((headLog % 180ull) == 1ull)
        {
            LogLine(std::string("[HEADCTRL] headValid=") + std::to_string(headValid ? 1 : 0) +
                    " menu=" + std::to_string(menuOpen ? 1 : 0) +
                    " stereo=" + std::to_string(stereoActive ? 1 : 0) +
                    " headLook=" + std::to_string(headLookApplied ? 1 : 0) +
                    " headAim=" + std::to_string(headAimApplied ? 1 : 0) +
                    " lean=" + std::to_string((cfg.leanEnabled && headValid && !menuOpen && !forceFlatCine) ? 1 : 0) +
                    " weaponMode=" + std::to_string(weaponModeForLog) +
                    " mode='" + weaponModeNameForLog + "'" +
                    " yaw=" + std::to_string(yawDegForLog) +
                    " pitch=" + std::to_string(pitchDegForLog) +
                    " viewUU=(" + std::to_string(rightUU) + "," + std::to_string(upUU) + "," + std::to_string(fwdUU) + ")");
        }
    }

    // --- TAG WHAT YOU RENDERED (one-present delayed tag = THE anti-drag fix, 2026-07-12) --------------
    // RunFrame runs at Present. SetHeadLook armed THIS present affects the NEXT game frame's CalcSceneView,
    // so the backbuffer just captured was rendered with the PREVIOUS present's head sample - but the submit was
    // tagging it with the CURRENT (or late-latched = even fresher) pose. During any head motion the tag then
    // claims the image is one frame fresher than it is, the compositor under-corrects by exactly one frame's
    // head delta, and the world rides WITH the head before settling = the "drags at the start of a turn,
    // shakes a little then stabilizes". This violates the pose-tag rule (tag the pose it was RENDERED with).
    // AER never had this problem because it tags per-eye render-time history poses; mono/stereo now get the
    // same treatment: tag with the pose that ARMED this frame's render (last present's sample).
    // NOTE: late-latch (lateLatchPose) is the OPPOSITE correction - fresher tag = bigger mismatch - which is
    // why it made the shake slightly worse. Kept as an A/B toggle; the delayed tag wins when both are on.
    // Pose history ring: [0] = previous present's frame-start sample, [1] = two back, [2] = three back.
    // The delay is TUNABLE (poseTagDelayFrames, fractional, blended) because the true render-ahead depth is
    // not guaranteed to be exactly one present (DXGI frame queueing; 60fps game shown twice per 120Hz cycle).
    // Dial the slider in-headset until the world locks; where it locks = the real pipeline depth.
    static XrPosef s_tagHistPose[3] = {IdentityPose(), IdentityPose(), IdentityPose()};
    static XrVector3f s_tagHistEye[3][2] = {};
    static int s_tagHistCount = 0;
    // [POSEEXACT 2026-08-11] Arm-indexed pose ring. poseTagDelayFrames guesses HOW MANY PRESENTS back
    // the submitted image was rendered; that guess is only right at the framerate it was dialled at,
    // which is why dropping render resolution (4x the fps) made head tracking shake everywhere while
    // the dll was byte-identical. The render already stamps WHICH head arm it consumed, so look that
    // arm up and tag with the pose it actually rendered with. Exact, and free of any tuning constant.
    // Falls back to the old delayed-tag path on a ring miss, so this can never be worse than before.
    static constexpr int kArmRing = 64;   // exceeds the 45-present SFR stale-pair hold window
    static uint64_t  s_armSeq[kArmRing] = {};
    static XrPosef   s_armPose[kArmRing] = {};
    static XrVector3f s_armEye[kArmRing][2] = {};
    static bool      s_armPrimed = false;
    if (g_tagHistReset) { s_tagHistCount = 0; g_tagHistReset = false; }   // recenter: old-space poses invalid
    const bool useDelayedTag = cfg.poseTagDelay && !aerActive && headValid && s_tagHistCount > 0;

    // Normalized-lerp for the fractional blend (angles between adjacent presents are tiny; nlerp is plenty).
    auto nlerpQuat = [](const XrQuaternionf& a, XrQuaternionf b, float t) noexcept -> XrQuaternionf {
        const float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (dot < 0.0f) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
        XrQuaternionf q{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                        a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
        const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
        if (n > 1e-6f) { q.x /= n; q.y /= n; q.z /= n; q.w /= n; }
        return q;
    };
    // Sample at "k presents back": k=0 = this present's fresh sample, k>=1 = history (clamped to what's available).
    auto tagPoseAt = [&](int k) noexcept -> const XrPosef& {
        if (k <= 0 || s_tagHistCount <= 0) return renderTagPose;
        const int i = (k - 1 < s_tagHistCount - 1) ? k - 1 : s_tagHistCount - 1;
        return s_tagHistPose[i];
    };
    auto tagEyeAt = [&](int k, int e) noexcept -> const XrVector3f& {
        if (k <= 0 || s_tagHistCount <= 0) return g_subViews[e].pose.position;
        const int i = (k - 1 < s_tagHistCount - 1) ? k - 1 : s_tagHistCount - 1;
        return s_tagHistEye[i][e];
    };

    XrPosef delayedTagPose = renderTagPose;
    XrVector3f delayedEyePos[2] = {};
    // [POSEEXACT] For the conversation pipeline, tag with the pose the render ACTUALLY used.
    // No tuning constant is involved. cfg.poseTagExact off forces the delayed A/B path.
    bool usedExactTag = false;
    int exactBranch = 0;   // [POSEEXACT DIAG] 0 = fallback, 2 = captured-pair arm ring hit
    // [POSEEXACT SCOPE 2026-08-11] measured in-headset the same day, both directions:
    //  - GAMEPLAY: exact = shake, dialled delay = smooth. The submit reads g_pairArmSeq live while
    //    the game thread runs ahead of the present, so the stamp often describes the NEXT frame and
    //    the tag lands too fresh - intermittently, which is worse than a consistent small error.
    //  - CONVERSATION (gm=5): exact = smooth, the dialled cine constant = jitter.
    //  - SCRIPTED CUTSCENE (gm=6): exact = jitter, the 2-frame cine delay = smooth.
    // These are distinct render pipelines despite appearing inside one continuous sequence.
    // So exact is CONVERSATION-ONLY (and only while cine head tracking is on). Scripted
    // cutscenes and gameplay use their hand-dialled lock values.
    // [LANELATCH 2026-08-12] The tag lane is LATCHED, not switched per present. Measured in the field log:
    // the cine classification flaps mid-cutscene - five flips in three seconds around gm 7 and the
    // gm 8 "movie" flag (VR cine -> FLAT -> VR cine -> VR cine -> FLAT) - and because the exact-tag
    // lane keys off vrCineActive, every flip swapped the tag model under a running shot. Cine and
    // gameplay have genuinely different render-ahead depths, so each swap is a tag discontinuity =
    // the judder reported in some cutscenes while others stayed clean.
    // The rendering decision is untouched: this latch feeds ONLY the choice of pose-tag lane. A new
    // state has to hold for kLaneSettleFrames consecutive presents before the lane follows it, which
    // rides out sub-second flapping while still switching within ~0.15s at a real transition.
    static bool s_laneCine = false;
    static int  s_laneCandidateFrames = 0;
    {
        constexpr int kLaneSettleFrames = 12;
        const bool want = vrCineActive && cfg.cineHeadTracking;
        if (want == s_laneCine)
        {
            s_laneCandidateFrames = 0;
        }
        else if (++s_laneCandidateFrames >= kLaneSettleFrames)
        {
            s_laneCine = want;
            s_laneCandidateFrames = 0;
            static int s_laneLogged = 0;
            if (s_laneLogged < 200)   // raised from 12 for the [CINEJIT] correlation runs; still bounded
            {
                ++s_laneLogged;
                LogLine(std::string("[LANELATCH] pose-tag lane -> ") + (s_laneCine ? "cine (auto timing)" : "gameplay (dialled lock)"));
            }
        }
    }
    // [CINETAG-AUTO 2026-08-12] A direct in-headset A/B isolated two opposite
    // pipelines inside what looks like one continuous Normandy cinematic:
    //   gm=5 Conversation -> exact capture-owned arm is smooth; 2-frame delay shakes.
    //   gm=6 Cinematic    -> 2-frame delayed lock is smooth; exact arm shakes.
    // DecideCine already latches those engine modes through gm 7/8/-1 transition blips,
    // so route on that authoritative context. poseTagExact remains the master A/B:
    // OFF forces the delayed model everywhere; ON means automatic per-pipeline timing.
    const bool exactRoute = s_laneCine && inConversationMode;
    const bool exactEligible = exactRoute && sfrPairSubmitted;
    {
        static int s_lastTagRoute = -1;
        const int tagRoute = !s_laneCine ? 0 : ((cfg.poseTagExact && exactRoute) ? 1 : 2);
        if (tagRoute != s_lastTagRoute)
        {
            s_lastTagRoute = tagRoute;
            LogLine(std::string("[CINETAG-AUTO] route -> ") +
                    (tagRoute == 1 ? "exact (conversation/gm5)" :
                     tagRoute == 2 ? "delayed (cutscene/gm6 or manual override)" :
                                     "gameplay delayed lock"));
        }
    }
    if (useDelayedTag && cfg.poseTagExact && s_armPrimed && exactEligible)
    {
        // [CAPTUREPOSE 2026-08-12] The arm travels with the texture bank from pass-0 capture to
        // pair publication to this exact submit. The old live GetPairArmSeq() read could observe a
        // later render, then ARMPREV guessed around that race. No guess remains: this arm names the
        // head-look baked into these pixels.
        const uint64_t target = g_sfrPairArmThisFrame;
        for (int i = 0; i < kArmRing; ++i)
        {
            if (s_armSeq[i] != target || target == 0ull) continue;
            delayedTagPose = s_armPose[i];
            delayedEyePos[0] = s_armEye[i][0];
            delayedEyePos[1] = s_armEye[i][1];
            usedExactTag = true;
            exactBranch = 2;
            break;
        }
        static bool s_exactLogged = false;
        if (usedExactTag && !s_exactLogged)
        {
            s_exactLogged = true;
            LogLine("[POSEEXACT] conversation: captured pair carries its exact rendered-pose arm; "
                    "scripted cutscenes and gameplay stay on their dialled locks");
        }
    }
    // [CAPTUREPOSE DIAG] Every eligible fresh/held pair should resolve its capture-owned arm.
    if (useDelayedTag && cfg.poseTagExact && exactEligible)
    {
        static uint32_t s_dRing = 0, s_dFall = 0, s_dTotal = 0;
        if (exactBranch == 2) ++s_dRing;
        else ++s_dFall;
        if (++s_dTotal >= 600)
        {
            LogLine("[POSEEXACT] window: armHit=" + std::to_string(s_dRing) +
                    " capturedArm=1 fallback=" + std::to_string(s_dFall) + " /600");
            s_dRing = s_dFall = s_dTotal = 0;
        }
    }
    if (useDelayedTag && !usedExactTag)
    {
        // Cine runs a different render-ahead depth than gameplay (measured), so it carries its own.
        float d = vrCineActive ? cfg.cinePoseTagDelayFrames : cfg.poseTagDelayFrames;
        if (d < 0.0f) d = 0.0f;
        if (d > 3.0f) d = 3.0f;
        const int k0 = static_cast<int>(d);
        const float frac = d - static_cast<float>(k0);
        const XrPosef& pA = tagPoseAt(k0);
        const XrPosef& pB = tagPoseAt(k0 + 1);
        delayedTagPose.orientation = nlerpQuat(pA.orientation, pB.orientation, frac);
        delayedTagPose.position.x = pA.position.x + (pB.position.x - pA.position.x) * frac;
        delayedTagPose.position.y = pA.position.y + (pB.position.y - pA.position.y) * frac;
        delayedTagPose.position.z = pA.position.z + (pB.position.z - pA.position.z) * frac;
        for (int e = 0; e < 2; ++e)
        {
            const XrVector3f& eA = tagEyeAt(k0, e);
            const XrVector3f& eB = tagEyeAt(k0 + 1, e);
            delayedEyePos[e].x = eA.x + (eB.x - eA.x) * frac;
            delayedEyePos[e].y = eA.y + (eB.y - eA.y) * frac;
            delayedEyePos[e].z = eA.z + (eB.z - eA.z) * frac;
        }
        // [VRCINE] ME2's anchor, ported verbatim in intent: with the render rotation parked, a
        // delayed tag would describe a head pose the image does not contain, so the compositor
        // would reproject against a lie. Anchor the pair at the CURRENT head each frame instead
        // -> a head-locked 3D picture that follows you and does not drift.
        if (vrCineActive && !cfg.cineHeadTracking)
        {
            delayedTagPose = renderTagPose;
            delayedEyePos[0] = g_subViews[0].pose.position;
            delayedEyePos[1] = g_subViews[1].pose.position;
        }
    }

    // Late-latch (previous attempt, default OFF now): re-locate just before submit and tag fresher.
    XrPosef submitTagPose = renderTagPose;   // fallback = frame-start pose
    XrView lateViews[2] = {};
    bool haveLatePose = false;
    if (!useDelayedTag && cfg.lateLatchPose && headValid && !aerActive &&
        LocateViewsIn(g_appSpace, fs.predictedDisplayTime, lateViews))
    {
        haveLatePose = true;
        submitTagPose.orientation = QuatRemoveRoll(lateViews[0].pose.orientation);
        submitTagPose.position.x = (lateViews[0].pose.position.x + lateViews[1].pose.position.x) * 0.5f;
        submitTagPose.position.y = (lateViews[0].pose.position.y + lateViews[1].pose.position.y) * 0.5f;
        submitTagPose.position.z = (lateViews[0].pose.position.z + lateViews[1].pose.position.z) * 0.5f;
    }
    // INSTRUMENT: angular delta between the delayed (rendered-at) tag and the current head = the per-frame
    // mismatch the old tagging was baking in. Should be ~0 when still and grow with head speed.
    if (useDelayedTag)
    {
        const XrQuaternionf& a = delayedTagPose.orientation;
        const XrQuaternionf& bq = renderTagPose.orientation;
        float dot = a.x * bq.x + a.y * bq.y + a.z * bq.z + a.w * bq.w;
        if (dot < 0.0f) dot = -dot;
        if (dot > 1.0f) dot = 1.0f;
        const float deltaDeg = 2.0f * std::acos(dot) * 57.29578f;
        static uint64_t s_tdLog = 0;
        if ((s_tdLog++ % 120ull) == 0ull)
            LogLine("[TAGDELAY] rendered-at vs current head delta = " + std::to_string(deltaDeg) + " deg");
    }

    // --- Projection layer (mono) ---------------------------------------------------------------------
    // Both eyes carry the SAME image and the SAME world-locked head pose (roll removed). The render hook
    // widens the GAME projection to the headset target FOV; here the actual rendered FOV is submitted so AER
    // and the compositor agree.
    // [FREEZETAG] ME2's fix, ported (me2_xr.cpp ~1741). A projection layer only head-tracks if the
    // pose tag and the PIXELS agree. During conversation shot changes the engine skips rendering for
    // bursts: the SFR pair is held, the image freezes - but the tag kept following the live head, so
    // the compositor saw "already aligned", did zero reprojection, and the stale shot rode the
    // headset. Standard missed-frame handling is the opposite: freeze the tag AND the declared FOV
    // WITH the image and let the runtime reproject it, so tracking survives the stale frame.
    // Skipped when the pair is fresh, and skipped for locked cine (tracking off), where the
    // current-head anchor above IS the intent.
    // EXTENDED TO GAMEPLAY 2026-08-11. Originally cine-scoped on the belief that "gameplay's
    // tagging is already dialled in", but the mechanism never was cine-specific: presents run
    // ~95-104/s while the SFR replay renders ~85/s, so 10-15% of GAMEPLAY presents re-submit a
    // held pair too - with a live tag on stale pixels = zero reprojection = the shimmer during
    // head motion that settles when the head stops (exact field report, same day the pacing
    // fix cleaned up cine). Freeze now covers every held sfr2 pair outside locked cine, and it
    // freezes the per-eye tag positions with it (they kept tracking the live head before).
    {
        static XrPosef s_freshDelayed = {}, s_freshSubmit = {}, s_freshRender = {};
        static XrVector3f s_freshEye[2] = {};
        static XrFovf  s_freshFov = {};
        static bool    s_freshInit = false;
        const bool lockedCine = vrCineActive && !cfg.cineHeadTracking;
        const bool freezeEligible = sfrPairSubmitted && !lockedCine && !forceFlatCine;
        if (!freezeEligible || g_sfrPairFreshThisFrame || !s_freshInit)
        {
            s_freshDelayed = delayedTagPose; s_freshSubmit = submitTagPose;
            s_freshRender = renderTagPose;   s_freshFov = renderTagFov;
            s_freshEye[0] = delayedEyePos[0]; s_freshEye[1] = delayedEyePos[1];
            s_freshInit = true;
        }
        else
        {
            delayedTagPose = s_freshDelayed; submitTagPose = s_freshSubmit;
            renderTagPose  = s_freshRender;  renderTagFov  = s_freshFov;
            delayedEyePos[0] = s_freshEye[0]; delayedEyePos[1] = s_freshEye[1];
            static int s_logged = 0;
            if (s_logged < 8 || (s_logged % 600) == 0)
                LogLine(std::string("[FREEZETAG] stale SFR pair: tag + declared FOV frozen with the image (") +
                        (vrCineActive ? "cine" : "gameplay") + ")");
            ++s_logged;
        }
    }

    // [CINEJIT 2026-08-12] Residual cutscene jitter, timing unknown ("random / can't tell"). One
    // line per second while a VR cine is live, so the log timestamps correlate with when it is
    // felt. Reads: holds = held-pair freeze events (>=2 consecutive stale presents) and the longest
    // one; headDeg = how far the head moved this second (jitter is only visible while it is > 0);
    // laneSwitch counts [LANELATCH] handovers. Interpretation: jitter seconds with holds > 0 =
    // the freeze mechanism (port ME2's STALEEYE); jitter seconds with holds = 0 and steady fresh =
    // the cine capture pipeline (the 08-02 suspect); jitter only on laneSwitch seconds = handover.
    // [CINEJIT v2 2026-08-12] GAPLESS while any cine context is latched. v1 only counted while the
    // pair path was healthy, so the exact seconds where pairs died (the bug) produced NO line at
    // all - the 3s and 7.5s holes in that run were the evidence, silently dropped. Now every latched
    // second logs, with the submit path split into fresh / held / MONO-fallback / flat, plus the raw
    // mode byte so gate-vs-presentation disagreements are visible on one line.
    {
        static int s_jPresents = 0, s_jFresh = 0, s_jHeld = 0, s_jMono = 0, s_jFlat = 0;
        static int s_jHoldRun = 0, s_jMaxHold = 0;
        static ULONGLONG s_jWinStart = 0;
        static XrQuaternionf s_jLastQ = {0, 0, 0, 1};
        static bool s_jHaveQ = false;
        static float s_jHeadDeg = 0.0f;
        // [TAGTRUTH 2026-08-12] The two per-present signals no fix ever measured, aggregated per
        // second. tagErr = angle between the submitted tag and the current head: its per-present
        // SHAPE is the point - a steady value is a healthy pipeline depth, a 0-and-2-frame sawtooth
        // is the compositor correction oscillating at 60Hz = the felt shake, proportional to head
        // speed. lookStep = how far the head-look consumed by successive FRESH pairs advanced: even
        // steps = content turns smoothly, 0/2x alternation = the content itself vibrates while the
        // tag stays honest. flips count reversals > 0.35 deg against the previous delta.
        static float s_teMin = 1e9f, s_teMax = -1.0f, s_tePrev = -1.0f;
        static int   s_teFlips = 0;
        static float s_lsMin = 1e9f, s_lsMax = -1.0f, s_lsPrev = -1.0f;
        static int   s_lsFlips = 0, s_lsPrevYaw = 0, s_lsPrevPitch = 0;
        static bool  s_lsHavePrev = false;
        if (inConversationMode || inCutsceneMode)
        {
            ++s_jPresents;
            if (forceFlatCine)            ++s_jFlat;
            else if (!sfrPairSubmitted)   ++s_jMono;   // sfr2 pair path failed -> live-mono fallback
            else if (g_sfrPairFreshThisFrame)
            {
                ++s_jFresh;
                if (s_jHoldRun > s_jMaxHold) s_jMaxHold = s_jHoldRun;
                s_jHoldRun = 0;
            }
            else { ++s_jHeld; ++s_jHoldRun; }
            if (headValid)
            {
                if (s_jHaveQ) s_jHeadDeg += QuatAngleDeg(s_jLastQ, g_subViews[0].pose.orientation);
                s_jLastQ = g_subViews[0].pose.orientation;
                s_jHaveQ = true;
            }
            if (useDelayedTag && !forceFlatCine)
            {
                const float te = QuatAngleDeg(delayedTagPose.orientation, renderTagPose.orientation);
                if (te < s_teMin) s_teMin = te;
                if (te > s_teMax) s_teMax = te;
                if (s_tePrev >= 0.0f && std::fabs(te - s_tePrev) > 0.35f) ++s_teFlips;
                s_tePrev = te;
            }
            if (g_sfrPairFreshThisFrame && !forceFlatCine)
            {
                int py = 0, pp = 0;
                MELEVR::RenderHook::GetPairLookUU(py, pp);
                if (s_lsHavePrev)
                {
                    constexpr float kUUToDeg = 360.0f / 65536.0f;
                    const float dy = static_cast<float>(py - s_lsPrevYaw) * kUUToDeg;
                    const float dp = static_cast<float>(pp - s_lsPrevPitch) * kUUToDeg;
                    const float ls = std::sqrt(dy * dy + dp * dp);
                    if (ls < s_lsMin) s_lsMin = ls;
                    if (ls > s_lsMax) s_lsMax = ls;
                    if (s_lsPrev >= 0.0f && std::fabs(ls - s_lsPrev) > 0.35f) ++s_lsFlips;
                    s_lsPrev = ls;
                }
                s_lsPrevYaw = py; s_lsPrevPitch = pp; s_lsHavePrev = true;
            }
            const ULONGLONG nowJ = GetTickCount64();
            if (s_jWinStart == 0) s_jWinStart = nowJ;
            if (nowJ - s_jWinStart >= 1000)
            {
                // [CINETILT] tilt = camera tilt vs world up; camStep = max per-frame camera basis
                // rotation this second (0 = static shot, the shake's scene-selectivity discriminator);
                // wy = the [WORLDYAW2] axis A/B state this run.
                float jTilt = 0.0f, jCamStep = 0.0f;
                MELEVR::RenderHook::ReadCineCamTelemetry(jTilt, jCamStep);
                const float teMin = (s_teMax < 0.0f) ? 0.0f : s_teMin;
                const float teMax = (s_teMax < 0.0f) ? 0.0f : s_teMax;
                const float lsMin = (s_lsMax < 0.0f) ? 0.0f : s_lsMin;
                const float lsMax = (s_lsMax < 0.0f) ? 0.0f : s_lsMax;
                char jl[352];
                std::snprintf(jl, sizeof(jl),
                              "[CINEJIT] 1s: gm=%d vr=%d tag=%s presents=%d fresh=%d held=%d mono=%d flat=%d maxHold=%d headDeg=%.1f tilt=%.1f camStep=%.2f wy=%d"
                              " tagErr=%.2f/%.2f/%d lookStep=%.2f/%.2f/%d",
                              gameMode, vrCineActive ? 1 : 0,
                              (cfg.poseTagExact && exactRoute) ? "exact" : "delay",
                              s_jPresents, s_jFresh, s_jHeld, s_jMono,
                              s_jFlat, s_jMaxHold, s_jHeadDeg, jTilt, jCamStep, cfg.cineWorldYaw ? 1 : 0,
                              teMin, teMax, s_teFlips, lsMin, lsMax, s_lsFlips);
                LogLine(jl);
                s_jPresents = 0; s_jFresh = 0; s_jHeld = 0; s_jMono = 0; s_jFlat = 0;
                s_jMaxHold = 0; s_jHeadDeg = 0.0f;
                s_teMin = 1e9f; s_teMax = -1.0f; s_tePrev = -1.0f; s_teFlips = 0;
                s_lsMin = 1e9f; s_lsMax = -1.0f; s_lsPrev = -1.0f; s_lsFlips = 0;
                s_jWinStart = nowJ;
            }
        }
        else
        {
            s_jPresents = 0; s_jFresh = 0; s_jHeld = 0; s_jMono = 0; s_jFlat = 0;
            s_jHoldRun = 0; s_jMaxHold = 0; s_jWinStart = 0; s_jHaveQ = false; s_jHeadDeg = 0.0f;
            s_teMin = 1e9f; s_teMax = -1.0f; s_tePrev = -1.0f; s_teFlips = 0;
            s_lsMin = 1e9f; s_lsMax = -1.0f; s_lsPrev = -1.0f; s_lsFlips = 0; s_lsHavePrev = false;
        }
    }

    XrCompositionLayerProjectionView projViews[2] = {};
    const uint64_t viewFillLog = ++g_viewFillLogCounter;
    for (int e = 0; e < 2; ++e)
    {
        projViews[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
        // Tag priority: delayed rendered-at pose (the fix) > late-latch (A/B) > frame-start pose.
        XrPosef viewPose = useDelayedTag ? delayedTagPose : (haveLatePose ? submitTagPose : renderTagPose);
        XrFovf viewFov = renderTagFov;
        // [CINEVR2 zoom v2] cine zoom declares the UNZOOMED fill window (constant across cuts and
        // zoom values) while the render is narrowed - the magnifier. See the FOV block above.
        if (cineZoomDeclare && cineZoomDeclH > 0.05f && cineZoomDeclV > 0.05f)
        {
            viewFov.angleLeft = -cineZoomDeclH;
            viewFov.angleRight = cineZoomDeclH;
            viewFov.angleUp = cineZoomDeclV;
            viewFov.angleDown = -cineZoomDeclV;
        }
        if (aerActive && !forceFlatCine && g_aerHistPoseValid[e])
        {
            // AER anti turn-shake (ME2 parity, 2026-07-07): submit each eye at the head pose IT was rendered at,
            // not the shared current pose. The held eye is ~1 frame old; tagging it with its own (older) pose
            // lets the compositor reproject it to the current head -> both eyes align on a head turn. Tagging
            // both with the current pose (the old behaviour) left the held eye showing stale rotation = the shake.
            viewPose = g_aerHistPose[e];
        }
        // [EYETAG fix 2026-07-19] sfr2 included: Stereo 2 never entered this branch (stereoActive = the LEGACY
        // flag, forced false in sfr2 mode) so both eyes were tagged at head-center. VDXR reprojects
        // orientation-only and tolerated the lie; Quest Link's compositor corrects each eye positionally
        // -> opposite per-eye shifts -> the constant unfusable doubling ([EYETAG] log-proven: decl p identical
        // for e=0/e=1 while loc p showed the real +-3cm eyes).
        else if ((stereoActive || sfr2Active) && !forceFlatCine)
        {
            // ME2-EXACT stereo tags (2026-07-02, v2): submit each eye at the RUNTIME's real per-eye position
            // (fixed true IPD from xrLocateViews), orientation stays the roll-removed head orientation (= the
            // direction the pair was actually rendered facing; pose-tag rule: tag what you RENDERED).
            // This is exactly ME2's arrangement (me2_xr.cpp:648 projViews[eye].pose = views[eye].pose) and it is
            // why ME2's separation slider reads as WORLD SCALE (render disparity moves, tags stay put) while
            // still fusing. The previous hand-rolled tag offset here was slider-linked and its sign could oppose
            // the render offset -> baseline doubling that GREW with the slider (exact field report). No sign
            // reasoning needed now: the runtime's own eye positions are ground truth.
            if (headValid)
            {
                // Per-eye position from the SAME sample as the orientation tag: delayed (rendered-at) when the
                // delay is on, else late-latched, else frame-start. Always the runtime's real per-eye = true IPD.
                viewPose.position = useDelayedTag ? delayedEyePos[e]
                                  : (haveLatePose ? lateViews[e].pose.position : g_subViews[e].pose.position);
            }
        }
        const XrFovf hmdEyeFov = headValid ? g_subViews[e].fov : viewFov;
        projViews[e].pose = viewPose;
        projViews[e].fov = viewFov;
        // Stage-1: if the sharp stereo path filled g_stereoEyes this frame, submit THOSE (half-size, 1:1 = sharp);
        // otherwise (AER / DIBR / mono / fallback) submit g_eyes exactly as before -> those modes are untouched.
        const bool useStereoSw = usedSharpStereo && g_stereoEyes[e].swapchain != nullptr;
        projViews[e].subImage.swapchain = useStereoSw ? g_stereoEyes[e].swapchain : g_eyes[e].swapchain;
        projViews[e].subImage.imageArrayIndex = 0;
        projViews[e].subImage.imageRect.offset.x = 0;
        projViews[e].subImage.imageRect.offset.y = 0;
        projViews[e].subImage.imageRect.extent.width = static_cast<int32_t>(useStereoSw ? g_stereoEyes[e].width : g_eyes[e].width);
        projViews[e].subImage.imageRect.extent.height = static_cast<int32_t>(useStereoSw ? g_stereoEyes[e].height : g_eyes[e].height);
        // [LINKFOV fix 2026-07-19] On Meta's runtime only: crop each eye's subImage to the intersection of
        // what was rendered (the declared window D) and the runtime's own per-eye asymmetric frustum T, and
        // declare exactly that intersection. Meta's compositor assumes T regardless of the declared FOV
        // ([EYETAG] log-proven: decl symmetric +-0.9425 vs loc (-0.9425,0.6981)/( -0.6981,0.9425) mirrored,
        // pose-tag fix alone did NOT cure the doubling); once declared == T-window == the submitted pixels,
        // its assumption and the declaration agree. Pure rect math in tan space - no extra rendering, and
        // "FOV declared must EXACTLY match what was rendered" stays true because the RECT shrinks with it.
        // When D is NARROWER than T (menus / flat cine rendered at game FOV) the intersection equals D and
        // this is a no-op - if Link still misplaces those, the next stage is padding into a T-sized canvas.
        // VDXR / any non-Meta runtime: never runs, path bit-identical to the shipped build.
        // [STEAMVRFOV] SteamVR voids any projection view declared wider than its own per-eye frustum
        // (log-proven; see g_isSteamVrRuntime). The same intersection-crop fixes it - unconditional
        // there. Meta keeps the questFovMatch toggle; VDXR remains untouched.
        if (((g_isOculusRuntime && cfg.questFovMatch) || g_isSteamVrRuntime) && headValid)
        {
            const XrFovf d = projViews[e].fov;           // declared render window
            const XrFovf& t = g_subViews[e].fov;         // runtime's per-eye frustum
            const float aL = std::fmax(d.angleLeft, t.angleLeft);
            const float aR = std::fmin(d.angleRight, t.angleRight);
            const float aU = std::fmin(d.angleUp, t.angleUp);
            const float aD = std::fmax(d.angleDown, t.angleDown);
            const float tanDL = std::tan(d.angleLeft), tanDR = std::tan(d.angleRight);
            const float tanDU = std::tan(d.angleUp), tanDD = std::tan(d.angleDown);
            const float hSpan = tanDR - tanDL;
            const float vSpan = tanDU - tanDD;
            if (aR - aL > 0.05f && aU - aD > 0.05f && hSpan > 1e-4f && vSpan > 1e-4f)
            {
                const int32_t w = projViews[e].subImage.imageRect.extent.width;
                const int32_t h = projViews[e].subImage.imageRect.extent.height;
                auto clampI = [](int32_t v, int32_t lo, int32_t hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); };
                int32_t x0 = clampI(static_cast<int32_t>(std::lround((std::tan(aL) - tanDL) / hSpan * w)), 0, w - 1);
                int32_t x1 = clampI(static_cast<int32_t>(std::lround((std::tan(aR) - tanDL) / hSpan * w)), x0 + 1, w);
                int32_t y0 = clampI(static_cast<int32_t>(std::lround((tanDU - std::tan(aU)) / vSpan * h)), 0, h - 1);
                int32_t y1 = clampI(static_cast<int32_t>(std::lround((tanDU - std::tan(aD)) / vSpan * h)), y0 + 1, h);
                projViews[e].subImage.imageRect.offset.x += x0;
                projViews[e].subImage.imageRect.offset.y += y0;
                projViews[e].subImage.imageRect.extent.width = x1 - x0;
                projViews[e].subImage.imageRect.extent.height = y1 - y0;
                projViews[e].fov.angleLeft = aL;
                projViews[e].fov.angleRight = aR;
                projViews[e].fov.angleUp = aU;
                projViews[e].fov.angleDown = aD;
            }
        }
        if (e == 0 && (viewFillLog % 180ull) == 1ull)
        {
            LogLine("[VIEWFILL_RENDER_MATCH] forced=0 headValid=" + std::to_string(headValid ? 1 : 0) +
                    " enabled=" + std::to_string(cfg.vrFovFillEnabled ? 1 : 0) +
                    " fill=(" + std::to_string(ClampVrFill(cfg.vrFillH)) + "," +
                    std::to_string(ClampVrFill(cfg.vrFillV)) + ")" +
                    " renderedFov=(" + std::to_string(renderTagFov.angleLeft) + "," +
                    std::to_string(renderTagFov.angleRight) + "," +
                    std::to_string(renderTagFov.angleUp) + "," +
                    std::to_string(renderTagFov.angleDown) + ")" +
                    " hmdFov=(" + std::to_string(hmdEyeFov.angleLeft) + "," +
                    std::to_string(hmdEyeFov.angleRight) + "," +
                    std::to_string(hmdEyeFov.angleUp) + "," +
                    std::to_string(hmdEyeFov.angleDown) + ")" +
                    " eyeRect=" + std::to_string(g_eyes[e].width) + "x" + std::to_string(g_eyes[e].height) +
                    " subRect=" + std::to_string(projViews[e].subImage.imageRect.extent.width) + "x" +
                    std::to_string(projViews[e].subImage.imageRect.extent.height) + "+" +
                    std::to_string(projViews[e].subImage.imageRect.offset.x) + "," +
                    std::to_string(projViews[e].subImage.imageRect.offset.y));
        }
    }

    // [AERBLACK] A renderable XR frame must never lose the projection layer because GetBuffer or
    // another transient copy-path prerequisite hiccupped. The eye swapchains still contain the last
    // released pair, so hold their exact last projection instead of flashing both eyes black.
    if (aerActive && !forceFlatCine && fs.shouldRender != 0)
    {
        if (submitted)
        {
            g_aerLastProjViews[0] = projViews[0];
            g_aerLastProjViews[1] = projViews[1];
            g_aerHaveLastProjection = true;
        }
        else if (g_aerHaveLastProjection &&
                 g_eyes[0].swapchain != nullptr && g_eyes[1].swapchain != nullptr)
        {
            projViews[0] = g_aerLastProjViews[0];
            projViews[1] = g_aerLastProjViews[1];
            submitted = true;
            ++g_aerFallbackFrames;
        }
        else
        {
            ++g_aerBlackFrames;
        }

        const ULONGLONG nowMs = GetTickCount64();
        if (g_aerSubWindowMs == 0) g_aerSubWindowMs = nowMs;
        if (nowMs - g_aerSubWindowMs >= 2000)
        {
            char line[192] = {};
            sprintf_s(line, "[AERSUB] fallback=%u black=%u (zero-layer AER frames rescued)",
                      g_aerFallbackFrames, g_aerBlackFrames);
            LogLine(line);
            g_aerSubWindowMs = nowMs;
            g_aerFallbackFrames = 0;
            g_aerBlackFrames = 0;
        }
    }
    // [EYETAG] Quest Link doubling hunt (2026-07-19): per-eye DECLARED tag (what gets submitted) vs the RUNTIME's own
    // per-eye view (xrLocateViews) - pose position, orientation, and fov, both eyes, same instant. On VDXR stereo
    // fuses; on Link it doubles with each eye clean -> the offset must live in this relationship. One paired
    // sample/sec makes the delta readable straight off the log instead of guessed at.
    if ((viewFillLog % 90ull) == 2ull)
    {
        for (int e = 0; e < 2; ++e)
        {
            const XrPosef& dp = projViews[e].pose;
            const XrFovf& df = projViews[e].fov;
            const XrPosef& lp = g_subViews[e].pose;
            const XrFovf& lf = g_subViews[e].fov;
            char et[352];
            std::snprintf(et, sizeof(et),
                          "[EYETAG] e=%d decl p=(%.4f,%.4f,%.4f) q=(%.4f,%.4f,%.4f,%.4f) fov=(%.4f,%.4f,%.4f,%.4f)"
                          " | loc p=(%.4f,%.4f,%.4f) q=(%.4f,%.4f,%.4f,%.4f) fov=(%.4f,%.4f,%.4f,%.4f)",
                          e,
                          dp.position.x, dp.position.y, dp.position.z,
                          dp.orientation.x, dp.orientation.y, dp.orientation.z, dp.orientation.w,
                          df.angleLeft, df.angleRight, df.angleUp, df.angleDown,
                          lp.position.x, lp.position.y, lp.position.z,
                          lp.orientation.x, lp.orientation.y, lp.orientation.z, lp.orientation.w,
                          lf.angleLeft, lf.angleRight, lf.angleUp, lf.angleDown);
            LogLine(et);
        }
    }

    // [VDCRASH] AER inter-eye pose divergence, 2026-07-12: the leading (unproven) theory for the recurring
    // VirtualDesktop.LibOVRRT64_1.dll __fastfail crash (100% AER, never Stereo, uncatchable by SEH - Windows
    // deliberately makes __fastfail un-handleable). AER is the only mode that submits two eyes from DIFFERENT
    // points in time in one projection layer (fresh + up-to-a-frame-stale held eye, each tagged with its OWN
    // pose - see g_aerHistPose above); Stereo always submits both eyes from the same instant. If VD's compositor
    // has an edge case around how far apart the two submitted eyes' poses are allowed to be, AER is the only
    // mode that could ever trip it. Measure it every present so if it crashes again, the log shows whether
    // divergence spiked right before - proves or kills this theory instead of another guess.
    if (aerActive && !forceFlatCine)
    {
        const XrVector3f& p0 = projViews[0].pose.position;
        const XrVector3f& p1 = projViews[1].pose.position;
        const float posDelta = std::sqrt((p0.x-p1.x)*(p0.x-p1.x) + (p0.y-p1.y)*(p0.y-p1.y) + (p0.z-p1.z)*(p0.z-p1.z));
        const float rotDelta = QuatAngleDeg(projViews[0].pose.orientation, projViews[1].pose.orientation);
        static ULONGLONG s_lastVdcLog = 0;
        const ULONGLONG nowVdc = GetTickCount64();
        // Log every present when divergence is notably large (>2cm or >3deg beyond the base IPD-ish offset a
        // held eye naturally carries), else throttle to 1/sec so a healthy session doesn't flood the log.
        const bool spike = posDelta > 0.10f || rotDelta > 5.0f;
        if (spike || nowVdc - s_lastVdcLog >= 1000)
        {
            s_lastVdcLog = nowVdc;
            char vl[176];
            std::snprintf(vl, sizeof(vl), "[VDCRASH] AER inter-eye posDelta=%.4fm rotDelta=%.2fdeg%s",
                          posDelta, rotDelta, spike ? " <<SPIKE" : "");
            LogLine(vl);
        }
    }

    // Latch THIS present's frame-start sample into the history ring for the NEXT presents' tags: it's the
    // pose that armed the SetHeadLook/lean the next captured frame will be rendered with.
    if (headValid)
    {
        for (int i = 2; i >= 1; --i)
        {
            s_tagHistPose[i] = s_tagHistPose[i - 1];
            s_tagHistEye[i][0] = s_tagHistEye[i - 1][0];
            s_tagHistEye[i][1] = s_tagHistEye[i - 1][1];
        }
        s_tagHistPose[0] = renderTagPose;
        s_tagHistEye[0][0] = g_subViews[0].pose.position;
        s_tagHistEye[0][1] = g_subViews[1].pose.position;
        if (s_tagHistCount < 3) ++s_tagHistCount;
        // [POSEEXACT] record THIS present's sample against the arm it armed, so a later submit can
        // find the exact pose its frame was rendered with.
        {
            const uint64_t arm = MELEVR::RenderHook::GetHeadArmSeq();
            const int slot = static_cast<int>(arm % static_cast<uint64_t>(kArmRing));
            s_armSeq[slot] = arm;
            s_armPose[slot] = renderTagPose;
            s_armEye[slot][0] = g_subViews[0].pose.position;
            s_armEye[slot][1] = g_subViews[1].pose.position;
            s_armPrimed = true;
        }
    }
    else
    {
        s_tagHistCount = 0;   // tracking dropped: don't tag future frames with a stale history
    }

    XrCompositionLayerProjection proj = {};
    proj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
    proj.layerFlags = 0;
    proj.space = g_appSpace;   // world-anchored: the layer does NOT ride the head
    proj.viewCount = 2;
    proj.views = projViews;

    const XrCompositionLayerBaseHeader* layers[4] = {nullptr, nullptr, nullptr, nullptr};
    uint32_t layerCount = 0;
    XrCompositionLayerQuad flatCineQuad = {};
    // The head-locked per-eye QUAD path for cine was removed 2026-08-02. A VR cine now submits on
    // the normal projection layer with the [VRCINE FILL] window, which is what ME2 and ME3 do; the
    // quad path was an ME1 invention that declared no FOV and so could never carry the fill model.
    if (submitted && forceFlatCine && g_eyes[0].swapchain != nullptr)
    {
        float quadAspect = flatCineQuadAspect;
        if (!(quadAspect > 0.5f && quadAspect < 2.5f)) quadAspect = 16.0f / 9.0f;

        flatCineQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD_VALUE;
        flatCineQuad.layerFlags = 0;
        flatCineQuad.space = g_viewSpace;
        flatCineQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH_VALUE;
        flatCineQuad.subImage.swapchain = g_eyes[0].swapchain;
        flatCineQuad.subImage.imageArrayIndex = 0;
        flatCineQuad.subImage.imageRect.offset.x = 0;
        flatCineQuad.subImage.imageRect.offset.y = 0;
        flatCineQuad.subImage.imageRect.extent.width = static_cast<int32_t>(g_eyes[0].width);
        flatCineQuad.subImage.imageRect.extent.height = static_cast<int32_t>(g_eyes[0].height);
        flatCineQuad.pose = IdentityPose();
        flatCineQuad.pose.position.z = -1.4f;
        float monoWidth = inMenuMode ? cfg.menuMonoQuadWidth
                        : (inGalaxyMode ? cfg.galaxyMonoQuadWidth
                        : (inConversationMode ? cfg.convoMonoQuadWidth : cfg.cutsceneMonoQuadWidth));  // movies use the cutscene width now
        if (!(monoWidth > 0.5f && monoWidth < 5.0f)) monoWidth = 2.35f;
        if (monoWidth < 1.0f) monoWidth = 1.0f;
        if (monoWidth > 3.5f) monoWidth = 3.5f;
        flatCineQuad.size.width = monoWidth;
        flatCineQuad.size.height = flatCineQuad.size.width / quadAspect;
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&flatCineQuad);
    }
    else if (submitted)
    {
        layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj);
    }

    // Options menu: head-locked, alpha-blended quad rendered on top of the world.
    XrCompositionLayerQuad menuQuad = {};
    if (menuOpen && g_menuSwapchain != nullptr)
    {
        ID3D11Texture2D* menuTex = MELEVR::Menu::RenderFrame();
        if (menuTex != nullptr && CopyMenuToSwapchain(menuTex))
        {
            menuQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD_VALUE;
            menuQuad.layerFlags = kBlendSourceAlpha;
            menuQuad.space = g_viewSpace;   // head-locked
            menuQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH_VALUE;
            menuQuad.subImage.swapchain = g_menuSwapchain;
            menuQuad.subImage.imageArrayIndex = 0;
            menuQuad.subImage.imageRect.offset.x = 0;
            menuQuad.subImage.imageRect.offset.y = 0;
            menuQuad.subImage.imageRect.extent.width = kMenuW;
            menuQuad.subImage.imageRect.extent.height = kMenuH;
            menuQuad.pose = IdentityPose();
            {
                const auto& menuCfg = MELEVR::Config::Get();
                float menuDist = menuCfg.menuDistanceM;
                float menuX = menuCfg.menuOffsetXM;
                float menuY = menuCfg.menuOffsetYM;
                if (!(menuDist > 0.3f)) menuDist = 1.5f;   // guard against a bad/zero ini value
                if (!(menuX > -4.0f && menuX < 4.0f)) menuX = 0.0f;
                if (!(menuY > -4.0f && menuY < 4.0f)) menuY = 0.0f;
                if (menuX < -2.0f) menuX = -2.0f; else if (menuX > 2.0f) menuX = 2.0f;
                if (menuY < -1.5f) menuY = -1.5f; else if (menuY > 1.5f) menuY = 1.5f;
                menuQuad.pose.position.x = menuX;
                menuQuad.pose.position.y = menuY;
                menuQuad.pose.position.z = -menuDist;       // head-locked distance (further = less disparity, easier to fuse)
            }
            // Resizable menu (2026-07-18: usable regardless of screen size/settings): the
            // quad width comes from the Comfort slider; height keeps the texture aspect. Guarded so
            // a bad ini value can never shrink the menu into unusability or blow it up past reach.
            float menuW = MELEVR::Config::Get().menuSizeM;
            if (!(menuW >= 0.5f && menuW <= 2.5f)) menuW = 1.0f;
            menuQuad.size.width = menuW;
            menuQuad.size.height = menuW * static_cast<float>(kMenuH) / static_cast<float>(kMenuW);  // keep aspect
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuQuad);
        }
    }

    // [STEAMVRSCENE 2026-07-20] never end a SteamVR frame without a projection layer. Quad-only and
    // zero-layer frames read as "scene app not rendering" to SteamVR's compositor state machine - the
    // waiting-room screen shows INSTEAD of the game's content while the quads (overlays) float on top of it:
    // exactly the "menu with SteamVR background at the edges" + "in-game only the wait screen".
    // When no real projection is in the frame (pre-capture, menus, flat cine), submit the pre-cleared
    // BLACK swapchain as a projection at the runtime's own per-eye pose+frustum (always valid), placed
    // FIRST so it sits under every quad. Result: quads on black, same look as VDXR, and the compositor
    // sees a rendering scene app from frame 1. Non-SteamVR runtimes: dead code, path bit-identical.
    XrCompositionLayerProjectionView blackSceneViews[2];
    XrCompositionLayerProjection blackSceneProj = {};
    if (g_isSteamVrRuntime && g_blackSceneSwapchain != nullptr)
    {
        bool projInLayers = false;
        for (uint32_t li = 0; li < layerCount; ++li)
            if (layers[li] == reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj)) projInLayers = true;
        if (!projInLayers && layerCount < 4)
        {
            for (int e = 0; e < 2; ++e)
            {
                blackSceneViews[e] = {};
                blackSceneViews[e].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW_VALUE;
                blackSceneViews[e].pose = headValid ? g_subViews[e].pose : renderTagPose;
                blackSceneViews[e].fov = headValid ? g_subViews[e].fov : renderTagFov;
                blackSceneViews[e].subImage.swapchain = g_blackSceneSwapchain;
                blackSceneViews[e].subImage.imageArrayIndex = 0;
                blackSceneViews[e].subImage.imageRect.offset.x = 0;
                blackSceneViews[e].subImage.imageRect.offset.y = 0;
                blackSceneViews[e].subImage.imageRect.extent.width = kBlackSceneSize;
                blackSceneViews[e].subImage.imageRect.extent.height = kBlackSceneSize;
            }
            blackSceneProj.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VALUE;
            blackSceneProj.layerFlags = 0;
            blackSceneProj.space = g_appSpace;
            blackSceneProj.viewCount = 2;
            blackSceneProj.views = blackSceneViews;
            for (uint32_t li = layerCount; li > 0; --li) layers[li] = layers[li - 1];
            layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&blackSceneProj);
            ++layerCount;
            static bool s_blackLogged = false;
            if (!s_blackLogged)
            {
                s_blackLogged = true;
                LogLine("[STEAMVRSCENE] placeholder black projection active under quad-only frames.");
            }
        }
    }

    XrFrameEndInfo fei = {};
    fei.type = XR_TYPE_FRAME_END_INFO_VALUE;
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE_VALUE;
    fei.layerCount = layerCount;
    fei.layers = (layerCount > 0) ? layers : nullptr;
    // [XRVALIDATE / STEAMVR 2026-07-20] validate BOTH eyes' declarations before submit. A non-finite
    // or garbage field in EITHER view invalidates the whole projection layer on strict runtimes with
    // NO error (endFrame still returns 0, the compositor just shows the void) - and the pixel dump
    // proved the submitted images are perfect while SteamVR displayed nothing. The mono path (head-center pose,
    // provably displayable: main menu + Insert menu both show) is the repair value.
    for (int ve = 0; ve < 2; ++ve)
    {
        XrPosef& vp2 = projViews[ve].pose;
        XrFovf& vf2 = projViews[ve].fov;
        const float qlen2 = vp2.orientation.x * vp2.orientation.x + vp2.orientation.y * vp2.orientation.y +
                            vp2.orientation.z * vp2.orientation.z + vp2.orientation.w * vp2.orientation.w;
        const bool declBad =
            !std::isfinite(vp2.position.x) || !std::isfinite(vp2.position.y) || !std::isfinite(vp2.position.z) ||
            !std::isfinite(vp2.orientation.x) || !std::isfinite(vp2.orientation.y) ||
            !std::isfinite(vp2.orientation.z) || !std::isfinite(vp2.orientation.w) ||
            qlen2 < 0.81f || qlen2 > 1.21f ||
            std::fabs(vp2.position.x) > 100.0f || std::fabs(vp2.position.y) > 100.0f ||
            std::fabs(vp2.position.z) > 100.0f ||
            !std::isfinite(vf2.angleLeft) || !std::isfinite(vf2.angleRight) ||
            !std::isfinite(vf2.angleUp) || !std::isfinite(vf2.angleDown) ||
            vf2.angleRight <= vf2.angleLeft || vf2.angleUp <= vf2.angleDown;
        if (declBad)
        {
            static std::atomic<unsigned> s_valLogs{0};
            const unsigned vn = s_valLogs.fetch_add(1, std::memory_order_relaxed);
            if (vn < 12 || (vn % 300) == 0)
            {
                char vline[288];
                std::snprintf(vline, sizeof(vline),
                              "[XRVALIDATE] eye%d INVALID decl REPAIRED: pos=(%f,%f,%f) q2=%f fov=(%f,%f,%f,%f)",
                              ve, vp2.position.x, vp2.position.y, vp2.position.z, qlen2,
                              vf2.angleLeft, vf2.angleRight, vf2.angleUp, vf2.angleDown);
                LogLine(vline);
            }
            vp2 = renderTagPose;
            vf2 = renderTagFov;
        }
    }

    // [XRENDF / STEAMVR 2026-07-20] xrEndFrame's result was silently discarded - a runtime rejecting
    // the submitted layers (SteamVR validates; Meta forgives) was indistinguishable from success. Log failures.
    const XrResult endFrameResult = g_fn.endFrame(g_session, &fei);
    if (!XrSucceeded(endFrameResult))
    {
        static std::atomic<unsigned> s_efLogs{0};
        const unsigned n = s_efLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 8 || (n % 300) == 0)
            LogLine("[XRENDF] xrEndFrame FAILED XrResult=" + std::to_string(static_cast<int>(endFrameResult)) +
                    " layers=" + std::to_string(layerCount) +
                    " submitted=" + std::to_string(submitted ? 1 : 0));
    }
    // [XRSUBMIT / STEAMVR 2026-07-20] the SteamVR in-game void shows NO failure anywhere (endFrame OK,
    // session FOCUSED, pairs streaming) - so log the full submitted truth every ~2s: layer count and
    // the exact pose+fov eye 0 was declared with. A layer tagged with a garbage pose doesn't error,
    // it just isn't where the user looks; this line makes that case name itself.
    {
        static uint64_t s_xrsubmitTick = 0;
        if ((s_xrsubmitTick++ % 240ull) == 0ull)
        {
            char line[320];
            std::snprintf(line, sizeof(line),
                          "[XRSUBMIT] result=%d layers=%u submitted=%d flatCine=%d e0 pos=(%.3f,%.3f,%.3f) "
                          "q=(%.3f,%.3f,%.3f,%.3f) fov=(%.3f,%.3f,%.3f,%.3f) rect=%dx%d sw=%p",
                          static_cast<int>(endFrameResult), layerCount, submitted ? 1 : 0, forceFlatCine ? 1 : 0,
                          projViews[0].pose.position.x, projViews[0].pose.position.y, projViews[0].pose.position.z,
                          projViews[0].pose.orientation.x, projViews[0].pose.orientation.y,
                          projViews[0].pose.orientation.z, projViews[0].pose.orientation.w,
                          projViews[0].fov.angleLeft, projViews[0].fov.angleRight,
                          projViews[0].fov.angleUp, projViews[0].fov.angleDown,
                          projViews[0].subImage.imageRect.extent.width, projViews[0].subImage.imageRect.extent.height,
                          reinterpret_cast<void*>(projViews[0].subImage.swapchain));
            LogLine(line);
            std::snprintf(line, sizeof(line),
                          "[XRSUBMIT] e1 pos=(%.3f,%.3f,%.3f) q=(%.3f,%.3f,%.3f,%.3f) fov=(%.3f,%.3f,%.3f,%.3f) "
                          "rect=%dx%d sw=%p",
                          projViews[1].pose.position.x, projViews[1].pose.position.y, projViews[1].pose.position.z,
                          projViews[1].pose.orientation.x, projViews[1].pose.orientation.y,
                          projViews[1].pose.orientation.z, projViews[1].pose.orientation.w,
                          projViews[1].fov.angleLeft, projViews[1].fov.angleRight,
                          projViews[1].fov.angleUp, projViews[1].fov.angleDown,
                          projViews[1].subImage.imageRect.extent.width, projViews[1].subImage.imageRect.extent.height,
                          reinterpret_cast<void*>(projViews[1].subImage.swapchain));
            LogLine(line);
        }
    }

    static bool firstFrame = true;
    if (submitted && firstFrame)
    {
        firstFrame = false;
        LogLine("[M1] first head-tracked frame submitted (world-locked projection).");
    }
}

// One-time bring-up. The per-frame loop runs on the game's Present thread (OnPresent), not here, so the
// runtime composites with the game's single-threaded D3D11 context.
DWORD WINAPI XrThread(LPVOID) noexcept
{
    // Load saved settings before swapchain creation so the active profile is live before the first frame.
    MELEVR::Config::LoadFromIni();

    // [DOF] Re-assert the DepthOfField setting into GamerSettings.ini every launch (the game's own video-
    // options screen can rewrite that file). The engine already read SystemSettings for THIS launch, so
    // this lands on the NEXT one - the natural semantics for an engine ini graphics setting.
    MELEVR::D3DCapture::EnsureDepthOfFieldSetting(MELEVR::Config::Get().disableDof);

    if (!CreateInstance() || !ResolveSessionFunctions() || !CreateSessionAndSwapchains())
    {
        LogLine("[XR] session bring-up FAILED - see the [XR] line above for the failing call.");
        return 0;
    }
    // Install the one game-side hook: CalcSceneView head-look (decoupled look-around). Non-fatal if it
    // fails - the world-locked submission still runs, you just can't turn the camera with your head.
    MELEVR::RenderHook::Install();

    g_ready.store(true, std::memory_order_release);
    LogLine("[XR] session bring-up complete - head-tracked session armed (Insert = menu).");
    return 0;
}
}  // namespace

namespace MELEVR::XrSession
{
void TryCreateSession(ID3D11Device* device, int64_t colorFormat, uint32_t width, uint32_t height) noexcept
{
    if (device == nullptr) return;
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    g_device = device;
    g_device->AddRef();
    g_device->GetImmediateContext(&g_context);
    g_colorFormat = colorFormat;
    g_bbWidth = width;
    g_bbHeight = height;

    // [SVRDIAG 2026-07-20] SteamVR scene submission fails on THIS game's device with
    // "Failed to create sync texture" -> VRCompositorError_SharedTexturesNotSupported on every
    // ComposeLayerProjection (xrclient_MassEffect1.txt; ME2 identical, Hogwarts Legacy + SteamVR Home
    // fine on the same machine). The sync texture is a 1x1 KEYED-MUTEX shared texture created on the
    // device bound to the session. Reproduce SteamVR's exact call here and log the real HRESULT,
    // plus the sharing variants and the device's identity, so the failing precondition names itself.
    {
        char dline[256];
        const D3D_FEATURE_LEVEL fl = g_device->GetFeatureLevel();
        const UINT cflags = g_device->GetCreationFlags();
        std::snprintf(dline, sizeof(dline), "[SVRDIAG] device featureLevel=0x%04X creationFlags=0x%08X",
                      static_cast<unsigned>(fl), cflags);
        LogLine(dline);
        IDXGIDevice* dxgiDev = nullptr;
        if (SUCCEEDED(g_device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev))) && dxgiDev != nullptr)
        {
            IDXGIAdapter* adap = nullptr;
            if (SUCCEEDED(dxgiDev->GetAdapter(&adap)) && adap != nullptr)
            {
                DXGI_ADAPTER_DESC ad = {};
                adap->GetDesc(&ad);
                char name[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
                std::snprintf(dline, sizeof(dline), "[SVRDIAG] adapter='%s' luid=%08lX:%08lX",
                              name, static_cast<unsigned long>(ad.AdapterLuid.HighPart),
                              static_cast<unsigned long>(ad.AdapterLuid.LowPart));
                LogLine(dline);
                adap->Release();
            }
            dxgiDev->Release();
        }
        struct { const char* tag; UINT misc; } tries[] = {
            { "KEYEDMUTEX (SteamVR sync texture)", D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX },
            { "SHARED (legacy)",                   D3D11_RESOURCE_MISC_SHARED },
            { "KEYEDMUTEX|NTHANDLE",               D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE },
            { "none (control)",                    0 },
        };
        for (const auto& t : tries)
        {
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = 1;
            td.Height = 1;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = t.misc;
            ID3D11Texture2D* tex = nullptr;
            const HRESULT hr = g_device->CreateTexture2D(&td, nullptr, &tex);
            std::snprintf(dline, sizeof(dline), "[SVRDIAG] CreateTexture2D %s -> hr=0x%08lX %s",
                          t.tag, static_cast<unsigned long>(hr), SUCCEEDED(hr) ? "OK" : "FAIL");
            LogLine(dline);
            if (tex != nullptr) tex->Release();
        }

        // [SVRDIAG2] the process-wide-vs-device-params discriminator: a FRESH device created right here
        // (null adapter, HARDWARE, flags=0 - the exact recipe that succeeds in a standalone probe on
        // this machine, proxy loaded or not). If KEYEDMUTEX works on the fresh device inside this same
        // process, the game DEVICE's creation parameters are the poison; if it fails here too, the
        // process state is (Denuvo / overlay / injected hooks), and the fix moves accordingly.
        {
            ID3D11Device* fresh = nullptr;
            D3D_FEATURE_LEVEL flGot = static_cast<D3D_FEATURE_LEVEL>(0);
            const D3D_FEATURE_LEVEL want = D3D_FEATURE_LEVEL_11_0;
            // Suppress the proxy's factory capture: this create pulls an internal CreateDXGIFactory2,
            // and hooking that factory re-entrantly from the game's CreateSwapChain stack crashed boot.
            MELEVR::D3DCapture::SetFactoryCaptureSuppressed(true);
            const HRESULT chr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &want, 1,
                                                  D3D11_SDK_VERSION, &fresh, &flGot, nullptr);
            MELEVR::D3DCapture::SetFactoryCaptureSuppressed(false);
            std::snprintf(dline, sizeof(dline), "[SVRDIAG2] fresh in-process device hr=0x%08lX fl=0x%04X",
                          static_cast<unsigned long>(chr), static_cast<unsigned>(flGot));
            LogLine(dline);
            if (SUCCEEDED(chr) && fresh != nullptr)
            {
                D3D11_TEXTURE2D_DESC td = {};
                td.Width = 1;
                td.Height = 1;
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
                ID3D11Texture2D* tex = nullptr;
                const HRESULT khr = fresh->CreateTexture2D(&td, nullptr, &tex);
                std::snprintf(dline, sizeof(dline),
                              "[SVRDIAG2] fresh device KEYEDMUTEX -> hr=0x%08lX %s",
                              static_cast<unsigned long>(khr), SUCCEEDED(khr) ? "OK" : "FAIL");
                LogLine(dline);
                if (tex != nullptr) tex->Release();
                fresh->Release();
            }
        }
    }
    MELEVR::D3DCapture::SetRenderResolution(width, height);   // DIBR depth gate matches live res, not 2096
    LogLine("[XR] starting OpenXR session thread (game device captured, backbuffer " +
            std::to_string(width) + "x" + std::to_string(height) + " fmt " + std::to_string(colorFormat) + ").");

    HANDLE thread = CreateThread(nullptr, 0, XrThread, nullptr, 0, nullptr);
    if (thread != nullptr) CloseHandle(thread);
    else MELEVR::Logger::LogWindowsError("[XR] CreateThread for XR session failed", GetLastError());
}

void OnPresent(IDXGISwapChain* gameSwapChain) noexcept
{
    if (!g_ready.load(std::memory_order_acquire) || g_shutdown.load(std::memory_order_relaxed)) return;
    PollEvents();
    if (g_sessionRunning) RunFrame(gameSwapChain);
    // [MIRRORTHROTTLE] published for the Present hook: true once the headset is actually being driven,
    // so the hook knows the flat mirror present is cosmetic and can be skipped. See PresentHook.
    g_headsetDriven.store(g_sessionRunning, std::memory_order_release);
}

void FlatHudTick() noexcept
{
    // OpenXR disabled (no-headset flat test): RunFrame never runs, so drive the HUD/subtitle controls here.
    MELEVR::Pchud::Install();
    const MELEVR::Config::VrConfig& cfg = MELEVR::Config::Get();
    // Per-mode layout: stereo draws the UI per-eye (half width) so it uses its own *Stereo geometry set.
    const bool useStereoUi = cfg.stereoEnabled;
    const bool useDibrUi = cfg.dibrEnabled;   // DIBR owns its own PCHUD master layout (Stereo owns its own; AER/Mono share normal)
    MELEVR::Pchud::SetTransform(cfg.pchudEnabled,
                                useDibrUi ? cfg.pchudScaleXDibr  : (useStereoUi ? cfg.pchudScaleXStereo  : cfg.pchudScaleX),
                                useDibrUi ? cfg.pchudScaleYDibr  : (useStereoUi ? cfg.pchudScaleYStereo  : cfg.pchudScaleY),
                                useDibrUi ? cfg.pchudOffsetXDibr : (useStereoUi ? cfg.pchudOffsetXStereo : cfg.pchudOffsetX),
                                useDibrUi ? cfg.pchudOffsetYDibr : (useStereoUi ? cfg.pchudOffsetYStereo : cfg.pchudOffsetY));
    MELEVR::Pchud::SetConvoTransform(cfg.convoEnabled,
                                useDibrUi ? cfg.convoScaleXDibr  : (useStereoUi ? cfg.convoScaleXStereo  : cfg.convoScaleX),
                                useDibrUi ? cfg.convoScaleYDibr  : (useStereoUi ? cfg.convoScaleYStereo  : cfg.convoScaleY),
                                useDibrUi ? cfg.convoOffsetXDibr : (useStereoUi ? cfg.convoOffsetXStereo : cfg.convoOffsetX),
                                useDibrUi ? cfg.convoOffsetYDibr : (useStereoUi ? cfg.convoOffsetYStereo : cfg.convoOffsetY));
    // Designer UI (mission/boss scripted overlays - e.g. Saren's charge counter): one set of values, no
    // per-mode split - this is an occasional overlay, not a persistent per-mode HUD layout.
    MELEVR::Pchud::SetDesignerUiTransform(cfg.designerUiEnabled, cfg.designerUiScaleX, cfg.designerUiScaleY,
                                          cfg.designerUiOffsetX, cfg.designerUiOffsetY);
    MELEVR::Pchud::SetLaserUiTransform(cfg.laserUiEnabled, cfg.laserUiScaleX, cfg.laserUiScaleY,
                                       cfg.laserUiOffsetX, cfg.laserUiOffsetY);   // [LASERUI]
    MELEVR::Pchud::SetSubtitleMode(cfg.subtitleForce, cfg.subtitleMode);
    MELEVR::Pchud::SetSubtitleRedraw(cfg.subtitleRedraw, cfg.subtitleHideOriginal,
                                     useDibrUi ? cfg.subtitlePosXFracDibr : (useStereoUi ? cfg.subtitlePosXFracStereo : cfg.subtitlePosXFrac),
                                     useDibrUi ? cfg.subtitlePosYFracDibr : (useStereoUi ? cfg.subtitlePosYFracStereo : cfg.subtitlePosYFrac),
                                     useDibrUi ? cfg.subtitleScaleXDibr   : (useStereoUi ? cfg.subtitleScaleXStereo   : cfg.subtitleScaleX),
                                     useDibrUi ? cfg.subtitleScaleYDibr   : (useStereoUi ? cfg.subtitleScaleYStereo   : cfg.subtitleScaleY));
    MELEVR::Pchud::SetNativeSubtitleMove(cfg.nativeSubtitleEnabled, cfg.nativeSubtitlePosXFrac, cfg.nativeSubtitlePosYFrac,
                                         cfg.nativeSubtitleScaleX, cfg.nativeSubtitleScaleY, cfg.nativeSubtitleFontSize);
    PushElementConfigs(cfg);
    MELEVR::Pchud::Tick();
}

bool IsHdrDetected() noexcept { return g_hdrDetected.load(std::memory_order_relaxed); }
bool IsHeadsetDriven() noexcept { return g_headsetDriven.load(std::memory_order_acquire); }

bool GetResolutionStats(ResolutionStats* out) noexcept
{
    if (out == nullptr) return false;
    out->renderWidth = g_lastBackbufferWidth.load(std::memory_order_relaxed);
    out->renderHeight = g_lastBackbufferHeight.load(std::memory_order_relaxed);
    if (out->renderWidth == 0 || out->renderHeight == 0)
    {
        out->renderWidth = g_bbWidth;
        out->renderHeight = g_bbHeight;
    }
    out->sourceWidth = g_lastCopySourceWidth.load(std::memory_order_relaxed);
    out->sourceHeight = g_lastCopySourceHeight.load(std::memory_order_relaxed);
    if (out->sourceWidth == 0 || out->sourceHeight == 0)
    {
        out->sourceWidth = out->renderWidth;
        out->sourceHeight = out->renderHeight;
    }
    out->eyeWidth = g_eyes[0].width;
    out->eyeHeight = g_eyes[0].height;
    out->recommendedEyeWidth = g_recommendedEyeWidth.load(std::memory_order_relaxed);
    out->recommendedEyeHeight = g_recommendedEyeHeight.load(std::memory_order_relaxed);
    return out->eyeWidth != 0 && out->eyeHeight != 0;
}

void RequestShutdown() noexcept
{
    g_shutdown.store(true, std::memory_order_relaxed);
}

}
