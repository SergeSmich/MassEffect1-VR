#include "render_hook.h"
#include "head_aim.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <string>

#include <MinHook.h>

#include "logger.h"
#include "d3d_capture.h"
#include "le1_game.h"
#include "vr_config.h"

using MELEVR::Logger::LogLine;

// ============================================================================
// Milestone 1a head-look hook. Lifted from the proven CalcSceneView free-look in the parked tree
// (which rendered + culled coherently in a real headset), stripped to JUST the head yaw/pitch view
// rotation. Everything else the old hook did (AER eye shift, FOV fill, off-center lens, positional,
// depth capture, draw-replay banks) is gone.
// ============================================================================

namespace
{
// CalcSceneView: builds the per-frame FSceneView (the view+projection the GPU renders with).
// Module-base-relative; LE1 v2.0.0.48602 fact (proven hook point).
constexpr std::uintptr_t kCalcSceneViewRva = 0x4C5300;
constexpr std::uintptr_t kFViewportClientDrawRva = 0x4C71A0;
constexpr std::uintptr_t kFViewportClientDrawEndRva = 0x4C9434;
constexpr std::uintptr_t kGameplayCalcSceneViewReturnRva = 0x4C7A4E;
constexpr std::uintptr_t kLikelyAllocateViewStateRva = 0x742B70;
constexpr std::uintptr_t kSceneRenderBRva = 0x76EE20;
constexpr std::uintptr_t kSceneRenderCRva = 0x767900;
struct ChildProbe
{
    const char* tag;
    std::uintptr_t rva;
};

constexpr ChildProbe kPostCalcChildren[] = {
    {"CHILDA", 0x30EBF0}, // FVC direct child idx=47, callsite 0x4C7A95
    {"CHILDB", 0x072A70}, // FVC direct child idx=48, callsite 0x4C7AB4
    {"CHILDC", 0x063B00}, // FVC direct child idx=49, callsite 0x4C7AE7
    {"CHILDD", 0x3043B0}, // FVC direct child idx=50, callsite 0x4C7CD0
    {"CHILDE", 0x402170}, // FVC direct child idx=51, callsite 0x4C7D03
};
constexpr int kPostCalcChildCount = static_cast<int>(sizeof(kPostCalcChildren) / sizeof(kPostCalcChildren[0]));
constexpr int kActiveChildReplayIndex = 3; // CHILDD: clean render-thread, inside-FViewport child.

// FSceneView field offsets (FMatrix = 16 floats, row-major, row-vector convention: out = A * B).
constexpr std::uintptr_t kFsvState = 0x8;                        // FSceneView::State (FSceneViewStateInterface*) -
                                                                  // per-view TEMPORAL history (motion blur/TAA). Same
                                                                  // offset ME2 validated (calcview_hook.cpp kFsvState);
                                                                  // FSceneView is an engine-core UE3 type, not per-game
                                                                  // (see [[mele-trilogy-engine-closeness]]). See the
                                                                  // per-eye view-state fix below (STEREOGHOST).
constexpr std::uintptr_t kFsvViewMatrix = 0x90;                  // world -> view
constexpr std::uintptr_t kFsvProjectionMatrix = 0xD0;            // view -> clip
constexpr std::uintptr_t kFsvTranslatedViewMatrix = 0x190;       // camera-relative world -> view
constexpr std::uintptr_t kFsvTranslatedViewProjMatrix = 0x1D0;   // what the GPU reads
constexpr std::uintptr_t kFsvPreViewTranslation = 0x250;         // per-vertex world pre-translation (camera move)
constexpr std::uintptr_t kFsvViewProjectionMatrix = 0x260;       // the cull frustum
constexpr std::uintptr_t kFsvViewOrigin = 0x320;                 // camera world position
constexpr std::uintptr_t kLocalPlayerViewState = 0x46C;
constexpr std::uintptr_t kLocalPlayerOrigin = 0x59C;
constexpr std::uintptr_t kLocalPlayerSize = 0x5A4;
constexpr std::uintptr_t kFsvStateScanBytes = 0x800;

using CalcFn = void*(__fastcall*)(void*, void*, void*, void*, void*, void*);
using FViewportClientDrawFn = void(__fastcall*)(void*, void*, void*);
using SeamProbeFn = std::uintptr_t(__fastcall*)(void*, void*, void*, void*, void*, void*, void*, void*);
using AllocateViewStateFn = void*(__fastcall*)(uint32_t);

std::atomic_bool g_installed{false};
CalcFn g_origCalc = nullptr;
FViewportClientDrawFn g_origFViewportClientDraw = nullptr;
SeamProbeFn g_origSceneRenderB = nullptr;
SeamProbeFn g_origSceneRenderC = nullptr;
SeamProbeFn g_origPostCalcChildren[kPostCalcChildCount] = {};

// Head-look state, pushed every Present by xr_session (already signed; UE FRotator units).
std::atomic_bool g_headLookOn{false};
// [POSEEXACT] Which head sample the live render is consuming.
// SetHeadLook is armed once per present; g_headArmSeq counts those arms. The RENDER stamps
// g_pairArmSeq with whatever arm it actually latched. At submit, xr_session looks that arm up and
// tags the frame with the EXACT pose it was rendered with - instead of guessing how many presents
// back that was via poseTagDelayFrames, a hand-dialled constant that is only correct at the one
// framerate it was dialled at (which is why changing render resolution broke head tracking).
std::atomic<uint64_t> g_headArmSeq{0};
std::atomic<uint64_t> g_pairArmSeq{0};

std::atomic<int> g_headYawUU{0};
std::atomic<int> g_headPitchUU{0};

// Last rendered FOV half-angles (radians), read from the live projection matrix. xr_session matches them.
std::atomic_bool g_fovValid{false};
std::atomic<float> g_fovHalfHoriz{0.0f};
std::atomic<float> g_fovHalfVert{0.0f};
std::atomic_bool g_rawProjValid{false};
std::atomic<float> g_rawProjAspect{1.0f};
std::atomic<float> g_rawProjHalfHoriz{0.0f};
std::atomic<float> g_rawProjHalfVert{0.0f};
std::atomic_bool g_targetFovEnabled{false};
std::atomic<float> g_targetFovHalfHoriz{0.0f};
std::atomic<float> g_targetFovHalfVert{0.0f};
std::atomic<int> g_targetFovLogs{0};

// Static camera-view offset (view-relative UE units), pushed by xr_session from the menu's View settings.
std::atomic<float> g_viewOffRight{0.0f};
std::atomic<float> g_viewOffUp{0.0f};
std::atomic<float> g_viewOffFwd{0.0f};
std::atomic<int> g_viewOffEye{-1};
std::atomic_bool g_viewOffAer{false};
// [AERFULL-P1OWN] Keep AER eye ownership separate from the generic lean/base offset.
// Only the primary-player AER branch can advance the render-stamp sequence.
std::atomic_bool g_aerPrimaryEnabled{false};
std::atomic<int> g_aerPrimaryRenderEye{0};
std::atomic<float> g_aerPrimaryHalfEyeUU{0.0f};
std::atomic_bool g_aerPrimarySwapEyes{false};
std::atomic<std::uintptr_t> g_primaryLocalPlayer{0};
std::atomic<uint64_t> g_renderStampSeq{0};
std::atomic<int> g_renderStampEye{-1};
// [AERSHAKE 2026-08-21, ported from ME2/ME3 after field confirm in BOTH] The single-slot stamp was
// the full-rate AER shake: the stamp is written at BUILD time but the pixels land at PRESENT one
// frame later (UE3 pipelines), so latest-read labels the on-screen frame with the NEXT build's eye
// whenever the game thread runs ahead. At display/2 the mislabel was constant (a fixed eye swap -
// why aerSwapEyes defaulted TRUE); at full rate the pipeline depth flaps and the world visibly
// vibrates between the two eye positions. Fix: the build derives its eye from its own seq parity
// (L,R,L,R by construction - no present-armed cross-thread feedback), every build is queued
// {seq, eye} in a ring, and the present consumes OLDEST-FIRST, one per present, matching swapchain
// delivery order. The eye label can no longer disagree with the pixels.
constexpr int kAerRingN = 8;
struct AerRingSlot { std::atomic<uint64_t> seq{0}; std::atomic<int> eye{0}; };
AerRingSlot g_aerRing[kAerRingN];
std::atomic_bool g_renderStampAer{false};
std::atomic<uint64_t> g_presentSeq{0};
std::atomic<uint64_t> g_calcCallsThisPresent{0};
std::atomic<uint64_t> g_replayCalcCallsThisPresent{0};
std::atomic<uint64_t> g_replayCalcTotal{0};
std::atomic<uint64_t> g_fvcDrawCallsThisPresent{0};
std::atomic<uint64_t> g_fvcDrawSeq{0};
std::atomic<uint64_t> g_sceneBCallsThisPresent{0};
std::atomic<uint64_t> g_sceneCCallsThisPresent{0};
std::atomic<uint64_t> g_sceneBCalcThisPresent{0};
std::atomic<uint64_t> g_sceneCCalcThisPresent{0};
std::atomic<uint64_t> g_sceneBSeq{0};
std::atomic<uint64_t> g_sceneCSeq{0};
std::atomic<uint64_t> g_childSeq[kPostCalcChildCount] = {};
std::atomic<uint64_t> g_childCallsThisPresent[kPostCalcChildCount] = {};
std::atomic<uint64_t> g_childCalcThisPresent[kPostCalcChildCount] = {};
std::atomic<uint64_t> g_childReplayTotal{0};
std::atomic<int> g_childLogs[kPostCalcChildCount] = {};
std::atomic_bool g_fvcReplayArmed{false};

// [SFR] Same-Frame Rendering M0 (2026-07-14, adopted as the permanent-fix direction): render the WHOLE frame
// twice per present via a second FViewportClient::Draw - each pass is a bona fide single-view PRIMARY
// render (full lighting + post + bloom by construction; AER's frames are the existence proof, this is
// AER made same-frame). Pass 0 = game camera (left eye), pass 1 = replay-shifted right eye; the
// existing pass captures + [SYNCSTEREO] Mono-path submit publish the pair. Armed by
// MELEVR_ENABLE_SFR_M0.txt; never runs while P1 SBS stereo is building families (switching mode to
// Stereo in the Insert menu = live escape hatch).
std::atomic<float> g_sfrHalfEyeUU{0.8f};         // per-eye half separation (uu); xr_session publishes
                                                 // regular Stereo's tuned stereoHalfEyeUU every frame
// [SFRCONV] convergence: a small opposite per-eye horizontal projection shift (off-axis frustum) that pulls
// the zero-disparity (fusion) plane IN from infinity, so distant isolated elements (floating interaction
// prompts, markers) fuse without lowering the IPD/world scale. Published per frame by xr_session from
// cfg.sfr2Convergence. 0 = fusion at infinity (parallel eyes, current behavior).
std::atomic<float> g_sfrConvergence{0.0f};
std::atomic<uint64_t> g_sfrFrames{0};
std::atomic_bool g_sfrModeEnabled{false};        // vrMode 4 (Stereo 2) master switch, from xr_session
std::atomic<int> g_sfrGameMode{-1};              // EGameModes byte, from xr_session (0..4 = gameplay)
std::atomic_bool g_sfrConvoAllowed{false};       // [CINEVR2] cfg.cineVrConvo: mode 5 may also replay
std::atomic_bool g_sfrCutsceneAllowed{false};    // [CINEVR2] cfg.cineVrCutscene: mode 6 may also replay
std::atomic_bool g_sfrLiveGui7{false};           // [LIVEGUI] mode 7 with the world still SIMULATING
                                                 // (in-world GUI overlay, e.g. Eden Prime bomb disarm)

// One place for "may SFR double-render in this game mode": real gameplay always. Convo (5) and
// cutscene (6) behind their [CINEVR2] opt-in toggles (VR cine on = the replay must keep running or
// the submit holds the last gameplay pair = frozen headset; VR cine off = flat screen owns the
// submit and the replay would be waste). Movie 8 / galaxy 9 never. Mode 7 only when [LIVEGUI]
// proved the world is simulating under the GUI (possessed pawn + unpaused + sim clock advancing,
// computed by xr_session) - the Eden Prime bomb disarm reports 7 like a menu but is live gameplay.
// True menus never set the flag, and the FVC arm site independently requires a fresh perspective
// scene before any replay fires.
bool SfrModeAllowsReplay(int gameMode) noexcept
{
    if (gameMode >= 0 && gameMode <= 4) return true;
    if (gameMode == 5 && g_sfrConvoAllowed.load(std::memory_order_acquire)) return true;
    if (gameMode == 6 && g_sfrCutsceneAllowed.load(std::memory_order_acquire)) return true;
    if (gameMode == 7 && g_sfrLiveGui7.load(std::memory_order_acquire)) return true;
    return false;
}

// [CINEFOCUS] true while a VR convo/cutscene is actually replaying (same condition the replay gate
// uses for modes 5/6). Scopes the two cine eye-strain mechanisms (per-eye view states + adaptive
// cine render scale) so GAMEPLAY and flat cine stay bit-identical.
bool SfrCineActive() noexcept
{
    const int m = g_sfrGameMode.load(std::memory_order_acquire);
    return (m == 5 && g_sfrConvoAllowed.load(std::memory_order_acquire)) ||
           (m == 6 && g_sfrCutsceneAllowed.load(std::memory_order_acquire));
}
std::atomic<int> g_sfrExceptions{0};             // guarded-replay strikes; >=3 self-disarms SFR
std::atomic_bool g_childReplayAuto{false};
std::atomic<float> g_childReplayShiftRightUU{120.0f};
std::atomic<uint64_t> g_childReplayAutoLastPresent{0};
std::atomic<int> g_childReplayAutoCaptures{0};
std::atomic_bool g_p1StereoEnabled{false};
std::atomic<float> g_p1StereoHalfEyeUU{0.8f};
std::atomic_bool g_p1StereoSwapEyes{false};
std::atomic<float> g_p1StereoEyeAspect{1.0f};
std::atomic<float> g_p1StereoBackbufferAspect{1.0f};
std::atomic<uint64_t> g_p1LayoutStereoCalls{0};
std::atomic<int> g_p1StereoLogs{0};
std::atomic<uint64_t> g_sfxEarlyOwnershipSeq{0};
AllocateViewStateFn g_allocateViewState = nullptr;
void* g_allocatedStereoViewState = nullptr;
// STEREOGHOST fix (2026-07-12, ported from ME2's calcview_hook.cpp g_eyeState/g_eyeStateLeft): each eye needs
// its OWN private, persistent FSceneViewState (motion blur/TAA temporal history) or it borrows the game's own
// shared/native state - the same object the game overwrites every frame for its own rendering. That poisoned,
// mis-framed history shows as a translucent moving ghost, worst in high-contrast areas, invisible at low res
// at low resolution. ME1 previously gave NEITHER eye a private state (both shared
// the poisoned one); this gives each its own, mirroring ME2's proven fix.
void* g_stereoViewStateLeft = nullptr;
void* g_stereoViewStateRight = nullptr;
// ME1-specific AER isolation: alternating eye cameras must not feed one shared temporal history.
// Keep these slots separate from same-frame stereo so switching modes cannot import stale history.
void* g_aerViewStateLeft = nullptr;
void* g_aerViewStateRight = nullptr;
constexpr uint64_t kChildReplayAutoIntervalPresents = 4;
constexpr int kChildReplayAutoMaxCaptures = 1000000000;
std::atomic_bool g_f9WasDown{false};
std::atomic<DWORD> g_lastCalcThread{0};
std::atomic<DWORD> g_lastFvcDrawThread{0};
// [NOLETTERBOX] armed by xr_session while a VR cine is active in Stereo 2: clear the LE1 cine
// camera's 16:9 aspect constraint at Draw entry (game thread, pre-consumption), so cine renders
// full-frame exactly like gameplay - which is what LE2/LE3 cine always did. See head_aim.cpp.
std::atomic_bool g_cineUnconstrain{false};
std::atomic<int> g_noLetterboxClears{0};
std::atomic<DWORD> g_lastPresentThread{0};
std::atomic<int> g_stackLogs{0};
std::atomic<std::uintptr_t> g_seenCallers[16] = {};
std::atomic<int> g_calcSeqLogs{0};
std::atomic<int> g_replayCalcSeqLogs{0};
std::atomic<int> g_replayShiftLogs{0};
std::atomic_bool g_boundaryMapLogged{false};
std::atomic_bool g_fvcProbeHooksEnabled{false};
std::atomic<void*> g_lastPerspectiveFsv{nullptr};
std::atomic<uint64_t> g_lastPerspectiveFsvPresent{0};
thread_local int t_fvcDrawDepth = 0;
thread_local bool t_fvcReplay = false;
thread_local int t_sceneBDepth = 0;
thread_local int t_sceneCDepth = 0;
thread_local int t_childDepth[kPostCalcChildCount] = {};

// The game's base camera world position (ViewOrigin BEFORE the applied offset), captured each main view. xr_session
// reads its per-frame delta as the locomotion term of the AER comfort motion score.
std::atomic<float> g_baseVOx{0.0f};
std::atomic<float> g_baseVOy{0.0f};
std::atomic<float> g_baseVOz{0.0f};
std::atomic_bool g_baseVOValid{false};
std::atomic<float> g_lastRightX{1.0f};
std::atomic<float> g_lastRightY{0.0f};
std::atomic<float> g_lastRightZ{0.0f};
std::atomic_bool g_lastRightValid{false};

struct CalcSeqFields
{
    float p0;
    float p5;
    float p15;
    float fovH;
    float vo[3];
    float pvt[3];
};

struct FsvShiftSnapshot
{
    float vo[3];
    float pvt[3];
};

bool ApplyViewRelativeOffsetRawSEH(void* fsv, float r, float u, float f,
                                   float* outWx, float* outWy, float* outWz,
                                   float* outVo, float* outPvt) noexcept;
bool SnapshotFsvShiftFieldsSEH(void* fsv, FsvShiftSnapshot* out) noexcept;
bool RestoreFsvShiftFieldsSEH(void* fsv, const FsvShiftSnapshot& snap) noexcept;

bool MarkerEnabled(const wchar_t* name) noexcept
{
    if (name == nullptr) return false;
    wchar_t exePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash == nullptr) return false;
    *(slash + 1) = L'\0';
    wchar_t marker[MAX_PATH] = {};
    if (swprintf_s(marker, L"%s%s", exePath, name) <= 0) return false;
    return GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES;
}

// ReadViewInputSEH / WriteViewInputSEH removed 2026-07-03 (strip-firstperson-livecode): they were the
// read/write primitives behind the view-input pin (a live game-object write). Nothing calls them now.

// SfxModeNameContains / SfxModeIsStableForViewPin / ApplySfxViewInputPinProbe were REMOVED 2026-07-03
// (strip-firstperson-livecode). ApplySfxViewInputPinProbe WROTE the game's view location/rotation live to
// pin the camera through transitions - exactly the kind of per-frame game-object write being stripped.

bool CalcSequenceProbeEnabled() noexcept
{
    static int s_enabled = -1;
    if (s_enabled < 0) s_enabled = MarkerEnabled(L"MELEVR_ENABLE_CALCSEQ_LOG.txt") ? 1 : 0;
    return s_enabled == 1;
}

bool BoundaryProbeEnabled() noexcept
{
    static int s_enabled = -1;
    if (s_enabled < 0) s_enabled = MarkerEnabled(L"MELEVR_ENABLE_BOUNDARY_LOG.txt") ? 1 : 0;
    return s_enabled == 1;
}
// Row-major 4x4 multiply (UE FMatrix, row-vector: out = A * B). out must not alias A or B.
void Mul4x4(float* out, const float* A, const float* B) noexcept
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += A[i * 4 + k] * B[k * 4 + j];
            out[i * 4 + j] = s;
        }
}

// General 4x4 inverse (cofactor expansion, works for projection-bearing matrices too). Returns false on a
// near-singular determinant. Used by the [INVMAT] inverse-slot refresh.
bool Inverse4x4(const float* m, float* out) noexcept
{
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];

    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-25f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
    return true;
}

std::string Hex64(std::uintptr_t v)
{
    char b[32] = {};
    sprintf_s(b, "0x%llX", static_cast<unsigned long long>(v));
    return std::string(b);
}

std::string AddrLabel(void* p)
{
    const auto addr = reinterpret_cast<std::uintptr_t>(p);
    const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (exe != 0 && addr >= exe && addr < exe + 0x4000000)
    {
        return std::string("MassEffect1.exe+") + Hex64(addr - exe);
    }
    HMODULE mod = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(p),
                           &mod) && mod != nullptr &&
        GetModuleFileNameW(mod, path, MAX_PATH) > 0)
    {
        std::wstring ws(path);
        const size_t slash = ws.find_last_of(L"\\/");
        if (slash != std::wstring::npos) ws = ws.substr(slash + 1);
        char name[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, name, MAX_PATH, nullptr, nullptr);
        return std::string(name) + "+" + Hex64(addr - reinterpret_cast<std::uintptr_t>(mod));
    }
    return Hex64(addr);
}

std::string BytesAt(BYTE* base, std::uintptr_t rva, int count)
{
    char out[256] = {};
    char* p = out;
    size_t remaining = sizeof(out);
    for (int i = 0; i < count && remaining > 4; ++i)
    {
        const int written = sprintf_s(p, remaining, "%02X%s", base[rva + static_cast<std::uintptr_t>(i)], (i + 1 == count) ? "" : " ");
        if (written <= 0) break;
        p += written;
        remaining -= static_cast<size_t>(written);
    }
    return std::string(out);
}

bool Readable(const void* ptr, size_t size) noexcept
{
    if (ptr == nullptr || size == 0) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
    const auto start = reinterpret_cast<std::uintptr_t>(ptr);
    const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return start + size >= start && start + size <= end;
}

struct ViewFamilyInfo
{
    void** views = nullptr;
    uint32_t count = 0;
    uint32_t capacity = 0;
    uint64_t packedCountCapacity = 0;
    bool sane = false;
};

struct LocalPlayerLayout
{
    float originX = 0.0f;
    float originY = 0.0f;
    float sizeX = 0.0f;
    float sizeY = 0.0f;
};

ViewFamilyInfo ParseViewFamily(void* viewFamily) noexcept
{
    ViewFamilyInfo info = {};
    if (!Readable(viewFamily, 0x10)) return info;

    auto* bytes = static_cast<BYTE*>(viewFamily);
    __try
    {
        info.views = *reinterpret_cast<void***>(bytes + 0x0);
        info.packedCountCapacity = *reinterpret_cast<uint64_t*>(bytes + 0x8);
        info.count = static_cast<uint32_t>(info.packedCountCapacity & 0xFFFFFFFFu);
        info.capacity = static_cast<uint32_t>(info.packedCountCapacity >> 32);
        const uint32_t readableEntries = info.count > 0 ? info.count : 1;
        info.sane = info.views != nullptr &&
                    info.count <= info.capacity &&
                    info.count <= 16 &&
                    info.capacity <= 64 &&
                    Readable(info.views, static_cast<size_t>(readableEntries) * sizeof(void*));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        info = {};
    }
    return info;
}

bool TryReadPointerAt(const void* base, std::uintptr_t offset, void** out) noexcept
{
    if (out == nullptr) return false;
    *out = nullptr;
    if (base == nullptr) return false;
    const BYTE* ptr = static_cast<const BYTE*>(base) + offset;
    if (!Readable(ptr, sizeof(void*))) return false;

    __try
    {
        *out = *reinterpret_cast<void* const*>(ptr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *out = nullptr;
        return false;
    }
}

bool TryWritePointerAt(void* base, std::uintptr_t offset, void* value, void** oldValue) noexcept
{
    if (base == nullptr) return false;
    BYTE* ptr = static_cast<BYTE*>(base) + offset;
    if (!Readable(ptr, sizeof(void*))) return false;

    __try
    {
        if (oldValue != nullptr) *oldValue = *reinterpret_cast<void**>(ptr);
        *reinterpret_cast<void**>(ptr) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool TryReadFloatAt(const void* base, std::uintptr_t offset, float* out) noexcept
{
    if (out == nullptr) return false;
    *out = 0.0f;
    if (base == nullptr) return false;
    const BYTE* ptr = static_cast<const BYTE*>(base) + offset;
    if (!Readable(ptr, sizeof(float))) return false;

    __try
    {
        *out = *reinterpret_cast<const float*>(ptr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *out = 0.0f;
        return false;
    }
}

bool TryWriteFloatAt(void* base, std::uintptr_t offset, float value) noexcept
{
    if (base == nullptr) return false;
    BYTE* ptr = static_cast<BYTE*>(base) + offset;
    if (!Readable(ptr, sizeof(float))) return false;

    __try
    {
        *reinterpret_cast<float*>(ptr) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadLocalPlayerLayout(void* localPlayer, LocalPlayerLayout* out) noexcept
{
    if (localPlayer == nullptr || out == nullptr) return false;
    return TryReadFloatAt(localPlayer, kLocalPlayerOrigin + 0, &out->originX) &&
           TryReadFloatAt(localPlayer, kLocalPlayerOrigin + 4, &out->originY) &&
           TryReadFloatAt(localPlayer, kLocalPlayerSize + 0, &out->sizeX) &&
           TryReadFloatAt(localPlayer, kLocalPlayerSize + 4, &out->sizeY) &&
           std::isfinite(out->originX) &&
           std::isfinite(out->originY) &&
           std::isfinite(out->sizeX) &&
           std::isfinite(out->sizeY);
}

bool WriteLocalPlayerLayout(void* localPlayer, const LocalPlayerLayout& layout) noexcept
{
    if (localPlayer == nullptr ||
        !std::isfinite(layout.originX) ||
        !std::isfinite(layout.originY) ||
        !std::isfinite(layout.sizeX) ||
        !std::isfinite(layout.sizeY)) return false;

    bool ok = true;
    ok = TryWriteFloatAt(localPlayer, kLocalPlayerOrigin + 0, layout.originX) && ok;
    ok = TryWriteFloatAt(localPlayer, kLocalPlayerOrigin + 4, layout.originY) && ok;
    ok = TryWriteFloatAt(localPlayer, kLocalPlayerSize + 0, layout.sizeX) && ok;
    ok = TryWriteFloatAt(localPlayer, kLocalPlayerSize + 4, layout.sizeY) && ok;
    return ok;
}

std::string LocalPlayerLayoutText(const LocalPlayerLayout& layout) noexcept
{
    return "origin=[" + std::to_string(layout.originX) + "," + std::to_string(layout.originY) + "]" +
           " size=[" + std::to_string(layout.sizeX) + "," + std::to_string(layout.sizeY) + "]";
}

// ----------------------------------------------------------------------------
// [DYNRES] read-only offset validation for ULocalPlayer::DynamicResolutionFraction.
// kLocalPlayerDynResFraction (le1_game.h, 0x5C4) was recorded from an SDK dump and never exercised.
// Origin (0x59C) and Size (0x5A4) ARE proven - the stereo layout split rides on them every frame - so
// they anchor the dump: whatever object this is, those two read as a sane viewport rect. This prints the
// whole 0x580..0x5F0 window as float+hex so a wrong offset is diagnosable from one run instead of a
// guess-and-relaunch loop. NOTHING IS WRITTEN.
// ----------------------------------------------------------------------------
constexpr std::uintptr_t kDynResProbeStart = 0x580;
constexpr std::uintptr_t kDynResProbeEnd = 0x5F0;
constexpr int kDynResProbeMaxDumps = 8;

int g_dynResProbeDumps = 0;
ULONGLONG g_dynResProbeLastTick = 0;

void ProbeDynResFraction(void* localPlayer) noexcept
{
    if (localPlayer == nullptr) return;
    if (g_dynResProbeDumps >= kDynResProbeMaxDumps) return;

    const ULONGLONG now = GetTickCount64();
    if (g_dynResProbeLastTick != 0 && now - g_dynResProbeLastTick < 2000) return;
    g_dynResProbeLastTick = now;
    ++g_dynResProbeDumps;

    LocalPlayerLayout layout = {};
    const bool haveLayout = ReadLocalPlayerLayout(localPlayer, &layout);

    char header[192];
    std::snprintf(header, sizeof(header),
                  "[DYNRES] dump %d/%d localPlayer=0x%p anchorOk=%d %s",
                  g_dynResProbeDumps, kDynResProbeMaxDumps, localPlayer, haveLayout ? 1 : 0,
                  haveLayout ? LocalPlayerLayoutText(layout).c_str() : "(anchor read FAILED)");
    LogLine(header);

    for (std::uintptr_t off = kDynResProbeStart; off < kDynResProbeEnd; off += 4)
    {
        float value = 0.0f;
        const bool ok = TryReadFloatAt(localPlayer, off, &value);
        std::uint32_t raw = 0;
        std::memcpy(&raw, &value, sizeof(raw));

        const char* note = "";
        if (off == kLocalPlayerOrigin) note = "  <- Origin.X (proven)";
        else if (off == kLocalPlayerOrigin + 4) note = "  <- Origin.Y (proven)";
        else if (off == kLocalPlayerSize) note = "  <- Size.X (proven)";
        else if (off == kLocalPlayerSize + 4) note = "  <- Size.Y (proven)";
        else if (off == MELEVR::LE1::kLocalPlayerDynResFraction) note = "  <== CANDIDATE DynResFraction";

        char line[192];
        std::snprintf(line, sizeof(line), "[DYNRES]   +0x%03zX  ok=%d  f=%.6f  hex=0x%08X%s",
                      static_cast<size_t>(off), ok ? 1 : 0, ok ? value : 0.0f, ok ? raw : 0u, note);
        LogLine(line);
    }
}

// Write the supersample fraction. Offset validated against the LE1 SDK (Engine_classes.h ULocalPlayer:
// DynamicResolutionFraction @0x05C4) AND confirmed live - the whole surrounding struct read back exactly
// as the SDK describes it (PreviousDynResFrameIndex == -1, NearClipPlane == 10.0).
//
// The readback is the point: what's there gets read BEFORE writing. If the engine's dynamic-res driver ever
// starts fighting this write, the pre-write value will come back as something other than what was last written, and
// the log says so. That is how it becomes clear when to hook the WRITE instead of the read, rather than guessing.
constexpr float kDynResMin = 0.5f;
constexpr float kDynResMax = 3.0f;

ULONGLONG g_dynResApplyLastTick = 0;
float g_dynResLastWritten = 0.0f;

bool SfrCineActive() noexcept;   // defined below (near SfrModeAllowsReplay)

void ApplyDynResFraction(void* localPlayer) noexcept
{
    if (localPlayer == nullptr) return;

    float want = MELEVR::Config::Get().dynResFraction;
    // 1.0 (or anything non-finite/out of range) = do not touch the field at all. Byte-identical to
    // vanilla - EXCEPT a one-shot restore write of 1.0 if a smaller fraction was previously written;
    // without it the engine keeps rendering soft after the knob returns to 1.0.
    if (!std::isfinite(want) || want < kDynResMin || want > kDynResMax || want == 1.0f)
    {
        if (g_dynResLastWritten != 0.0f && g_dynResLastWritten != 1.0f)
        {
            LocalPlayerLayout restoreLayout = {};
            if (ReadLocalPlayerLayout(localPlayer, &restoreLayout) &&
                TryWriteFloatAt(localPlayer, MELEVR::LE1::kLocalPlayerDynResFraction, 1.0f))
            {
                g_dynResLastWritten = 1.0f;
                LogLine("[DYNRES] restored 1.0 (render scale released)");
            }
        }
        return;
    }

    float before = 0.0f;
    const bool readOk = TryReadFloatAt(localPlayer, MELEVR::LE1::kLocalPlayerDynResFraction, &before);
    // Refuse to write into an object whose viewport rect doesn't read sane - that means it isn't the
    // ULocalPlayer it's believed to be, and a blind float write is how this project earned its crash history.
    LocalPlayerLayout layout = {};
    if (!ReadLocalPlayerLayout(localPlayer, &layout)) return;

    const bool wrote = TryWriteFloatAt(localPlayer, MELEVR::LE1::kLocalPlayerDynResFraction, want);
    if (wrote) g_dynResLastWritten = want;

    const ULONGLONG now = GetTickCount64();
    if (g_dynResApplyLastTick == 0 || now - g_dynResApplyLastTick >= 1000)
    {
        g_dynResApplyLastTick = now;
        // stomped = the engine overwrote the written value between frames (pre-write read != what was last written).
        const bool stomped = readOk && g_dynResLastWritten != 0.0f &&
                             std::fabs(before - g_dynResLastWritten) > 1e-6f;
        char line[192];
        std::snprintf(line, sizeof(line),
                      "[DYNRES] apply want=%.3f readBefore=%.6f readOk=%d wrote=%d stompedByEngine=%d",
                      want, before, readOk ? 1 : 0, wrote ? 1 : 0, stomped ? 1 : 0);
        LogLine(line);
    }
}

int ReplacePointerMatches(void* base, std::uintptr_t scanBytes, const void* needle, void* replacement) noexcept
{
    if (base == nullptr || needle == nullptr || replacement == nullptr || scanBytes < sizeof(void*)) return 0;

    int writes = 0;
    for (std::uintptr_t offset = 0; offset + sizeof(void*) <= scanBytes; offset += sizeof(void*))
    {
        void* value = nullptr;
        if (!TryReadPointerAt(base, offset, &value)) break;
        if (value != needle) continue;
        if (TryWritePointerAt(base, offset, replacement, nullptr)) ++writes;
    }
    return writes;
}

bool FindSharedViewStateOffset(const void* firstView,
                               const void* secondView,
                               const void* localViewState,
                               std::uintptr_t* outOffset) noexcept
{
    if (outOffset == nullptr) return false;
    *outOffset = 0;
    if (firstView == nullptr || secondView == nullptr || localViewState == nullptr) return false;

    for (std::uintptr_t offset = 0; offset + sizeof(void*) <= kFsvStateScanBytes; offset += sizeof(void*))
    {
        void* firstValue = nullptr;
        void* secondValue = nullptr;
        if (!TryReadPointerAt(firstView, offset, &firstValue) ||
            !TryReadPointerAt(secondView, offset, &secondValue)) return false;

        if (firstValue == localViewState && secondValue == localViewState)
        {
            *outOffset = offset;
            return true;
        }
    }
    return false;
}

void* CallAllocateViewStateSEH(AllocateViewStateFn fn) noexcept
{
    if (fn == nullptr) return nullptr;
    void* state = nullptr;
    __try
    {
        state = fn(0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        state = nullptr;
    }
    return state;
}

// Allocates into `cache` once and reuses it thereafter (re-checked readable each call in case the engine
// ever frees it). `tag` names the slot in the log ("left"/"right") so the two allocations are distinguishable.
void* EnsureAllocatedViewStateSlot(void*& cache, const char* tag) noexcept
{
    if (cache != nullptr && Readable(cache, 0x40)) return cache;

    if (g_allocateViewState == nullptr)
    {
        auto* moduleBase = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
        if (moduleBase != nullptr) g_allocateViewState = reinterpret_cast<AllocateViewStateFn>(moduleBase + kLikelyAllocateViewStateRva);
    }
    if (g_allocateViewState == nullptr) return nullptr;

    void* state = CallAllocateViewStateSEH(g_allocateViewState);
    if (state != nullptr && Readable(state, 0x40))
    {
        cache = state;
        void* vtable = nullptr;
        TryReadPointerAt(state, 0, &vtable);
        LogLine(std::string("[STEREOGHOST] allocated standalone ViewState (") + tag + ") state=" +
                Hex64(reinterpret_cast<std::uintptr_t>(state)) + " vtable=" + Hex64(reinterpret_cast<std::uintptr_t>(vtable)));
    }
    else
    {
        LogLine(std::string("[STEREOGHOST] AllocateViewState(0) failed or returned unreadable (") + tag +
                ") state=" + Hex64(reinterpret_cast<std::uintptr_t>(state)));
    }
    return cache;
}

// Legacy single-slot wrapper - kept for the (currently unused) generic call site, now backed by the left slot.
void* EnsureAllocatedStereoViewState() noexcept
{
    return EnsureAllocatedViewStateSlot(g_allocatedStereoViewState, "legacy");
}

// Writes FSceneView::State (SEH-guarded, same pattern as every other Fsv field write in this file). Only
// ever called with a non-null, freshly-validated state - NEVER force NULL onto the field (an explicit NULL
// state is itself a smear source per the ME2 fix notes), so a failed allocation just leaves the eye on
// whatever state the engine already set (the prior, poisoned-but-not-worse behavior).
void SetViewState(void* fsv, void* state) noexcept
{
    if (fsv == nullptr || state == nullptr) return;
    __try
    {
        *reinterpret_cast<void* volatile*>(reinterpret_cast<BYTE*>(fsv) + kFsvState) = state;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

std::uintptr_t ModuleImageSize(BYTE* base) noexcept
{
    __try
    {
        if (base == nullptr) return 0;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        return static_cast<std::uintptr_t>(nt->OptionalHeader.SizeOfImage);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

bool RvaRangeValid(std::uintptr_t rva, std::uintptr_t count, std::uintptr_t imageSize) noexcept
{
    return imageSize != 0 && rva < imageSize && count <= imageSize - rva;
}

std::string SafeBytesAt(BYTE* base, std::uintptr_t rva, int count, std::uintptr_t imageSize)
{
    if (count <= 0 || !RvaRangeValid(rva, static_cast<std::uintptr_t>(count), imageSize))
        return "<out-of-image>";
    return BytesAt(base, rva, count);
}

uint64_t NowQpc() noexcept
{
    LARGE_INTEGER v = {};
    QueryPerformanceCounter(&v);
    return static_cast<uint64_t>(v.QuadPart);
}

uint64_t QpcToUs(uint64_t start, uint64_t end) noexcept
{
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f = {};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    if (freq.QuadPart <= 0 || end < start) return 0;
    return static_cast<uint64_t>(((end - start) * 1000000ULL) / static_cast<uint64_t>(freq.QuadPart));
}

void LogRetSiteDecode(BYTE* base, std::uintptr_t retRva)
{
    if (retRva < 8) return;
    const std::uintptr_t callRva = retRva - 5;
    const BYTE op = base[callRva];
    if (op == 0xE8)
    {
        const int32_t rel = *reinterpret_cast<const int32_t*>(base + callRva + 1);
        const std::uintptr_t target = callRva + 5 + static_cast<std::intptr_t>(rel);
        LogLine("[FUNCMAP] ret=" + Hex64(retRva) +
                " precall=" + Hex64(callRva) +
                " E8target=" + Hex64(target) +
                " bytes=" + BytesAt(base, callRva, 8));
    }
    else
    {
        LogLine("[FUNCMAP] ret=" + Hex64(retRva) +
                " precall=" + Hex64(callRva) +
                " op=" + Hex64(op) +
                " bytesAround=" + BytesAt(base, retRva - 8, 16));
    }
}

void LogFvcChildCallsOnce(BYTE* base) noexcept
{
    const std::uintptr_t imageSize = ModuleImageSize(base);
    if (!RvaRangeValid(kFViewportClientDrawRva,
                       kFViewportClientDrawEndRva - kFViewportClientDrawRva,
                       imageSize))
    {
        LogLine("[FVCCHILD] skipped: FViewportClient::Draw range is outside image bounds");
        return;
    }

    LogLine("[FVCCHILD] ---- direct E8 calls inside FViewportClient::Draw ----");
    int count = 0;
    for (std::uintptr_t rva = kFViewportClientDrawRva; rva + 5 < kFViewportClientDrawEndRva; ++rva)
    {
        if (base[rva] != 0xE8) continue;

        const int32_t rel = *reinterpret_cast<const int32_t*>(base + rva + 1);
        const int64_t signedTarget = static_cast<int64_t>(rva) + 5 + static_cast<int64_t>(rel);
        const bool targetInImage =
            signedTarget >= 0 &&
            RvaRangeValid(static_cast<std::uintptr_t>(signedTarget), 1, imageSize);
        const std::uintptr_t target = targetInImage ? static_cast<std::uintptr_t>(signedTarget) : 0;

        std::string bounds = targetInImage ? "func=<no-pdata>" : "func=<external/out-of-image>";
        if (targetInImage)
        {
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(base + target), &imageBase, nullptr);
            if (fn != nullptr)
            {
                bounds = "func=[" + Hex64(fn->BeginAddress) + "," + Hex64(fn->EndAddress) + ")";
            }
        }

        LogLine("[FVCCHILD] idx=" + std::to_string(count) +
                " callsite=" + Hex64(rva) +
                " ret=" + Hex64(rva + 5) +
                " target=" + (targetInImage ? Hex64(target) : std::string("<out-of-image>")) +
                " " + bounds +
                " callBytes=" + SafeBytesAt(base, rva, 8, imageSize) +
                " targetBytes=" + SafeBytesAt(base, target, 12, imageSize));
        ++count;
    }
    LogLine("[FVCCHILD] totalDirectCalls=" + std::to_string(count));
}

void LogFunctionBoundsOnce() noexcept
{
    bool expected = false;
    if (!g_boundaryMapLogged.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;

    BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    if (base == nullptr) return;

    constexpr std::uintptr_t kCandidates[] = {
        0x4C71A0, 0x4C7A4E, 0x4C8366,
        0x3110E4, 0x3113A9,
        0x3C7E6D, 0x3C6CCC, 0x3CAA05, 0x3BA2E0, 0x3C32DD,
        0x9B9190, 0x9B8D34, 0x9BC327, 0x9B8DA0, 0x9BDBBE, 0xF09322,
        0x76EE20, 0x767913, 0x2594FE, 0x2599EC, 0x187524
    };

    LogLine("[FUNCMAP] ---- boundary candidate function map ----");
    for (const std::uintptr_t rva : kCandidates)
    {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(base + rva), &imageBase, nullptr);
        if (fn != nullptr)
        {
            LogLine("[FUNCMAP] rva=" + Hex64(rva) +
                    " func=[" + Hex64(fn->BeginAddress) + "," + Hex64(fn->EndAddress) + ")" +
                    " unwind=" + Hex64(fn->UnwindData) +
                    " bytes=" + BytesAt(base, rva, 12));
        }
        else
        {
            LogLine("[FUNCMAP] rva=" + Hex64(rva) +
                    " func=<no-pdata>" +
                    " bytes=" + BytesAt(base, rva, 12));
        }
        LogRetSiteDecode(base, rva);
    }
    LogFvcChildCallsOnce(base);
}

void LogCalcSceneStackOnce() noexcept
{
    void* frames[16] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, 16, frames, nullptr);
    if (n < 4) return;
    const auto caller = reinterpret_cast<std::uintptr_t>(frames[2]);

    for (auto& slot : g_seenCallers)
    {
        const std::uintptr_t seen = slot.load(std::memory_order_acquire);
        if (seen == caller) return;
        if (seen == 0)
        {
            std::uintptr_t expected = 0;
            if (slot.compare_exchange_strong(expected, caller, std::memory_order_acq_rel)) break;
        }
    }

    const int logIndex = g_stackLogs.fetch_add(1, std::memory_order_acq_rel);
    if (logIndex >= 12) return;

    std::string line = "[BOUNDARYSTACK] idx=" + std::to_string(logIndex) +
                       " present=" + std::to_string(g_presentSeq.load(std::memory_order_relaxed)) +
                       " thread=" + std::to_string(GetCurrentThreadId()) +
                       " stack=";
    for (USHORT i = 1; i < n && i < 10; ++i)
    {
        if (i > 1) line += " <- ";
        line += AddrLabel(frames[i]);
    }
    LogLine(line);
}


bool ReadCalcSeqFieldsSEH(void* fsv, CalcSeqFields* out) noexcept
{
    if (fsv == nullptr || out == nullptr) return false;
    __try
    {
        const BYTE* base = reinterpret_cast<const BYTE*>(fsv);
        const float* P = reinterpret_cast<const float*>(base + kFsvProjectionMatrix);
        const float* vo = reinterpret_cast<const float*>(base + kFsvViewOrigin);
        const float* pvt = reinterpret_cast<const float*>(base + kFsvPreViewTranslation);
        out->p0 = P[0];
        out->p5 = P[5];
        out->p15 = P[15];
        out->fovH = (std::fabs(P[0]) > 0.0001f)
            ? (2.0f * std::atan(1.0f / P[0]) * 57.2957795f)
            : 0.0f;
        out->vo[0] = vo[0]; out->vo[1] = vo[1]; out->vo[2] = vo[2];
        out->pvt[0] = pvt[0]; out->pvt[1] = pvt[1]; out->pvt[2] = pvt[2];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void LogCalcSequence(void* fsv, uint64_t ordinalInPresent, bool replayPhase) noexcept
{
    const int logIndex = replayPhase
        ? g_replayCalcSeqLogs.fetch_add(1, std::memory_order_acq_rel)
        : g_calcSeqLogs.fetch_add(1, std::memory_order_acq_rel);
    if ((!replayPhase && logIndex >= 240) || (replayPhase && logIndex >= 64)) return;

    CalcSeqFields fields = {};
    if (!ReadCalcSeqFieldsSEH(fsv, &fields))
    {
        LogLine(std::string(replayPhase ? "[CALCSEQ_REPLAY]" : "[CALCSEQ]") + " failed to read FSceneView fields.");
        return;
    }

    void* frames[8] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, 8, frames, nullptr);
    const std::string caller = (n >= 3) ? AddrLabel(frames[2]) : std::string("<unknown>");

    char line[768] = {};
    sprintf_s(line,
              "%s n=%d present=%llu ordinal=%llu replay=%d thread=%lu caller=%s fsv=0x%p P0=%.4f P5=%.4f P15=%.4f fovH=%.2f VO=(%.1f,%.1f,%.1f) PVT=(%.1f,%.1f,%.1f)",
              replayPhase ? "[CALCSEQ_REPLAY]" : "[CALCSEQ]",
              logIndex,
              static_cast<unsigned long long>(g_presentSeq.load(std::memory_order_relaxed)),
              static_cast<unsigned long long>(ordinalInPresent),
              replayPhase ? 1 : 0,
              static_cast<unsigned long>(GetCurrentThreadId()),
              caller.c_str(),
              fsv,
              fields.p0, fields.p5, fields.p15, fields.fovH,
              fields.vo[0], fields.vo[1], fields.vo[2],
              fields.pvt[0], fields.pvt[1], fields.pvt[2]);
    LogLine(line);
}

void LogFvcDrawCall(uint64_t seq, void* self, void* viewport, void* canvas, int depth) noexcept
{
    if (seq > 32 && (seq % 300) != 1) return;

    void* frames[10] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, 10, frames, nullptr);

    std::string line = "[FVCDRAW] seq=" + std::to_string(seq) +
                       " present=" + std::to_string(g_presentSeq.load(std::memory_order_relaxed)) +
                       " thread=" + std::to_string(GetCurrentThreadId()) +
                       " depth=" + std::to_string(depth) +
                       " self=" + Hex64(reinterpret_cast<std::uintptr_t>(self)) +
                       " viewport=" + Hex64(reinterpret_cast<std::uintptr_t>(viewport)) +
                       " canvas=" + Hex64(reinterpret_cast<std::uintptr_t>(canvas)) +
                       " stack=";
    for (USHORT i = 1; i < n && i < 8; ++i)
    {
        if (i > 1) line += " <- ";
        line += AddrLabel(frames[i]);
    }
    LogLine(line);
}

void LogSeamCall(const char* name,
                 std::uintptr_t rva,
                 uint64_t seq,
                 uint64_t present,
                 uint64_t calcBefore,
                 uint64_t calcAfter,
                 uint64_t durationUs,
                 int depth,
                 int insideFvc,
                 int insideC,
                 int replayPhase) noexcept
{
    if (seq > 64 && (seq % 300) != 1) return;

    char line[512] = {};
    sprintf_s(line,
              "[SEAM] name=%s rva=%s seq=%llu present=%llu thread=%lu depth=%d insideFvc=%d insideC=%d replay=%d calcDelta=%llu durationUs=%llu",
              name,
              Hex64(rva).c_str(),
              static_cast<unsigned long long>(seq),
              static_cast<unsigned long long>(present),
              static_cast<unsigned long>(GetCurrentThreadId()),
              depth,
              insideFvc,
              insideC,
              replayPhase,
              static_cast<unsigned long long>(calcAfter - calcBefore),
              static_cast<unsigned long long>(durationUs));
    LogLine(line);
}

std::uintptr_t __fastcall HookSceneRenderB(void* a1, void* a2, void* a3, void* a4,
                                           void* a5, void* a6, void* a7, void* a8) noexcept
{
    if (g_origSceneRenderB == nullptr) return 0;

    const uint64_t seq = g_sceneBSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint64_t present = g_presentSeq.load(std::memory_order_relaxed);
    const uint64_t calcBefore = g_calcCallsThisPresent.load(std::memory_order_relaxed);
    const uint64_t start = NowQpc();
    const int insideFvc = t_fvcDrawDepth > 0 ? 1 : 0;
    const int insideC = t_sceneCDepth > 0 ? 1 : 0;
    const int replayPhase = t_fvcReplay ? 1 : 0;

    g_sceneBCallsThisPresent.fetch_add(1, std::memory_order_relaxed);
    ++t_sceneBDepth;
    std::uintptr_t ret = g_origSceneRenderB(a1, a2, a3, a4, a5, a6, a7, a8);
    --t_sceneBDepth;

    const uint64_t end = NowQpc();
    const uint64_t calcAfter = g_calcCallsThisPresent.load(std::memory_order_relaxed);
    LogSeamCall("B", kSceneRenderBRva, seq, present, calcBefore, calcAfter,
                QpcToUs(start, end), t_sceneBDepth + 1, insideFvc, insideC, replayPhase);
    return ret;
}

std::uintptr_t __fastcall HookSceneRenderC(void* a1, void* a2, void* a3, void* a4,
                                           void* a5, void* a6, void* a7, void* a8) noexcept
{
    if (g_origSceneRenderC == nullptr) return 0;

    const uint64_t seq = g_sceneCSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint64_t present = g_presentSeq.load(std::memory_order_relaxed);
    const uint64_t calcBefore = g_calcCallsThisPresent.load(std::memory_order_relaxed);
    const uint64_t start = NowQpc();
    const int insideFvc = t_fvcDrawDepth > 0 ? 1 : 0;
    const int replayPhase = t_fvcReplay ? 1 : 0;

    g_sceneCCallsThisPresent.fetch_add(1, std::memory_order_relaxed);
    ++t_sceneCDepth;
    std::uintptr_t ret = g_origSceneRenderC(a1, a2, a3, a4, a5, a6, a7, a8);
    --t_sceneCDepth;

    const uint64_t end = NowQpc();
    const uint64_t calcAfter = g_calcCallsThisPresent.load(std::memory_order_relaxed);
    LogSeamCall("C", kSceneRenderCRva, seq, present, calcBefore, calcAfter,
                QpcToUs(start, end), t_sceneCDepth + 1, insideFvc, 1, replayPhase);
    return ret;
}

std::uintptr_t HookPostCalcChild(int index,
                                 void* a1, void* a2, void* a3, void* a4,
                                 void* a5, void* a6, void* a7, void* a8) noexcept
{
    if (index < 0 || index >= kPostCalcChildCount) return 0;
    if (g_origPostCalcChildren[index] == nullptr) return 0;

    const ChildProbe& probe = kPostCalcChildren[index];
    const uint64_t seq = g_childSeq[index].fetch_add(1, std::memory_order_acq_rel) + 1;
    const uint64_t present = g_presentSeq.load(std::memory_order_relaxed);
    const uint64_t calcBefore = g_calcCallsThisPresent.load(std::memory_order_relaxed);
    const uint64_t start = NowQpc();
    const int insideFvc = t_fvcDrawDepth > 0 ? 1 : 0;
    const int replayPhase = t_fvcReplay ? 1 : 0;

    g_childCallsThisPresent[index].fetch_add(1, std::memory_order_relaxed);
    ++t_childDepth[index];
    std::uintptr_t ret = g_origPostCalcChildren[index](a1, a2, a3, a4, a5, a6, a7, a8);
    --t_childDepth[index];

    const uint64_t end = NowQpc();
    const uint64_t calcAfter = g_calcCallsThisPresent.load(std::memory_order_relaxed);
    const uint64_t calcDelta = calcAfter - calcBefore;
    if (calcDelta > 0)
    {
        g_childCalcThisPresent[index].fetch_add(calcDelta, std::memory_order_relaxed);
    }

    bool autoReplayArmed =
        index == kActiveChildReplayIndex &&
        insideFvc &&
        !replayPhase &&
        g_childReplayAuto.load(std::memory_order_acquire);
    if (autoReplayArmed &&
        g_childReplayAutoCaptures.load(std::memory_order_acquire) >= kChildReplayAutoMaxCaptures)
    {
        autoReplayArmed = false;
    }
    if (autoReplayArmed)
    {
        uint64_t previousAutoPresent = g_childReplayAutoLastPresent.load(std::memory_order_acquire);
        const bool intervalReady =
            previousAutoPresent == 0 ||
            present >= previousAutoPresent + kChildReplayAutoIntervalPresents;
        if (!intervalReady)
        {
            autoReplayArmed = false;
        }
        else
        {
            autoReplayArmed = g_childReplayAutoLastPresent.compare_exchange_strong(
                previousAutoPresent,
                present,
                std::memory_order_acq_rel,
                std::memory_order_acquire);
        }
    }
    const bool oneShotReplayArmed =
        index == kActiveChildReplayIndex &&
        insideFvc &&
        !replayPhase &&
        g_fvcReplayArmed.exchange(false, std::memory_order_acq_rel);
    if (autoReplayArmed || oneShotReplayArmed)
    {
        const uint64_t replaySeq = g_childReplayTotal.fetch_add(1, std::memory_order_acq_rel) + 1;
        void* replayFsv = g_lastPerspectiveFsv.load(std::memory_order_acquire);
        const uint64_t replayFsvPresent = g_lastPerspectiveFsvPresent.load(std::memory_order_acquire);
        FsvShiftSnapshot replaySnap = {};
        bool replayShifted = false;
        bool replayRestored = false;
        float wx = 0.0f, wy = 0.0f, wz = 0.0f;
        float vo[3] = {};
        float pvt[3] = {};
        const bool autoReplay = g_childReplayAuto.load(std::memory_order_acquire);
        const float childReplayRightUU = autoReplay ?
            g_childReplayShiftRightUU.load(std::memory_order_relaxed) :
            120.0f;
        if (replayFsv != nullptr && replayFsvPresent == present &&
            SnapshotFsvShiftFieldsSEH(replayFsv, &replaySnap))
        {
            replayShifted = ApplyViewRelativeOffsetRawSEH(replayFsv, childReplayRightUU, 0.0f, 0.0f,
                                                          &wx, &wy, &wz, vo, pvt);
        }
        const bool pass0Ok = MELEVR::D3DCapture::CaptureStereoPass(0);
        LogLine(std::string("[CHILDREPLAY] firing ") + probe.tag +
                " one-shot second call seq=" + std::to_string(replaySeq) +
                " present=" + std::to_string(present) +
                " thread=" + std::to_string(GetCurrentThreadId()) +
                " fsv=" + Hex64(reinterpret_cast<std::uintptr_t>(replayFsv)) +
                " fsvPresent=" + std::to_string(replayFsvPresent) +
                " shifted=" + std::to_string(replayShifted ? 1 : 0) +
                " pass0=" + std::to_string(pass0Ok ? 1 : 0));
        const uint64_t replayStart = NowQpc();
        t_fvcReplay = true;
        ++t_childDepth[index];
        std::uintptr_t replayRet = g_origPostCalcChildren[index](a1, a2, a3, a4, a5, a6, a7, a8);
        --t_childDepth[index];
        t_fvcReplay = false;
        if (replayShifted)
        {
            replayRestored = RestoreFsvShiftFieldsSEH(replayFsv, replaySnap);
        }
        const bool pass1Ok = MELEVR::D3DCapture::CaptureStereoPass(1);
        const uint64_t replayEnd = NowQpc();
        LogLine(std::string("[CHILDREPLAY] survived ") + probe.tag +
                " second call seq=" + std::to_string(replaySeq) +
                " durationUs=" + std::to_string(QpcToUs(replayStart, replayEnd)) +
                " ret=" + Hex64(replayRet) +
                " restored=" + std::to_string(replayRestored ? 1 : 0) +
                " pass1=" + std::to_string(pass1Ok ? 1 : 0));
        if (autoReplay)
        {
            const int captures = g_childReplayAutoCaptures.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (captures >= kChildReplayAutoMaxCaptures)
            {
                LogLine("[CHILDREPLAY] auto replay capture cap reached; further auto captures suppressed.");
            }
        }
        if (replayShifted && replaySeq <= 16)
        {
            char line[384] = {};
            sprintf_s(line,
                      "[CHILDREPLAY] shifted %s rightUU=%.1f worldDelta=(%.1f,%.1f,%.1f) VO=(%.1f,%.1f,%.1f) PVT=(%.1f,%.1f,%.1f)",
                      probe.tag,
                      childReplayRightUU,
                      wx, wy, wz,
                      vo[0], vo[1], vo[2],
                      pvt[0], pvt[1], pvt[2]);
            LogLine(line);
        }
    }

    const int logIndex = g_childLogs[index].fetch_add(1, std::memory_order_acq_rel);
    if (logIndex < 160 || (seq % 240) == 0)
    {
        char line[512] = {};
        sprintf_s(line,
                  "[%s] rva=%s seq=%llu present=%llu thread=%lu depth=%d insideFvc=%d replay=%d calcDelta=%llu durationUs=%llu ret=%s",
                  probe.tag,
                  Hex64(probe.rva).c_str(),
                  static_cast<unsigned long long>(seq),
                  static_cast<unsigned long long>(present),
                  static_cast<unsigned long>(GetCurrentThreadId()),
                  t_childDepth[index] + 1,
                  insideFvc,
                  replayPhase,
                  static_cast<unsigned long long>(calcDelta),
                  static_cast<unsigned long long>(QpcToUs(start, end)),
                  Hex64(ret).c_str());
        LogLine(line);
    }
    return ret;
}

std::uintptr_t __fastcall HookPostCalcChildA(void* a1, void* a2, void* a3, void* a4,
                                             void* a5, void* a6, void* a7, void* a8) noexcept
{
    return HookPostCalcChild(0, a1, a2, a3, a4, a5, a6, a7, a8);
}

std::uintptr_t __fastcall HookPostCalcChildB(void* a1, void* a2, void* a3, void* a4,
                                             void* a5, void* a6, void* a7, void* a8) noexcept
{
    return HookPostCalcChild(1, a1, a2, a3, a4, a5, a6, a7, a8);
}

std::uintptr_t __fastcall HookPostCalcChildC(void* a1, void* a2, void* a3, void* a4,
                                             void* a5, void* a6, void* a7, void* a8) noexcept
{
    return HookPostCalcChild(2, a1, a2, a3, a4, a5, a6, a7, a8);
}

std::uintptr_t __fastcall HookPostCalcChildD(void* a1, void* a2, void* a3, void* a4,
                                             void* a5, void* a6, void* a7, void* a8) noexcept
{
    return HookPostCalcChild(3, a1, a2, a3, a4, a5, a6, a7, a8);
}

std::uintptr_t __fastcall HookPostCalcChildE(void* a1, void* a2, void* a3, void* a4,
                                             void* a5, void* a6, void* a7, void* a8) noexcept
{
    return HookPostCalcChild(4, a1, a2, a3, a4, a5, a6, a7, a8);
}

// [CINETILT 2026-08-12] Per-frame cine camera telemetry, published for the [CINEJIT] line: the
// camera's tilt (angle between world up as this camera sees it and the camera's own up) and the
// largest per-frame camera basis rotation since last read. The shake is scene-dependent and every
// delivery metric reads clean, so the missing datum is WHAT THE CAMERA WAS DOING during the felt
// seconds - these two numbers let a log timestamp answer that after the run.
std::atomic<int> g_cineTiltCdeg{0};       // last tilt, centi-degrees
std::atomic<int> g_cineCamStepCdegMax{0}; // max per-frame basis rotation since last read, centi-degrees
// [LOOKSTEP] the head-look angles the MAIN pass latched for the pair being rendered, published for
// the submit-side evenness probe.
std::atomic<int> g_pairYawPub{0};
std::atomic<int> g_pairPitchPub{0};
// [CINESAMPLE 2026-08-12] Coherent latest-publish snapshot for tracked cine. The failed LOOKLERP
// and LOOKPACE experiments tried to manufacture a second clock here. ME2 does neither: it smooths
// the one XR pose upstream, then the render consumes the latest complete publish. Keep the double
// buffer only to make {on,yaw,pitch,arm} one indivisible sample across the Present/render threads.
struct LookSlot
{
    int yaw = 0, pitch = 0;
    uint64_t arm = 0;
    bool on = false;
};
LookSlot g_lookSlots[2];
std::atomic<int> g_lookSlotIdx{0};
inline void AtomicMaxInt(std::atomic<int>& a, int v) noexcept
{
    int cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

// Rotate the RENDERED view by a head yaw/pitch (view space: X=right, Y=up, Z=forward). Post-multiply the
// view matrices by the rotation, then recompute the GPU-read products (ViewProjection + Translated) so
// render AND cull rotate together - no pop-in. Roll is NEVER applied (kept level; the compositor handles
// head roll on the submit side). SEH-guarded: a clean no-op on any fault.
bool ApplyHeadRotation(void* fsv, float yawRad, float pitchRad) noexcept
{
    __try
    {
        BYTE* base = reinterpret_cast<BYTE*>(fsv);
        float* VM = reinterpret_cast<float*>(base + kFsvViewMatrix);
        float* TVM = reinterpret_cast<float*>(base + kFsvTranslatedViewMatrix);
        const float* PM = reinterpret_cast<const float*>(base + kFsvProjectionMatrix);
        float* VPM = reinterpret_cast<float*>(base + kFsvViewProjectionMatrix);
        float* TVPM = reinterpret_cast<float*>(base + kFsvTranslatedViewProjMatrix);

        const float cy = std::cos(yawRad), sy = std::sin(yawRad);
        const float cp = std::cos(pitchRad), sp = std::sin(pitchRad);
        // [WORLDYAW2 2026-08-12] During a VR cine the yaw axis is WORLD up as seen by this camera,
        // not the camera's local +Y. The submit tag describes the head turn in world axes; a level
        // camera makes the two identical, a BANKED/PITCHED director camera (the opening flyby) makes
        // them diverge by the tilt angle, and the compositor then fights the pixels in proportion to
        // shot tilt - shake that survived every delivery fix while [CINEJIT] read clean. The image of
        // world +Z (UE up) under this row-vector world->view map is the third row of the 3x3, which
        // is CONTINUOUS in the camera orientation. The first WORLDYAW (reverted cd646b1) failed on a
        // hemisphere clamp that sign-flipped this axis when a tilted camera crossed level, inverting
        // the applied head-look mid-shot - so there is deliberately NO clamp here; the raw row is the
        // axis, whatever hemisphere it lies in. For u=(0,1,0) the Rodrigues form below reduces
        // element-by-element to the old local-up Ry, so level shots are bit-identical.
        // CINE ONLY: gameplay's smoothness was dialled in-headset against the local-up rotation and
        // the delayed pose tag (poseTagDelayFrames), so its calibration already absorbs any axis
        // residual - changing gameplay's axis would re-open a solved lane. Degenerate row (scaled or
        // garbage matrix) falls back to local up = the shipped behaviour.
        // [WORLDYAW2 A/B 2026-08-12] The world axis is now OPT-IN (cineWorldYaw, default off). It
        // shipped hardcoded and the broad scene-dependent shake regressed the same day: an animated
        // director camera moves this derived axis every frame, so the applied head-look sweeps
        // against the tag in proportion to camera motion. The telemetry below runs in every cine
        // frame regardless of the toggle, so a shaky log second can be correlated with tilt and
        // camera motion either way.
        float ux = 0.0f, uy = 1.0f, uz = 0.0f;
        if (SfrCineActive())
        {
            const float wx = VM[8], wy = VM[9], wz = VM[10];
            const float wn = std::sqrt(wx * wx + wy * wy + wz * wz);
            if (wn > 0.5f && wn < 2.0f)
            {
                const float nx = wx / wn, ny = wy / wn, nz = wz / wn;
                float cd = ny;   // dot(world-up-in-view, local +Y) = tilt cosine
                if (cd > 1.0f) cd = 1.0f;
                if (cd < -1.0f) cd = -1.0f;
                g_cineTiltCdeg.store(static_cast<int>(std::acos(cd) * 5729.578f), std::memory_order_relaxed);
                static float s_pr[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
                float mx = 0.0f;
                for (int r = 0; r < 3; ++r)
                {
                    const float* row = VM + 4 * r;
                    const float rn = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
                    if (rn < 0.5f || rn > 2.0f) { mx = 0.0f; break; }
                    float d = (row[0] * s_pr[3 * r] + row[1] * s_pr[3 * r + 1] + row[2] * s_pr[3 * r + 2]) / rn;
                    if (d > 1.0f) d = 1.0f;
                    if (d < -1.0f) d = -1.0f;
                    const float ang = std::acos(d);
                    if (ang > mx) mx = ang;
                    s_pr[3 * r] = row[0] / rn; s_pr[3 * r + 1] = row[1] / rn; s_pr[3 * r + 2] = row[2] / rn;
                }
                AtomicMaxInt(g_cineCamStepCdegMax, static_cast<int>(mx * 5729.578f));
                if (MELEVR::Config::Get().cineWorldYaw) { ux = nx; uy = ny; uz = nz; }
            }
        }
        const float oc = 1.0f - cy;
        const float Ry[16] = {
            cy + oc * ux * ux,      oc * ux * uy + sy * uz, oc * ux * uz - sy * uy, 0.0f,
            oc * uy * ux - sy * uz, cy + oc * uy * uy,      oc * uy * uz + sy * ux, 0.0f,
            oc * uz * ux + sy * uy, oc * uz * uy - sy * ux, cy + oc * uz * uz,      0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};
        const float Rx[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, cp, sp, 0.0f, 0.0f, -sp, cp, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        float dV[16];
        // Row-vector order: yaw first, then pitch around the yawed/local right axis.
        // Rx*Ry feels correct facing forward but inverts pitch when the user turns around.
        Mul4x4(dV, Ry, Rx);

        float vm2[16], tvm2[16];
        Mul4x4(vm2, VM, dV);
        Mul4x4(tvm2, TVM, dV);
        for (int i = 0; i < 16; ++i) { VM[i] = vm2[i]; TVM[i] = tvm2[i]; }
        Mul4x4(VPM, VM, PM);
        Mul4x4(TVPM, TVM, PM);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Read xScale/yScale (M[0][0]/M[1][1]) from the projection matrix and stash the rendered FOV half-angles.
// Perspective only (M[3][3]~=0) so ortho shadow sub-views don't overwrite the main scene FOV. SEH-guarded.
void StashRenderFov(void* fsv) noexcept
{
    __try
    {
        const float* P = reinterpret_cast<const float*>(reinterpret_cast<const BYTE*>(fsv) + kFsvProjectionMatrix);
        if (std::fabs(P[15]) > 0.01f) return;  // orthographic (shadow map) - skip
        const float xScale = P[0];   // 1 / tan(halfHoriz)
        const float yScale = P[5];   // 1 / tan(halfVert)
        if (xScale > 0.01f && yScale > 0.01f)
        {
            g_fovHalfHoriz.store(std::atan(1.0f / xScale), std::memory_order_relaxed);
            g_fovHalfVert.store(std::atan(1.0f / yScale), std::memory_order_relaxed);
            g_fovValid.store(true, std::memory_order_release);
        }
        // Base camera position (pre-offset) for the locomotion motion term.
        const float* vo = reinterpret_cast<const float*>(reinterpret_cast<const BYTE*>(fsv) + kFsvViewOrigin);
        g_baseVOx.store(vo[0], std::memory_order_relaxed);
        g_baseVOy.store(vo[1], std::memory_order_relaxed);
        g_baseVOz.store(vo[2], std::memory_order_relaxed);
        g_baseVOValid.store(true, std::memory_order_release);

        const float* VM = reinterpret_cast<const float*>(reinterpret_cast<const BYTE*>(fsv) + kFsvViewMatrix);
        g_lastRightX.store(VM[0], std::memory_order_relaxed);
        g_lastRightY.store(VM[4], std::memory_order_relaxed);
        g_lastRightZ.store(VM[8], std::memory_order_relaxed);
        g_lastRightValid.store(true, std::memory_order_release);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

bool IsPerspectiveSEH(void* fsv) noexcept
{
    __try
    {
        const float* P = reinterpret_cast<const float*>(reinterpret_cast<const BYTE*>(fsv) + kFsvProjectionMatrix);
        return std::fabs(P[15]) < 0.01f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ApplyTargetFovOverrideRawSEH(void* fsv, float halfH, float halfV, float* outP0, float* outP5) noexcept
{
    __try
    {
        auto* base = reinterpret_cast<BYTE*>(fsv);
        auto* projection = reinterpret_cast<float*>(base + kFsvProjectionMatrix);
        const auto* view = reinterpret_cast<const float*>(base + kFsvViewMatrix);
        const auto* translatedView = reinterpret_cast<const float*>(base + kFsvTranslatedViewMatrix);
        auto* viewProjection = reinterpret_cast<float*>(base + kFsvViewProjectionMatrix);
        auto* translatedViewProjection = reinterpret_cast<float*>(base + kFsvTranslatedViewProjMatrix);

        projection[0] = 1.0f / std::tan(halfH);
        projection[5] = 1.0f / std::tan(halfV);
        if (outP0 != nullptr) *outP0 = projection[0];
        if (outP5 != nullptr) *outP5 = projection[5];
        Mul4x4(viewProjection, view, projection);
        Mul4x4(translatedViewProjection, translatedView, projection);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// (The [VRCINE] fixed cinema-window FOV mode lived here; removed 2026-07-18 with the VR-cine layer.
// Cine is flat now - during a cinematic the FOV override is simply off (native projection).)
bool ApplyTargetFovOverride(void* fsv) noexcept
{
    if (fsv == nullptr || !g_targetFovEnabled.load(std::memory_order_acquire) ||
        !Readable(fsv, kFsvViewProjectionMatrix + sizeof(float) * 16) || !IsPerspectiveSEH(fsv))
    {
        return false;
    }

    float halfH = g_targetFovHalfHoriz.load(std::memory_order_relaxed);
    float halfV = g_targetFovHalfVert.load(std::memory_order_relaxed);
    if (!std::isfinite(halfH) || !std::isfinite(halfV) || halfH < 0.10f || halfV < 0.10f || halfH > 2.20f || halfV > 2.20f)
    {
        return false;
    }

    float p0 = 0.0f;
    float p5 = 0.0f;
    if (!ApplyTargetFovOverrideRawSEH(fsv, halfH, halfV, &p0, &p5)) return false;

    const int n = g_targetFovLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 8 || (n % 300) == 0)
    {
        LogLine("[RENDER_FOV_MATCH] applied halfH=" + std::to_string(halfH) +
                " halfV=" + std::to_string(halfV) +
                " p0=" + std::to_string(p0) +
                " p5=" + std::to_string(p5));
    }
    return true;
}

bool ApplyCloneEyeOffset(void* fsv, float eyeOffsetUU) noexcept
{
    if (fsv == nullptr || !Readable(fsv, kFsvViewProjectionMatrix + sizeof(float) * 16) || !IsPerspectiveSEH(fsv)) return false;

    __try
    {
        auto* base = reinterpret_cast<BYTE*>(fsv);
        auto* view = reinterpret_cast<float*>(base + kFsvViewMatrix);
        auto* translatedView = reinterpret_cast<float*>(base + kFsvTranslatedViewMatrix);
        const auto* projection = reinterpret_cast<const float*>(base + kFsvProjectionMatrix);
        auto* viewProjection = reinterpret_cast<float*>(base + kFsvViewProjectionMatrix);
        auto* translatedViewProjection = reinterpret_cast<float*>(base + kFsvTranslatedViewProjMatrix);

        const float eyeShift[16] = {
            1.0f,        0.0f, 0.0f, 0.0f,
            0.0f,        1.0f, 0.0f, 0.0f,
            0.0f,        0.0f, 1.0f, 0.0f,
            eyeOffsetUU, 0.0f, 0.0f, 1.0f,
        };

        float newView[16] = {};
        float newTranslatedView[16] = {};
        Mul4x4(newView, view, eyeShift);
        Mul4x4(newTranslatedView, translatedView, eyeShift);

        for (int i = 0; i < 16; ++i)
        {
            view[i] = newView[i];
            translatedView[i] = newTranslatedView[i];
        }

        Mul4x4(viewProjection, view, projection);
        Mul4x4(translatedViewProjection, translatedView, projection);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Shift the RENDERED camera by a view-relative offset (right/up/forward UE units): move ViewOrigin and the
// per-vertex PreViewTranslation so the game renders from the moved viewpoint (real parallax). Read the view
// basis from the game's ViewMatrix (BEFORE the head-look rotation rewrites it, so the offset is body-stable,
// not swimming with head-look). PreViewTranslation feeds the GPU directly, so no product recompute is needed.
// SEH-guarded; clean no-op on fault or all-zero offset.
bool ApplyViewOffsetSEH(void* fsv) noexcept
{
    // [PAIRLATCH] same Present-thread race as the head-look angles: these offsets (lean/screen
    // distance) are republished per present, and a Present can land between Stereo 2's two eye
    // passes -> positional interocular mismatch while the head moves. Main pass latches, replay
    // pass reuses. Game-thread-only statics; single-pass modes re-latch every frame (no-op).
    static float s_pairR = 0.0f, s_pairU = 0.0f, s_pairF = 0.0f;
    if (!t_fvcReplay)
    {
        s_pairR = g_viewOffRight.load(std::memory_order_relaxed);
        s_pairU = g_viewOffUp.load(std::memory_order_relaxed);
        s_pairF = g_viewOffFwd.load(std::memory_order_relaxed);
    }
    const float r = s_pairR;
    const float u = s_pairU;
    const float f = s_pairF;
    if (r == 0.0f && u == 0.0f && f == 0.0f) return true;
    __try
    {
        BYTE* base = reinterpret_cast<BYTE*>(fsv);
        const float* VM = reinterpret_cast<const float*>(base + kFsvViewMatrix);  // world->view, row-major
        // View axes expressed in world = columns of VM's rotation: right=col0, up=col1, forward=col2.
        const float wx = r * VM[0] + u * VM[1] + f * VM[2];
        const float wy = r * VM[4] + u * VM[5] + f * VM[6];
        const float wz = r * VM[8] + u * VM[9] + f * VM[10];
        float* pvt = reinterpret_cast<float*>(base + kFsvPreViewTranslation);
        float* vo = reinterpret_cast<float*>(base + kFsvViewOrigin);
        pvt[0] -= wx; pvt[1] -= wy; pvt[2] -= wz;   // move the camera by (+w): vertices pre-translate by -w
        vo[0] += wx; vo[1] += wy; vo[2] += wz;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ApplyViewRelativeOffsetRawSEH(void* fsv, float r, float u, float f,
                                   float* outWx, float* outWy, float* outWz,
                                   float* outVo, float* outPvt) noexcept
{
    __try
    {
        BYTE* base = reinterpret_cast<BYTE*>(fsv);
        const float* VM = reinterpret_cast<const float*>(base + kFsvViewMatrix);
        const float wx = r * VM[0] + u * VM[1] + f * VM[2];
        const float wy = r * VM[4] + u * VM[5] + f * VM[6];
        const float wz = r * VM[8] + u * VM[9] + f * VM[10];
        float* pvt = reinterpret_cast<float*>(base + kFsvPreViewTranslation);
        float* vo = reinterpret_cast<float*>(base + kFsvViewOrigin);
        pvt[0] -= wx; pvt[1] -= wy; pvt[2] -= wz;
        vo[0] += wx; vo[1] += wy; vo[2] += wz;

        if (outWx) *outWx = wx;
        if (outWy) *outWy = wy;
        if (outWz) *outWz = wz;
        if (outVo)
        {
            outVo[0] = vo[0]; outVo[1] = vo[1]; outVo[2] = vo[2];
        }
        if (outPvt)
        {
            outPvt[0] = pvt[0]; outPvt[1] = pvt[1]; outPvt[2] = pvt[2];
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// (CINEFP camera relocation + CINEDOLLY zoom dolly lived here; removed 2026-07-18 with the VR-cine
// layer. The KEEP CULL == RENDER recipe they proved - VM row3 from the new origin + VPM/TVPM
// recompute - remains in use by ApplyHeadRotation and the view-offset path.)

bool SnapshotFsvShiftFieldsSEH(void* fsv, FsvShiftSnapshot* out) noexcept
{
    if (fsv == nullptr || out == nullptr) return false;
    __try
    {
        BYTE* base = reinterpret_cast<BYTE*>(fsv);
        const float* pvt = reinterpret_cast<const float*>(base + kFsvPreViewTranslation);
        const float* vo = reinterpret_cast<const float*>(base + kFsvViewOrigin);
        out->vo[0] = vo[0]; out->vo[1] = vo[1]; out->vo[2] = vo[2];
        out->pvt[0] = pvt[0]; out->pvt[1] = pvt[1]; out->pvt[2] = pvt[2];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool RestoreFsvShiftFieldsSEH(void* fsv, const FsvShiftSnapshot& snap) noexcept
{
    if (fsv == nullptr) return false;
    __try
    {
        BYTE* base = reinterpret_cast<BYTE*>(fsv);
        float* pvt = reinterpret_cast<float*>(base + kFsvPreViewTranslation);
        float* vo = reinterpret_cast<float*>(base + kFsvViewOrigin);
        vo[0] = snap.vo[0]; vo[1] = snap.vo[1]; vo[2] = snap.vo[2];
        pvt[0] = snap.pvt[0]; pvt[1] = snap.pvt[1]; pvt[2] = snap.pvt[2];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// [REFLFIX] SEH-guarded FSceneView camera-position read for the mirror-camera rewrite publish.
bool ReadFsvViewOriginSEH(void* fsv, float out[3]) noexcept
{
    if (fsv == nullptr || out == nullptr) return false;
    __try
    {
        const float* vo = reinterpret_cast<const float*>(reinterpret_cast<const BYTE*>(fsv) + kFsvViewOrigin);
        out[0] = vo[0];
        out[1] = vo[1];
        out[2] = vo[2];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// [SFR] Per-pass eye offset, SYMMETRIC like regular Stereo: pass 0 renders from -halfEye,
// the replay pass from +halfEye, using
// the SAME tuned stereoHalfEyeUU value regular Stereo uses (published per frame by xr_session).
// Each CalcSceneView builds a fresh FSceneView, so the shifts never persist into game state.
// (History: was the fixed 120uu one-shot steering proof, then the asymmetric +3.2uu M0 offset.)
bool ApplyReplayProofOffsetSEH(void* fsv) noexcept
{
    if (!g_sfrModeEnabled.load(std::memory_order_acquire)) return true;
    // Mirror the FVC-side replay gates: gameplay + cine ([CLEANVRCINE]), and stop shifting once SFR
    // self-disarmed - otherwise pass 0 (the game's only render) would sit offset with no second pass
    // to pair it.
    const int sfrGameMode = g_sfrGameMode.load(std::memory_order_acquire);
    if (!SfrModeAllowsReplay(sfrGameMode)) return true;
    if (g_sfrExceptions.load(std::memory_order_acquire) >= 3) return true;

    const float halfEye = g_sfrHalfEyeUU.load(std::memory_order_relaxed);
    // [SWAPME2] negative halfEye = swap-eyes camera flip (ME2 parity); only zero/invalid is a no-op.
    if (halfEye == 0.0f || !std::isfinite(halfEye)) return true;
    const float shiftRightUU = t_fvcReplay ? halfEye : -halfEye;
    float wx = 0.0f, wy = 0.0f, wz = 0.0f;
    float vo[3] = {};
    float pvt[3] = {};
    const bool ok = ApplyViewRelativeOffsetRawSEH(fsv, shiftRightUU, 0.0f, 0.0f,
                                                  &wx, &wy, &wz, vo, pvt);
    if (!ok)
    {
        LogLine("[REPLAYSHIFT] failed: SEH caught fault while applying replay-only offset.");
        return false;
    }

    const int logIndex = g_replayShiftLogs.fetch_add(1, std::memory_order_acq_rel);
    if (logIndex < 16)
    {
        char line[384] = {};
        sprintf_s(line,
                  "[REPLAYSHIFT] applied %s-eye rightUU=%.2f fsv=0x%p worldDelta=(%.1f,%.1f,%.1f) VO=(%.1f,%.1f,%.1f) PVT=(%.1f,%.1f,%.1f)",
                  t_fvcReplay ? "right" : "left",
                  shiftRightUU,
                  fsv,
                  wx, wy, wz,
                  vo[0], vo[1], vo[2],
                  pvt[0], pvt[1], pvt[2]);
        LogLine(line);
    }
    return true;
}

// [SFRCONV] Convergence: shift THIS pass's projection horizontally (an off-axis frustum) so a chosen depth
// has zero disparity. The lateral eye offset alone (ApplyReplayProofOffsetSEH) puts zero-disparity at
// infinity, so distant isolated elements double until you're close; an opposite per-eye shift pulls the
// fusion plane in WITHOUT touching the IPD (world scale). proj[8] is UE3's horizontal off-center term.
// Run AFTER ApplyTargetFovOverride (which rebuilds the view-projection) so it isn't overwritten; rebuild
// the (translated) view-projection here. SEH-guarded: clean no-op on any fault. Sign matches the separation
// convention: pass0 (main, !replay) = left eye = +conv, pass1 (replay) = right eye = -conv (ME2 parity).
bool ApplyConvergenceSEH(void* fsv) noexcept
{
    if (!g_sfrModeEnabled.load(std::memory_order_acquire)) return true;
    const int sfrGameMode = g_sfrGameMode.load(std::memory_order_acquire);
    if (!SfrModeAllowsReplay(sfrGameMode)) return true;
    if (g_sfrExceptions.load(std::memory_order_acquire) >= 3) return true;

    const float conv = g_sfrConvergence.load(std::memory_order_relaxed);
    if (conv == 0.0f) return true;
    const float signedShift = t_fvcReplay ? -conv : +conv;
    __try
    {
        // [SFRCONV-UI 2026-07-20] GPU transform ONLY (TranslatedViewProjection, what the shaders read).
        // The projection matrix and ViewProjectionMatrix are LEFT UNTOUCHED on purpose: ME1 draws its
        // world-anchored HUD (crosshair, interact markers, radar) at the end of EACH pass, placing it
        // on the CPU through those matrices. Shifting them (the old proj[8]+rebuild, ME2's literal
        // code) made pass 0 place UI at +conv and pass 1 at -conv -> the left eye showed BOTH copies
        // 2*conv apart (baked snapshot copy + mirrored copy) = "UI doubles with convergence". ME2
        // never sees this only because ITS game draws the HUD once per present. Math note: a constant
        // NDC x-shift of s at every depth is exactly col0 += s*col3 (proj col3 is unit-z, so TVP col3
        // == (view*proj) col2 == what proj[8] fed) - the rendered scene convergence is bit-identical
        // to ME2's; only CPU placement/culling stay parallel, so both UI batches land in the SAME
        // place and coincide. MUST run AFTER ApplyHeadRotation (it recomposes the GPU matrix from the
        // un-converged projection and would wipe this).
        BYTE* base = reinterpret_cast<BYTE*>(fsv);
        const float* projection = reinterpret_cast<const float*>(base + kFsvProjectionMatrix);
        if (std::fabs(projection[15]) > 0.01f) return true;   // orthographic (shadow sub-view) -> skip
        float* tvp = reinterpret_cast<float*>(base + kFsvTranslatedViewProjMatrix);
        for (int i = 0; i < 4; ++i) tvp[i * 4 + 0] += signedShift * tvp[i * 4 + 3];   // col0 += s*col3
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void StampRenderedEye(int eye) noexcept
{
    if (eye < 0 || eye > 1) return;
    // [AERSHAKE] publish into the FIFO ring: eye first (relaxed), then seq with RELEASE so a reader
    // that sees the seq sees the eye. Single writer thread, so load+1/store is safe and keeps this
    // seq equal to the one the eye-parity pick below computed for this same build.
    const uint64_t seq = g_renderStampSeq.load(std::memory_order_relaxed) + 1;
    AerRingSlot& slot = g_aerRing[seq % kAerRingN];
    slot.eye.store(eye, std::memory_order_relaxed);
    slot.seq.store(seq, std::memory_order_release);
    g_renderStampEye.store(eye, std::memory_order_relaxed);   // legacy single-slot stamp kept live
    g_renderStampAer.store(true, std::memory_order_relaxed);
    g_renderStampSeq.store(seq, std::memory_order_release);
}

bool ReadRawProjMatrixSEH(void* fsv, float* m00, float* m11, float* p15) noexcept
{
    __try
    {
        const float* RP = reinterpret_cast<const float*>(reinterpret_cast<const BYTE*>(fsv) + kFsvProjectionMatrix);
        *m00 = RP[0];
        *m11 = RP[5];
        *p15 = RP[15];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void StoreRawProjectionInfo(float m00, float m11) noexcept
{
    if (!(m00 > 0.001f && m11 > 0.001f) || !std::isfinite(m00) || !std::isfinite(m11)) return;
    g_rawProjAspect.store(m11 / m00, std::memory_order_relaxed);
    g_rawProjHalfHoriz.store(std::atan(1.0f / m00), std::memory_order_relaxed);
    g_rawProjHalfVert.store(std::atan(1.0f / m11), std::memory_order_relaxed);
    g_rawProjValid.store(true, std::memory_order_release);
}

// ===== ME2-EXACT stereo eye math (ported verbatim from ME2 calcview_hook.cpp, 2026-07-02) =====
// ME1's SBS split builds the left & right views with two independent CalcSceneView calls. During a camera
// blend those calls can return slightly DIFFERENT base cameras, so the eyes diverge by more than the IPD ->
// unfusable double vision that gets WORSE as you raise separation (exact field report). ME2 kills this by
// deriving BOTH eyes from the LEFT camera, then applying a symmetric real-camera-move offset. Copies view
// fields only (projection is per-half; view-proj is rebuilt afterward by PostProcessPerspectiveFsv).
void SyncEyeCameraFields(void* dst, void* src) noexcept
{
    if (dst == nullptr || src == nullptr) return;
    if (!Readable(dst, kFsvViewOrigin + 16) || !Readable(src, kFsvViewOrigin + 16)) return;
    __try
    {
        BYTE* d = reinterpret_cast<BYTE*>(dst);
        BYTE* s = reinterpret_cast<BYTE*>(src);
        volatile float* dVM = reinterpret_cast<volatile float*>(d + kFsvViewMatrix);
        const volatile float* sVM = reinterpret_cast<const volatile float*>(s + kFsvViewMatrix);
        for (int i = 0; i < 16; ++i) dVM[i] = sVM[i];
        volatile float* dTVM = reinterpret_cast<volatile float*>(d + kFsvTranslatedViewMatrix);
        const volatile float* sTVM = reinterpret_cast<const volatile float*>(s + kFsvTranslatedViewMatrix);
        for (int i = 0; i < 16; ++i) dTVM[i] = sTVM[i];
        volatile float* dVO = reinterpret_cast<volatile float*>(d + kFsvViewOrigin);
        const volatile float* sVO = reinterpret_cast<const volatile float*>(s + kFsvViewOrigin);
        for (int i = 0; i < 3; ++i) dVO[i] = sVO[i];
        volatile float* dPVT = reinterpret_cast<volatile float*>(d + kFsvPreViewTranslation);
        const volatile float* sPVT = reinterpret_cast<const volatile float*>(s + kFsvPreViewTranslation);
        for (int i = 0; i < 3; ++i) dPVT[i] = sPVT[i];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Move this view's camera by signedUU along the render-camera RIGHT axis (ViewMatrix col0), editing
// PreViewTranslation + ViewOrigin - a REAL camera move => real binocular parallax (ME2's B2b / AER method,
// NOT a view-matrix multiply). Call BEFORE head-look so the separation uses the BODY right (doesn't swim).
bool ApplyEyeOffsetAER(void* fsv, float signedUU) noexcept
{
    if (fsv == nullptr || !Readable(fsv, kFsvViewOrigin + 16)) return false;
    __try
    {
        BYTE* base = reinterpret_cast<BYTE*>(fsv);
        const volatile float* VM = reinterpret_cast<const volatile float*>(base + kFsvViewMatrix);
        const float rx = VM[0], ry = VM[4], rz = VM[8];   // world-space camera right = column 0
        const float wx = signedUU * rx, wy = signedUU * ry, wz = signedUU * rz;
        volatile float* pvt = reinterpret_cast<volatile float*>(base + kFsvPreViewTranslation);
        volatile float* vo = reinterpret_cast<volatile float*>(base + kFsvViewOrigin);
        pvt[0] -= wx; pvt[1] -= wy; pvt[2] -= wz;
        vo[0] += wx;  vo[1] += wy;  vo[2] += wz;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- [INVMAT] FSceneView inverse-matrix mapper (2026-07-12, head-locked dark-panel hunt) -----------
// Theory under test: every edit made after CalcSceneView (FOV widen, view offset, head rotation)
// rebuilds only the FORWARD matrix products; any cached INVERSE / screen-to-world matrix inside
// FSceneView keeps describing the PRE-edit camera. Deferred passes that reconstruct world position from
// screen+depth (height fog, light shafts) would then disagree with the rendered geometry by exactly the
// injected pitch - a hard, screen-locked horizontal fog edge that appears only while head pitch is
// being injected. Yaw error is invisible to height fog (world Z unchanged), pitch error is not - which
// matches the reported look-up/look-down correlation.
// Scan (one-shot, on a pristine view BEFORE any edits): find 64-byte blocks that are numerically
// the inverse of a known matrix. Recheck (one-shot, after a real head-pitch rotation): are they still
// inverses? SetInverseFix(true) additionally recomputes the validated slots after each edit, every view.
constexpr int kInvMaxSlots = 24;
struct InvSlot { std::uint32_t offset; int target; float scanErr; };
InvSlot g_invSlots[kInvMaxSlots] = {};
int g_invSlotCount = 0;
std::atomic_bool g_invScanDone{false};
std::atomic_bool g_invRecheckDone{false};
std::atomic_bool g_invFixEnabled{false};
std::atomic<std::uint64_t> g_invScanViews{0};
std::atomic<std::uint64_t> g_invFixLogs{0};

struct InvTarget { const char* name; std::uintptr_t off; };
constexpr InvTarget kInvTargets[] = {
    {"ViewMatrix", kFsvViewMatrix},
    {"ProjectionMatrix", kFsvProjectionMatrix},
    {"TranslatedViewMatrix", kFsvTranslatedViewMatrix},
    {"TranslatedViewProjMatrix", kFsvTranslatedViewProjMatrix},
    {"ViewProjectionMatrix", kFsvViewProjectionMatrix},
};
constexpr int kInvTargetCount = static_cast<int>(sizeof(kInvTargets) / sizeof(kInvTargets[0]));
constexpr std::uintptr_t kInvScanBytes = 0x800;   // same reach the FSceneView::State scan already uses

// Max deviation of P from identity. The bottom (translation) row is normalized by the camera-translation
// magnitude: float32 residue of inv(M)*M on a 1e5-uu translation is legitimately ~1e-2, not a mismatch.
float IdentityErr(const float* P, float transMag) noexcept
{
    float tDiv = transMag * 5e-5f;
    if (tDiv < 1.0f) tDiv = 1.0f;
    float err = 0.0f;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
        {
            float d = P[i * 4 + j] - ((i == j) ? 1.0f : 0.0f);
            if (d < 0.0f) d = -d;
            if (i == 3 && j < 3) d /= tDiv;
            if (d > err) err = d;
        }
    return err;
}

// Best-of-both-orders inverse check: min(err(B*M), err(M*B)) vs identity.
float InvPairErrSEH(const float* B, const float* M, float transMag) noexcept
{
    __try
    {
        float P[16];
        Mul4x4(P, B, M);
        float err = IdentityErr(P, transMag);
        Mul4x4(P, M, B);
        const float err2 = IdentityErr(P, transMag);
        return (err2 < err) ? err2 : err;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 1e9f; }
}

// One-shot scan of a pristine FSceneView. Returns match count, or <0 to retry on a later view:
// -1 unreadable, -2 camera too close to the world origin (identity-ish views match everything).
int InvMatScanSEH(void* fsv, InvSlot* outSlots, int maxSlots, float* outTransMag) noexcept
{
    __try
    {
        const BYTE* base = reinterpret_cast<const BYTE*>(fsv);
        if (!Readable(base, kInvScanBytes)) return -1;
        const float* vm = reinterpret_cast<const float*>(base + kFsvViewMatrix);
        float transMag = 0.0f;
        for (int i = 12; i < 15; ++i)
        {
            const float a = (vm[i] < 0.0f) ? -vm[i] : vm[i];
            if (a > transMag) transMag = a;
        }
        if (outTransMag != nullptr) *outTransMag = transMag;
        if (transMag < 50.0f) return -2;

        int n = 0;
        for (std::uint32_t off = 0; off + 64 <= kInvScanBytes && n < maxSlots; off += 0x10)
        {
            const float* B = reinterpret_cast<const float*>(base + off);
            for (int t = 0; t < kInvTargetCount; ++t)
            {
                if (off == kInvTargets[t].off) continue;
                const float* M = reinterpret_cast<const float*>(base + kInvTargets[t].off);
                const float err = InvPairErrSEH(B, M, transMag);
                if (err < 0.05f)
                {
                    outSlots[n].offset = off;
                    outSlots[n].target = t;
                    outSlots[n].scanErr = err;
                    ++n;
                    break;
                }
            }
        }
        return n;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// After the edits: how far is each validated slot from being the inverse of its (now edited) target?
bool InvMatErrsSEH(void* fsv, const InvSlot* slots, int count, float* outErrs) noexcept
{
    __try
    {
        const BYTE* base = reinterpret_cast<const BYTE*>(fsv);
        if (!Readable(base, kInvScanBytes)) return false;
        const float* vm = reinterpret_cast<const float*>(base + kFsvViewMatrix);
        float transMag = 0.0f;
        for (int i = 12; i < 15; ++i)
        {
            const float a = (vm[i] < 0.0f) ? -vm[i] : vm[i];
            if (a > transMag) transMag = a;
        }
        for (int i = 0; i < count; ++i)
        {
            const float* B = reinterpret_cast<const float*>(base + slots[i].offset);
            const float* M = reinterpret_cast<const float*>(base + kInvTargets[slots[i].target].off);
            outErrs[i] = InvPairErrSEH(B, M, transMag);
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// [INVMAT SAFETY, 2026-07-13] Canonical WRITE whitelist. The original "self-validating scan can't corrupt"
// claim was WRONG: the scan is per-boot non-deterministic - on a degenerate frame (load transitions) it
// false-positives extra offsets (log-proven: 5/5 runs with 0x720@err0.04, and a 12/12 run that validated
// OVERLAPPING 16-byte-apart blocks at 0x4C0-0x660 - geometrically impossible for real distinct caches -
// then CRASHED the game in the same millisecond the fix first wrote them). Writing 64 bytes over
// misidentified FSceneView fields every frame = the source of the 2026-07-12 evening "random crash" family
// (ntdll heap / nvwgf2umx / VD-runtime / game-code null read - four signatures, one corruption). The fix
// keeps the runtime VALIDATION (a canonical slot is still only written if the scan confirmed it at that
// exact offset+target, so a future patch moving the struct degrades to no-op, never to corruption) but
// only these three slots - the ones from the original clean-room proof - may ever be WRITTEN. Anything
// else the scan matches is logged for study and never touched.
bool IsCanonicalInvSlot(std::uint32_t offset, int target) noexcept
{
    const std::uintptr_t targetOff = kInvTargets[target].off;
    return (offset == 0x210 && targetOff == kFsvTranslatedViewProjMatrix) ||
           (offset == 0x2A0 && targetOff == kFsvProjectionMatrix) ||
           (offset == 0x2E0 && targetOff == kFsvViewProjectionMatrix);
}

// The gated fix: rewrite each validated slot as inverse(current target). Writes are hard-bounded to the
// canonical whitelist above; scan-validated extras are diagnostic-only. Returns slots refreshed.
int RefreshInverseSlotsSEH(void* fsv, const InvSlot* slots, int count) noexcept
{
    __try
    {
        BYTE* base = reinterpret_cast<BYTE*>(fsv);
        if (!Readable(base, kInvScanBytes)) return 0;
        int fixedCount = 0;
        for (int i = 0; i < count; ++i)
        {
            if (!IsCanonicalInvSlot(slots[i].offset, slots[i].target)) continue;   // log-only candidate
            const float* M = reinterpret_cast<const float*>(base + kInvTargets[slots[i].target].off);
            float invM[16];
            if (!Inverse4x4(M, invM)) continue;
            float* B = reinterpret_cast<float*>(base + slots[i].offset);
            for (int k = 0; k < 16; ++k) B[k] = invM[k];
            ++fixedCount;
        }
        return fixedCount;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

void PostProcessPerspectiveFsv(void* fsv, bool aerPrimaryView = false) noexcept
{
    if (fsv == nullptr || !IsPerspectiveSEH(fsv)) return;

    const bool replayPhase = t_fvcReplay;
    const uint64_t ordinalInPresent = g_calcCallsThisPresent.fetch_add(1, std::memory_order_relaxed) + 1;
    if (replayPhase)
    {
        g_replayCalcCallsThisPresent.fetch_add(1, std::memory_order_relaxed);
        g_replayCalcTotal.fetch_add(1, std::memory_order_release);
    }
    if (t_sceneBDepth > 0) g_sceneBCalcThisPresent.fetch_add(1, std::memory_order_relaxed);
    if (t_sceneCDepth > 0) g_sceneCCalcThisPresent.fetch_add(1, std::memory_order_relaxed);
    g_lastCalcThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (CalcSequenceProbeEnabled())
    {
        LogCalcSceneStackOnce();
        LogCalcSequence(fsv, ordinalInPresent, replayPhase);
    }
    g_lastPerspectiveFsv.store(fsv, std::memory_order_release);
    g_lastPerspectiveFsvPresent.store(g_presentSeq.load(std::memory_order_acquire), std::memory_order_release);

    // [INVMAT] one-shot pristine-view scan. Must run BEFORE any of the edits below - the view is exactly
    // as CalcSceneView built it, so every cached inverse still matches and the scan can identify them.
    if (!g_invScanDone.load(std::memory_order_acquire))
    {
        const std::uint64_t views = g_invScanViews.fetch_add(1, std::memory_order_relaxed) + 1;
        if (views >= 300)   // let boot/menu views pass; a real scene view is wanted
        {
            float transMag = 0.0f;
            InvSlot slots[kInvMaxSlots];
            const int n = InvMatScanSEH(fsv, slots, kInvMaxSlots, &transMag);
            if (n >= 0)
            {
                for (int i = 0; i < n; ++i) g_invSlots[i] = slots[i];
                g_invSlotCount = n;
                g_invScanDone.store(true, std::memory_order_release);
                for (int i = 0; i < n; ++i)
                    LogLine(std::string("[INVMAT] scan: slot off=") + Hex64(slots[i].offset) +
                            " = inverse(" + kInvTargets[slots[i].target].name + ")" +
                            " err=" + std::to_string(slots[i].scanErr) +
                            (IsCanonicalInvSlot(slots[i].offset, slots[i].target)
                                 ? " [write-enabled]" : " [LOG-ONLY: non-canonical, never written]"));
                LogLine("[INVMAT] scan complete on view#" + std::to_string(views) +
                        " matches=" + std::to_string(n) +
                        " viewTransMag=" + std::to_string(transMag) +
                        (n == 0 ? " -> NO cached inverse matrices in FSceneView(0x0-0x800); stale-inverse theory dead at this layer"
                                : ""));
            }
            // n<0: unreadable or camera near origin - retry on a later view
        }
    }
    // [CINEFOCUS] per-eye FSceneViewState during a Stereo 2 convo/cutscene (the eye-focus-mismatch
    // strain during head movement). The SFR replay's two passes render DIFFERENT eye cameras through
    // the game's ONE shared view state; gameplay tolerates that (motion blur/DoF off = little
    // temporal work), but cinematics run the heavy temporal/post stack, so each eye keeps inheriting
    // the OTHER eye's temporal history -> the resolve smears on any head/camera motion and only
    // converges when perfectly still. Same defect class ME2's per-eye-state fix killed. Reuses the
    // Stereo-1 allocator + slots; pass0=left, pass1(replay)=right, CINE ONLY - outside cine the
    // game's own state is untouched and gameplay stays bit-identical. Knob-free ([CLEANVRCINE]).
    if (g_sfrModeEnabled.load(std::memory_order_acquire) && SfrCineActive())
    {
        void* eyeState = replayPhase ? EnsureAllocatedViewStateSlot(g_stereoViewStateRight, "right")
                                     : EnsureAllocatedViewStateSlot(g_stereoViewStateLeft, "left");
        SetViewState(fsv, eyeState);   // null-safe: keeps the game's state if allocation ever fails
        static std::atomic_bool s_cineFocusLogged{false};
        if (!s_cineFocusLogged.exchange(true))
            LogLine("[CINEFOCUS] per-eye view states active for VR cine (pass0=left, pass1=right)");
    }
    ApplyReplayProofOffsetSEH(fsv);
    ApplyViewOffsetSEH(fsv);   // base camera offset/lean, before head-look
    int aerRenderedEye = -1;
    if (aerPrimaryView)
    {
        // ME2/ME3 parity: apply and stamp AER only for the primary ULocalPlayer view.
        // [AERSHAKE] eye from THIS build's own upcoming seq parity - the present-armed
        // g_aerPrimaryRenderEye was a cross-thread feedback loop whose phase flapped with
        // pipeline depth; that flap was the visible full-rate shake in ME2/ME3.
        aerRenderedEye = static_cast<int>((g_renderStampSeq.load(std::memory_order_relaxed) + 1) & 1ull);
        void* eyeState = (aerRenderedEye == 0)
            ? EnsureAllocatedViewStateSlot(g_aerViewStateLeft, "aer-left")
            : EnsureAllocatedViewStateSlot(g_aerViewStateRight, "aer-right");
        SetViewState(fsv, eyeState);
        static std::atomic_bool s_aerViewStateLogged{false};
        if (g_aerViewStateLeft != nullptr && g_aerViewStateRight != nullptr &&
            !s_aerViewStateLogged.exchange(true))
        {
            LogLine("[AERVIEWSTATE] dedicated per-eye temporal histories active");
        }
        float he = g_aerPrimaryHalfEyeUU.load(std::memory_order_relaxed);
        if (!std::isfinite(he) || he < 0.0f) he = 0.0f;
        if (he > 32.0f) he = 32.0f;
        const float base = (aerRenderedEye == 0) ? -he : +he;
        const float signedUU = g_aerPrimarySwapEyes.load(std::memory_order_relaxed) ? -base : base;
        ApplyEyeOffsetAER(fsv, signedUU);
    }
    // [REFLFIX] publish the live head-look angles + tracked camera position for the mirror-camera
    // rewrite (planar reflections render from mirror(GAME camera) and never see the render-side
    // head transform - the "reflection slides under head tracking" bug). Main pass only.
    // [REFLLOG] also publish while the log-only evidence pass is armed, in ANY mode - the mono
    // head-turn experiment needs the classifier inputs without Stereo 2's second pass in the data.
    if (!t_fvcReplay && (g_sfrModeEnabled.load(std::memory_order_acquire) ||
                         MELEVR::D3DCapture::ReflLogArmed()))
    {
        float vo[3] = {};
        if (ReadFsvViewOriginSEH(fsv, vo))
        {
            constexpr float kUUToRadPub = 6.2831853f / 65536.0f;
            const bool lookOn = g_headLookOn.load(std::memory_order_acquire);
            const float yawRad = lookOn ? static_cast<float>(g_headYawUU.load(std::memory_order_relaxed)) * kUUToRadPub : 0.0f;
            const float pitchRad = lookOn ? static_cast<float>(g_headPitchUU.load(std::memory_order_relaxed)) * kUUToRadPub : 0.0f;
            // Field at 0x320 stores the camera TRANSLATION INVERSE (-position): the reflfix-12
            // dz-reject samples showed cam == -rec exactly. Negate to publish true world position.
            MELEVR::D3DCapture::SetReflFixInputs(yawRad, pitchRad, -vo[0], -vo[1], -vo[2]);
        }
    }
    // Feed the raw projection to the FOV-match consumers. The [RAWPROJ] per-frame DIAGNOSTIC logging (cinematic
    // squash hunt, long since solved) was removed 2026-07-07 - this keeps only the StoreRawProjectionInfo side
    // effect that the render/submit FOV matching actually needs.
    {
        float m00 = 0.0f, m11 = 0.0f, p15 = 0.0f;
        if (ReadRawProjMatrixSEH(fsv, &m00, &m11, &p15) && m00 > 0.001f && m11 > 0.001f)
        {
            StoreRawProjectionInfo(m00, m11);
        }
    }
    ApplyTargetFovOverride(fsv);   // FEATURE (widen game projection to headset FOV); [POSTPROJ] diagnostic log removed 2026-07-07
    // [SFRCONV] moved AFTER ApplyHeadRotation (see ApplyConvergenceSEH: GPU-matrix-only edit would be
    // wiped by the head-rotation recompose here).
    StashRenderFov(fsv);
    float invmatPitchRad = 0.0f;   // for the [INVMAT] recheck below
    {
        constexpr float kUUToRad = 6.2831853f / 65536.0f;
        // [PAIRLATCH] Stereo 2 renders TWO full passes per game frame (main + replay), but the
        // head-look angles live in atomics republished by the PRESENT thread - and a Present can
        // land between the two passes. Live reads gave each eye a slightly different head rotation
        // whenever the head was moving = the two images disagree = transient double vision that
        // merges when still (exact field report, 2026-07-17). Fix: the MAIN pass latches the
        // angles; the replay pass reuses the latched basis verbatim. Both passes run on the game
        // thread, so plain statics are safe. Single-pass modes hit the !t_fvcReplay branch every
        // frame -> latch == live read -> behaviorally identical.
        static bool s_pairLookOn = false;
        static int  s_pairYawUU = 0;
        static int  s_pairPitchUU = 0;
        static float s_pairYawF = 0.0f;
        static float s_pairPitchF = 0.0f;
        if (!t_fvcReplay)
        {
            s_pairLookOn  = g_headLookOn.load(std::memory_order_acquire);
            s_pairYawUU   = g_headYawUU.load(std::memory_order_relaxed);
            s_pairPitchUU = g_headPitchUU.load(std::memory_order_relaxed);
            s_pairYawF    = static_cast<float>(s_pairYawUU);
            s_pairPitchF  = static_cast<float>(s_pairPitchUU);
            // [POSEEXACT] stamp WHICH arm these angles came from, so the submit can tag with the
            // exact pose this render used rather than approximating the age of the frame.
            uint64_t pairArm = g_headArmSeq.load(std::memory_order_acquire);
            // [ME2SMOOTH/CINESAMPLE 2026-08-12] Consume the latest coherent publish exactly as
            // the working ME2 path does. LOOKPACE retained only two samples while claiming to be a
            // sequence consumer, so any phase burst necessarily became hold/skip lurches. Smooth
            // the source upstream; do not synthesize time or sequence here.
            if (SfrCineActive())
            {
                const LookSlot slot = g_lookSlots[g_lookSlotIdx.load(std::memory_order_acquire)];
                if (slot.arm > 0ull)
                {
                    s_pairLookOn = slot.on;
                    s_pairYawF = static_cast<float>(slot.yaw);
                    s_pairPitchF = static_cast<float>(slot.pitch);
                    pairArm = slot.arm;
                }
            }
            g_pairArmSeq.store(pairArm, std::memory_order_release);
            // [LOOKSTEP] publish the head-look this render CONSUMED (post-blend), so the submit
            // side keeps measuring the true content evenness.
            g_pairYawPub.store(static_cast<int>(s_pairYawF), std::memory_order_relaxed);
            g_pairPitchPub.store(static_cast<int>(s_pairPitchF), std::memory_order_relaxed);
        }
        else
        {
            // Instrument: count how often the live values actually diverged from the latch - this is
            // the direct evidence the Present-thread race was firing between the two eye passes.
            const int liveYaw = g_headYawUU.load(std::memory_order_relaxed);
            const int livePitch = g_headPitchUU.load(std::memory_order_relaxed);
            if (liveYaw != s_pairYawUU || livePitch != s_pairPitchUU)
            {
                static uint64_t s_divergeLog = 0;
                const uint64_t n = s_divergeLog++;
                if (n < 4 || (n % 600ull) == 0ull)
                    LogLine("[PAIRLATCH] race caught: replay pass live yaw/pitch=(" +
                            std::to_string(liveYaw) + "," + std::to_string(livePitch) +
                            ") vs latched (" + std::to_string(s_pairYawUU) + "," +
                            std::to_string(s_pairPitchUU) + ") diverge#" + std::to_string(n));
            }
        }
        float yawRad = 0.0f, pitchRad = 0.0f;
        if (s_pairLookOn)
        {
            yawRad   = s_pairYawF * kUUToRad;
            pitchRad = s_pairPitchF * kUUToRad;
        }
        // The per-first-person-state camera ANGLE fold-in (GameCamera::GetActiveFpViewAngleDeg) was REMOVED
        // 2026-07-03 with the FP camera-mode machinery. Head-look is the only view rotation source now.
        if (yawRad != 0.0f || pitchRad != 0.0f)
        {
            if (ApplyHeadRotation(fsv, yawRad, pitchRad)) invmatPitchRad = pitchRad;
        }
    }

    // [SFRCONV 2026-07-20 FINAL] convergence is NO LONGER applied at render AT ALL (ApplyConvergenceSEH
    // retired, left for reference). Every render-side variant (proj[8]+rebuild = ME2's literal code,
    // then GPU-TVP-only) shifted ME1's per-pass-drawn HUD along with the scene in one pass but not the
    // other -> the crosshair/interact/health copies split by 2*conv ("UI doubles with convergence").
    // Decision: DECOUPLE the UI from the slider. Convergence now happens at SUBMIT (xr_session
    // pair submit): each finished eye IMAGE - world and UI together, already composited - is shifted
    // horizontally toward the nose by conv/2 UV. A uniform NDC shift at render and an image shift at
    // submit are the same transform, so the world convergence is pixel-identical; the UI, rendered
    // unshifted in both passes, coincides with itself and can never double. Per-DESTINATION-eye sign,
    // so it also stays correct under swap-eyes.

    // [INVMAT] one-shot staleness proof: on the first view rendered with a real injected pitch (>5 deg),
    // measure how far each scan-validated inverse slot now is from inverting its edited target. err >> scan
    // err = the slot still describes the pre-edit camera = exactly the mismatch the panel theory needs.
    if (g_invScanDone.load(std::memory_order_acquire) && g_invSlotCount > 0 &&
        !g_invRecheckDone.load(std::memory_order_acquire) &&
        (invmatPitchRad > 0.087f || invmatPitchRad < -0.087f))
    {
        float errs[kInvMaxSlots] = {};
        if (InvMatErrsSEH(fsv, g_invSlots, g_invSlotCount, errs))
        {
            g_invRecheckDone.store(true, std::memory_order_release);
            for (int i = 0; i < g_invSlotCount; ++i)
                LogLine(std::string("[INVMAT] post-edit off=") + Hex64(g_invSlots[i].offset) +
                        " target=" + kInvTargets[g_invSlots[i].target].name +
                        " err=" + std::to_string(errs[i]) +
                        " pitchDeg=" + std::to_string(invmatPitchRad * 57.29578f) +
                        (errs[i] > 0.05f ? " -> STALE (slot still describes the pre-edit camera)"
                                         : " -> consistent (engine keeps it fresh)"));
        }
    }

    // [INVMAT] gated candidate fix (menu: "Panel fix: refresh inverse matrices"). Recompute every
    // scan-validated inverse slot from its edited target so screen->world reconstruction agrees with what
    // was actually rendered. Runs after ALL edits (FOV widen + view offset + head rotation). Default OFF.
    if (g_invFixEnabled.load(std::memory_order_acquire) &&
        g_invScanDone.load(std::memory_order_acquire) && g_invSlotCount > 0)
    {
        const int fixedCount = RefreshInverseSlotsSEH(fsv, g_invSlots, g_invSlotCount);
        const std::uint64_t k = g_invFixLogs.fetch_add(1, std::memory_order_relaxed);
        if (k < 4 || (k % 3600) == 0)
            LogLine("[INVMAT] fix active: refreshed " + std::to_string(fixedCount) + "/" +
                    std::to_string(g_invSlotCount) + " inverse slots");
    }

    // Release-publish the eye only after every camera edit is complete.
    if (aerPrimaryView) StampRenderedEye(aerRenderedEye);
}

bool TryP1LayoutStereo(std::uintptr_t retRva,
                       void* localPlayer,
                       void* viewFamily,
                       void* viewLocation,
                       void* viewRotation,
                       void* viewport,
                       void* viewDrawer,
                       void** outFsv) noexcept
{
    if (outFsv == nullptr) return false;
    *outFsv = nullptr;
    if (!g_p1StereoEnabled.load(std::memory_order_acquire) || retRva != kGameplayCalcSceneViewReturnRva) return false;
    if (g_origCalc == nullptr) return false;

    const uint64_t layoutCall = g_p1LayoutStereoCalls.fetch_add(1, std::memory_order_relaxed) + 1;
    const ViewFamilyInfo preInfo = ParseViewFamily(viewFamily);
    LocalPlayerLayout savedLayout = {};
    const bool haveLayout = ReadLocalPlayerLayout(localPlayer, &savedLayout);
    const bool cleanFamilyStart = Readable(viewFamily, 0x10) && preInfo.count == 0 && preInfo.capacity <= 64;
    const bool logSample = layoutCall <= 8 || layoutCall == 60 || layoutCall == 300 || (layoutCall % 1800) == 0;

    if (!haveLayout || !cleanFamilyStart)
    {
        if (logSample)
        {
            LogLine("[P1STEREO] skipped unsafe family/layout call=" + std::to_string(layoutCall) +
                    " haveLayout=" + std::to_string(haveLayout ? 1 : 0) +
                    " cleanFamilyStart=" + std::to_string(cleanFamilyStart ? 1 : 0) +
                    " countBefore=" + std::to_string(preInfo.count) +
                    " capacityBefore=" + std::to_string(preInfo.capacity));
        }
        return false;
    }

    float sizeY = 1.0f;
    const float eyeAspect = g_p1StereoEyeAspect.load(std::memory_order_relaxed);
    const float bbAspect = g_p1StereoBackbufferAspect.load(std::memory_order_relaxed);
    if (eyeAspect > 0.3f && eyeAspect < 3.0f && bbAspect > 0.5f && bbAspect < 4.0f)
    {
        sizeY = 0.5f * bbAspect / eyeAspect;
        if (sizeY > 1.0f) sizeY = 1.0f;
        if (sizeY < 0.1f) sizeY = 0.1f;
    }

    // CENTER the sub-height world viewport (2026-07-08, the "crosshair low" root fix). Top-aligned
    // (originY=0) put the world's optical center at y=sizeY/2=0.472 while the UI dup draws centered at
    // 0.5 - a 0.028 backbuffer gap = ~3.6 deg "crosshair below the shot" that NO affine submit crop can
    // undo (it preserves the gap). ME2 (which aligns perfectly) keeps world and UI both centered; with a
    // centered origin the crop band centers too and crosshair == shot at eye center. Knob made obsolete.
    const float originY = 0.5f * (1.0f - sizeY);
    const LocalPlayerLayout leftLayout{0.0f, originY, 0.5f, sizeY};
    const LocalPlayerLayout rightLayout{0.5f, originY, 0.5f, sizeY};
    const bool wroteLeftLayout = WriteLocalPlayerLayout(localPlayer, leftLayout);
    void* fsvLeft = wroteLeftLayout ? g_origCalc(localPlayer, viewFamily, viewLocation, viewRotation, viewport, viewDrawer) : nullptr;
    const ViewFamilyInfo afterLeftInfo = ParseViewFamily(viewFamily);

    const bool wroteRightLayout = fsvLeft != nullptr && WriteLocalPlayerLayout(localPlayer, rightLayout);
    // ME1 per-view effects fix: the old second call passed a null FViewElementDrawer, so anything
    // registered through the viewport's drawer existed only on the first/left view.  Build the right
    // view with the same engine-owned drawer so coronas/light elements and cinematic foreground view
    // elements are authored for both eyes at CalcSceneView time.
    void* fsvRight = wroteRightLayout ? g_origCalc(localPlayer, viewFamily, viewLocation, viewRotation, viewport, viewDrawer) : nullptr;
    const ViewFamilyInfo afterRightInfo = ParseViewFamily(viewFamily);
    const bool restoredLayout = WriteLocalPlayerLayout(localPlayer, savedLayout);

    if (fsvLeft == nullptr || fsvRight == nullptr)
    {
        if (logSample)
        {
            LogLine("[P1STEREO] partial second-view build call=" + std::to_string(layoutCall) +
                    " left=" + Hex64(reinterpret_cast<std::uintptr_t>(fsvLeft)) +
                    " right=" + Hex64(reinterpret_cast<std::uintptr_t>(fsvRight)) +
                    " wroteLeft=" + std::to_string(wroteLeftLayout ? 1 : 0) +
                    " wroteRight=" + std::to_string(wroteRightLayout ? 1 : 0) +
                    " restored=" + std::to_string(restoredLayout ? 1 : 0));
        }
        if (fsvLeft != nullptr) *outFsv = fsvLeft;
        return fsvLeft != nullptr;
    }

    // ===== ME2-EXACT stereo (ported 2026-07-02, replaces the old ApplyCloneEyeOffset + view-state injection) =====
    // PER-EYE VIEW STATE (STEREOGHOST fix, 2026-07-12): give each
    // eye its OWN standalone FSceneViewState so temporal effects (motion blur/TAA) don't borrow the game's
    // shared/native state - the same object the game overwrites every frame for its own rendering. That was the
    // "moving ghost" root cause (confirmed + fixed in ME2 first; ME1 shared it with BOTH eyes, not just one).
    // NOTE this is narrower than the ApplyCloneEyeOffset/ReplacePointerMatches machinery the rebuild dropped
    // below ("stereo fuses WITHOUT AllocateViewState / ReplacePointerMatches" - that was a broader pointer-
    // registration scheme, not this single FSceneView::State field write). Only sets the field, only on a
    // fresh successful allocation, NEVER forces NULL (an explicit NULL state is itself a smear source). If
    // fusion or occlusion regresses after this build, THIS is the change to re-check first.
    // (The cine keep-game-view-state branch lived here; removed 2026-07-18 - Stereo 1 never renders
    // cine anymore, cine is flat.)
    if (MELEVR::Config::Get().stereoPerEyeViewState)
    {
        void* leftState = EnsureAllocatedViewStateSlot(g_stereoViewStateLeft, "left");
        void* rightState = EnsureAllocatedViewStateSlot(g_stereoViewStateRight, "right");
        SetViewState(fsvLeft, leftState);
        SetViewState(fsvRight, rightState);
    }

    // ORDER IS THE WHOLE FIX: (1) sync right<-left so both eyes share ONE base camera; (2) symmetric IPD via a
    // real camera move (PreViewTranslation+ViewOrigin) along the BODY right axis; (3) THEN head-look + FOV +
    // view-proj rebuild in PostProcessPerspectiveFsv. Because separation is a pure camera translation and the
    // tags are fixed at the HMD's real eye positions (xr_session), raising the slider now SCALES the world (like
    // ME2/ME3) instead of splitting the image. This is ME2 calcview_hook.cpp:400-420 verbatim.
    SyncEyeCameraFields(fsvRight, fsvLeft);

    float eyeOffsetUU = g_p1StereoHalfEyeUU.load(std::memory_order_relaxed);
    if (!std::isfinite(eyeOffsetUU) || eyeOffsetUU < 0.0f) eyeOffsetUU = 0.0f;
    if (eyeOffsetUU > 32.0f) eyeOffsetUU = 32.0f;
    const bool swap = g_p1StereoSwapEyes.load(std::memory_order_relaxed);
    const float he = swap ? -eyeOffsetUU : eyeOffsetUU;   // ME2: left -he, right +he (swap flips)
    const bool leftNudged = ApplyEyeOffsetAER(fsvLeft, -he);
    const bool rightNudged = ApplyEyeOffsetAER(fsvRight, +he);

    PostProcessPerspectiveFsv(fsvLeft);
    PostProcessPerspectiveFsv(fsvRight);

    if (logSample)
    {
        LogLine("[P1STEREO] engine-authored P1 layout stereo returned call=" + std::to_string(layoutCall) +
                " left=" + Hex64(reinterpret_cast<std::uintptr_t>(fsvLeft)) +
                " right=" + Hex64(reinterpret_cast<std::uintptr_t>(fsvRight)) +
                " savedLayout={" + LocalPlayerLayoutText(savedLayout) + "}" +
                " leftLayout={" + LocalPlayerLayoutText(leftLayout) + "}" +
                " rightLayout={" + LocalPlayerLayoutText(rightLayout) + "}" +
                " countBefore=" + std::to_string(preInfo.count) +
                " countAfterLeft=" + std::to_string(afterLeftInfo.count) +
                " countAfterRight=" + std::to_string(afterRightInfo.count) +
                " restored=" + std::to_string(restoredLayout ? 1 : 0) +
                " eyeOffsetUU=" + std::to_string(eyeOffsetUU) +
                " swap=" + std::to_string(swap ? 1 : 0) +
                " leftNudged=" + std::to_string(leftNudged ? 1 : 0) +
                " rightNudged=" + std::to_string(rightNudged ? 1 : 0) +
                " eyeAspect=" + std::to_string(eyeAspect) +
                " bbAspect=" + std::to_string(bbAspect) +
                " viewStateLeft=" + Hex64(reinterpret_cast<std::uintptr_t>(g_stereoViewStateLeft)) +
                " viewStateRight=" + Hex64(reinterpret_cast<std::uintptr_t>(g_stereoViewStateRight)));
    }

    *outFsv = fsvLeft;
    return true;
}

// HarvestCameraValues (the read-only camera-value CSV harvester) was REMOVED 2026-07-03
// (strip-firstperson-livecode) along with the GameCamera module it depended on.

void* __fastcall HookCalcSceneView(void* a1, void* a2, void* a3, void* a4, void* a5, void* a6) noexcept
{
    if (g_origCalc == nullptr) return nullptr;

    // FIRST-PERSON CAMERA OWNERSHIP REMOVED 2026-07-03 (strip-firstperson-livecode): the K toggle, the
    // camera-value harvester, the SFXCameraMode ownership prewarm/tick, and the view-input pin all wrote or
    // scanned live game objects on the render thread every frame. They are gone. This hook is now read-only:
    // it only builds the stereo second view (layout save/restore) and applies render-side head-look/FOV.

    // Offset-validation dump (read-only, self-limits to 8), then the supersample write. Order matters:
    // the dump must see the field BEFORE it's touched, so dump 1 is always a clean vanilla reading.
    ProbeDynResFraction(a1);
    ApplyDynResFraction(a1);

    std::uintptr_t retRva = 0;
    BYTE* moduleBase = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    if (moduleBase != nullptr)
    {
        retRva = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - reinterpret_cast<std::uintptr_t>(moduleBase);
    }

    // Cache the proven FViewportClient::Draw ULocalPlayer, then use pointer identity exactly as
    // ME2/ME3 use EngineProbe::GetPrimaryLocalPlayer(). Auxiliary perspective views stay unowned.
    if (retRva == kGameplayCalcSceneViewReturnRva && a1 != nullptr)
    {
        const std::uintptr_t next = reinterpret_cast<std::uintptr_t>(a1);
        const std::uintptr_t prev = g_primaryLocalPlayer.exchange(next, std::memory_order_acq_rel);
        if (prev != next) LogLine("[AEROWN] primary ULocalPlayer acquired from gameplay CalcSceneView callsite.");
    }
    const bool aerEnabled = g_aerPrimaryEnabled.load(std::memory_order_acquire);
    // ME1 can call CalcSceneView several times with the same ULocalPlayer during one Draw. Pointer
    // identity alone therefore stamped auxiliary views as fresh AER frames (seq gaps up to 4 in the
    // live log). The proven gameplay callsite is the actual primary view and must own the eye.
    const bool isPrimary = retRva == kGameplayCalcSceneViewReturnRva &&
                           reinterpret_cast<std::uintptr_t>(a1) ==
                               g_primaryLocalPlayer.load(std::memory_order_acquire);

    void* stereoFsv = nullptr;
    if (TryP1LayoutStereo(retRva, a1, a2, a3, a4, a5, a6, &stereoFsv)) return stereoFsv;

    void* fsv = g_origCalc(a1, a2, a3, a4, a5, a6);
    // In AER only the primary view is shifted and stamped. Outside AER preserve established behavior.
    if (!aerEnabled || isPrimary) PostProcessPerspectiveFsv(fsv, aerEnabled && isPrimary);
    return fsv;
}

// [SFR] arming comes from the mode picker now (vrMode 4 = Stereo 2), published per frame by
// xr_session via SetSfrEnabled. The bring-up marker file is retired.
bool SfrM0Enabled(uint64_t /*drawSeq*/) noexcept
{
    return g_sfrModeEnabled.load(std::memory_order_acquire);
}

// [SFR] the capture+replay body, SEH-guarded so a fault becomes a log line instead of a dead game
// (the 16:18 main-menu crash died silently mid-replay). Separate function: no C++ unwind objects
// may share a frame with __try. stepOut says how far it got: 1=pass0 capture, 2=second Draw,
// 3=pass1 capture, 4=done.
std::atomic_bool g_sfrStepLogs{false};           // first frames only: breadcrumb before each step

void LogSfrStep(const char* what) noexcept
{
    // Called from inside SfrGuardedReplay's __try (the string object lives in THIS frame, which is
    // legal); LogLine writes+flushes per line, so these survive even a hard uncatchable death.
    if (!g_sfrStepLogs.load(std::memory_order_relaxed)) return;
    LogLine(std::string("[SFR] step: ") + what);
}

unsigned long SfrGuardedReplay(void* self, void* viewport, void* canvas, int* stepOut) noexcept
{
    // 2026-07-14 render-side-capture rework: this GAME-thread function no longer touches D3D at all
    // (its immediate-context copies raced the render thread - the 4-frames-then-die crash). It only
    // enqueues the second Draw; [SFR-CAP] in d3d_capture does all copying on the render thread.
    unsigned long code = 0;
    __try
    {
        *stepOut = 1;
        LogSfrStep("1 second FViewportClient::Draw (replay-shifted camera; captures are render-side)...");
        t_fvcReplay = true;
        g_origFViewportClientDraw(self, viewport, canvas);
        t_fvcReplay = false;
        *stepOut = 2;
        LogSfrStep("2 done");
    }
    __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
    {
        t_fvcReplay = false;
    }
    return code;
}

void __fastcall HookFViewportClientDraw(void* self, void* viewport, void* canvas) noexcept
{
    const uint64_t seq = g_fvcDrawSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_fvcDrawCallsThisPresent.fetch_add(1, std::memory_order_relaxed);
    g_lastFvcDrawThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    ++t_fvcDrawDepth;
    LogFvcDrawCall(seq, self, viewport, canvas, t_fvcDrawDepth);
    // [NOLETTERBOX] must run before the original Draw builds its scene views - that is the whole
    // fix (the 2026-07-08 present-time attempt lost this exact race). Depth 1 only: the SFR replay
    // re-enters this detour and the flag cannot have been re-set mid-frame.
    if (t_fvcDrawDepth == 1 && g_cineUnconstrain.load(std::memory_order_acquire))
    {
        const int r = MELEVR::HeadAim::ClearCameraAspectConstraintSEH();
        if (r == 1)
        {
            const int n = g_noLetterboxClears.fetch_add(1, std::memory_order_relaxed);
            if (n < 3 || (n % 300) == 0)
                LogLine("[NOLETTERBOX] cleared bConstrainAspectRatio pre-Draw (clear #" +
                        std::to_string(n + 1) + ") - cine renders full-frame like gameplay");
        }
    }
    // [SFR] one-shot thread identity check (16:40 crash died in CaptureStereoPass(0), which uses the
    // immediate context from THIS thread - only legal if this is also the DXGI Present thread).
    if (seq == 1)
    {
        const DWORD presentThread = g_lastPresentThread.load(std::memory_order_relaxed);
        const DWORD here = GetCurrentThreadId();
        LogLine("[SFR] threadcheck fvcDraw=" + std::to_string(here) +
                " present=" + std::to_string(presentThread) +
                (presentThread == 0 ? " (no present seen yet)"
                 : (here == presentThread
                        ? " SAME - immediate-context capture is legal on this thread"
                        : " DIFFERENT - capture from this thread races the render thread; move it")));
    }

    if (g_origFViewportClientDraw != nullptr)
    {
        g_origFViewportClientDraw(self, viewport, canvas);

        const bool oneShotReplay = g_fvcReplayArmed.exchange(false, std::memory_order_acq_rel);
        // [SFR M0] continuous same-frame second render: every frame gets a full second Draw with the
        // pass-1 eye offset; the pass captures publish the stereo pair the Mono submit already prefers.
        // Gates: (1) P1 SBS stereo off (menu mode switch = live escape hatch); (2) a PERSPECTIVE scene
        // rendered within the last 4 presents - menus/movies/loading never double-draw (the 16:18
        // main-menu crash lived in that territory, and SFR only matters in the 3D world anyway);
        // (3) auto-disarm after 3 guarded exceptions so a broken replay can't hiccup every frame.
        const uint64_t presentNow = g_presentSeq.load(std::memory_order_acquire);
        const uint64_t lastPersp = g_lastPerspectiveFsvPresent.load(std::memory_order_acquire);
        const bool in3DScene = lastPersp != 0 && presentNow >= lastPersp && (presentNow - lastPersp) <= 4;
        // ENGINE game-mode gate (the 16:31 lesson): ME1's MAIN MENU renders a perspective scene (the
        // Normandy flyby), so perspective freshness alone armed the replay at the menu, where the
        // re-entered Draw dies uncatchably mid-GUI. The EGameModes byte is authoritative: 0..4 =
        // real gameplay (default/vehicle/wheels/command); menu=7, movie=8, galaxy=9, unreadable=-1
        // all stay mono. Published per frame by xr_session's existing read.
        const int sfrGameMode = g_sfrGameMode.load(std::memory_order_acquire);
        // Convo (5) / cutscene (6) replay like gameplay ([CLEANVRCINE]) so the pair keeps publishing
        // through cine instead of holding stale.
        const bool inGameplayMode = SfrModeAllowsReplay(sfrGameMode);
        const bool sfrContinuous = !oneShotReplay &&
                                   in3DScene &&
                                   inGameplayMode &&
                                   g_sfrExceptions.load(std::memory_order_acquire) < 3 &&
                                   SfrM0Enabled(seq) &&
                                   !g_p1StereoEnabled.load(std::memory_order_acquire);
        const bool doReplay = t_fvcDrawDepth == 1 && !t_fvcReplay && (oneShotReplay || sfrContinuous);
        if (doReplay)
        {
            const uint64_t sfrFrame = g_sfrFrames.fetch_add(1, std::memory_order_relaxed) + 1;
            const bool verbose = oneShotReplay || sfrFrame <= 4 || (sfrFrame % 600) == 0;
            if (verbose)
            {
                LogLine("[SFR] frame=" + std::to_string(sfrFrame) +
                        (oneShotReplay ? " (one-shot)" : "") +
                        " BEGIN present=" + std::to_string(presentNow) +
                        " perspAge=" + std::to_string(presentNow - lastPersp) +
                        " thread=" + std::to_string(GetCurrentThreadId()));
                if (oneShotReplay)
                {
                    MELEVR::D3DCapture::LogDeviceHealth("before-replay");
                    MELEVR::D3DCapture::LogVideoMemory("before-replay", true);
                }
            }
            const uint64_t replayCalcBefore = g_replayCalcTotal.load(std::memory_order_acquire);
            g_sfrStepLogs.store(sfrFrame <= 4, std::memory_order_relaxed);
            int step = 0;
            const uint64_t sfrStart = NowQpc();
            const unsigned long exceptionCode = SfrGuardedReplay(self, viewport, canvas, &step);
            const uint64_t sfrEnd = NowQpc();
            if (exceptionCode != 0)
            {
                const int strikes = g_sfrExceptions.fetch_add(1, std::memory_order_acq_rel) + 1;
                LogLine("[SFR] frame=" + std::to_string(sfrFrame) +
                        " EXCEPTION 0x" + Hex64(exceptionCode) +
                        " at step=" + std::to_string(step) +
                        " (1=secondDraw) - replay aborted, game continues; strike " +
                        std::to_string(strikes) + "/3" +
                        (strikes >= 3 ? " -> SFR AUTO-DISARMED for this session" : ""));
            }
            else
            {
                MELEVR::D3DCapture::NotifySfrReplayEnqueued();   // arm render-side pass capture
                if (oneShotReplay)
                {
                    MELEVR::D3DCapture::LogDeviceHealth("after-replay");
                    MELEVR::D3DCapture::LogVideoMemory("after-replay", true);
                }
                if (verbose)
                {
                    const uint64_t replayCalcAfter = g_replayCalcTotal.load(std::memory_order_acquire);
                    LogLine("[SFR] frame=" + std::to_string(sfrFrame) +
                            " survived second Draw step=" + std::to_string(step) +
                            " pass1Us=" + std::to_string(QpcToUs(sfrStart, sfrEnd)) +
                            " replayCalcDelta=" + std::to_string(replayCalcAfter - replayCalcBefore) +
                            " halfEyeUU=" + std::to_string(g_sfrHalfEyeUU.load(std::memory_order_relaxed)));
                }
            }
        }
    }

    --t_fvcDrawDepth;
}

}  // namespace

namespace MELEVR::RenderHook
{
bool Install() noexcept
{
    if (g_installed.load(std::memory_order_acquire)) return true;

    const MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
    {
        LogLine("[M1] MH_Initialize FAILED status=" + std::to_string(static_cast<int>(init)));
        return false;
    }

    BYTE* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));

    // [SFRADDR] harvest the REAL FViewportClient::Draw entry from the exe's own unwind tables
    // (log-only, patches nothing). The 2026-07-14 blackout: detouring the inherited, never-fired
    // kFViewportClientDrawRva 0x4C71A0 broke rendering while the hook never logged a call - so that
    // address is NOT the entry of the function that contains the proven-live CalcSceneView call site
    // (kGameplayCalcSceneViewReturnRva 0x4C7A4E, matched every frame by TryP1LayoutStereo). Windows
    // keeps a RUNTIME_FUNCTION table for every x64 exe; asking it "which function contains
    // 0x4C7A4E?" yields the true Draw entry. Also resolve what 0x4C71A0 actually sits inside, to
    // know what the blackout corrupted.
    {
        const std::uintptr_t probes[2] = { kGameplayCalcSceneViewReturnRva, kFViewportClientDrawRva };
        const char* names[2] = { "calcCallSite", "claimedDrawEntry" };
        for (int i = 0; i < 2; ++i)
        {
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(
                reinterpret_cast<DWORD64>(base) + probes[i], &imageBase, nullptr);
            if (rf != nullptr)
            {
                LogLine(std::string("[SFRADDR] ") + names[i] + "=+" + Hex64(probes[i]) +
                        " containingFunction=+" + Hex64(rf->BeginAddress) +
                        "..+" + Hex64(rf->EndAddress) +
                        (probes[i] == kGameplayCalcSceneViewReturnRva
                             ? (rf->BeginAddress == kFViewportClientDrawRva
                                    ? " (matches claimed entry 0x4C71A0)"
                                    : " <- THE REAL FViewportClient::Draw ENTRY (claimed 0x4C71A0 is wrong)")
                             : ""));
            }
            else
            {
                LogLine(std::string("[SFRADDR] ") + names[i] + "=+" + Hex64(probes[i]) +
                        " has NO unwind entry (lookup returned null)");
            }
        }
    }

    void* target = base + kCalcSceneViewRva;
    const MH_STATUS cc = MH_CreateHook(target, reinterpret_cast<void*>(&HookCalcSceneView),
                                       reinterpret_cast<void**>(&g_origCalc));
    if (cc != MH_OK)
    {
        LogLine("[M1] MH_CreateHook CalcSceneView FAILED status=" + std::to_string(static_cast<int>(cc)));
        return false;
    }
    if (MH_EnableHook(target) != MH_OK)
    {
        LogLine("[M1] MH_EnableHook CalcSceneView FAILED");
        return false;
    }

    const bool fvcProbes = MarkerEnabled(L"MELEVR_ENABLE_FVC_PROBES.txt");
    g_fvcProbeHooksEnabled.store(fvcProbes, std::memory_order_release);
    // [SFR] Stereo 2 rides the FViewportClient::Draw detour, and the mode can be switched on at any
    // time from the Insert menu - so the detour ALWAYS installs (address unwind-table-verified,
    // pass-through proven harmless incl. full gameplay 2026-07-14; it does nothing unless Stereo 2
    // is the active mode). Probe logging/F8 arming stay gated on the probes marker.
    {
        void* fvcTarget = base + kFViewportClientDrawRva;
        unsigned char pre[16] = {};
        std::memcpy(pre, fvcTarget, sizeof(pre));
        const MH_STATUS fvc = MH_CreateHook(fvcTarget, reinterpret_cast<void*>(&HookFViewportClientDraw),
                                            reinterpret_cast<void**>(&g_origFViewportClientDraw));
        if (fvc != MH_OK)
        {
            LogLine("[BOUNDARY] MH_CreateHook FViewportClient::Draw FAILED status=" + std::to_string(static_cast<int>(fvc)));
            return false;
        }
        const MH_STATUS fvcEnable = MH_EnableHook(fvcTarget);
        if (fvcEnable != MH_OK)
        {
            LogLine("[BOUNDARY] MH_EnableHook FViewportClient::Draw FAILED status=" + std::to_string(static_cast<int>(fvcEnable)));
            return false;
        }
        unsigned char post[16] = {};
        std::memcpy(post, fvcTarget, sizeof(post));
        char bytes[128] = {};
        int off = 0;
        for (int i = 0; i < 16 && off < 100; ++i) off += sprintf_s(bytes + off, sizeof(bytes) - off, "%02X", pre[i]);
        off += sprintf_s(bytes + off, sizeof(bytes) - off, ">");
        for (int i = 0; i < 16 && off < 124; ++i) off += sprintf_s(bytes + off, sizeof(bytes) - off, "%02X", post[i]);
        LogLine(std::string("[BOUNDARY] FViewportClient::Draw hook INSTALLED at +") + Hex64(kFViewportClientDrawRva) +
                " trampoline=" + Hex64(reinterpret_cast<std::uintptr_t>(g_origFViewportClientDraw)) +
                " probes=" + std::to_string(fvcProbes ? 1 : 0) +
                " entryBytes=" + bytes);
    }

    if (fvcProbes)
    {
        void* childDetours[kPostCalcChildCount] = {
            reinterpret_cast<void*>(&HookPostCalcChildA),
            reinterpret_cast<void*>(&HookPostCalcChildB),
            reinterpret_cast<void*>(&HookPostCalcChildC),
            reinterpret_cast<void*>(&HookPostCalcChildD),
            reinterpret_cast<void*>(&HookPostCalcChildE),
        };
        for (int i = 0; i < kPostCalcChildCount; ++i)
        {
            if (i != kActiveChildReplayIndex) continue;

            void* childTarget = base + kPostCalcChildren[i].rva;
            const MH_STATUS child = MH_CreateHook(childTarget, childDetours[i],
                                                  reinterpret_cast<void**>(&g_origPostCalcChildren[i]));
            if (child != MH_OK)
            {
                LogLine(std::string("[") + kPostCalcChildren[i].tag +
                        "] MH_CreateHook post-Calc child FAILED rva=" + Hex64(kPostCalcChildren[i].rva) +
                        " status=" + std::to_string(static_cast<int>(child)));
                continue;
            }
            const MH_STATUS childEnable = MH_EnableHook(childTarget);
            if (childEnable != MH_OK)
            {
                LogLine(std::string("[") + kPostCalcChildren[i].tag +
                        "] MH_EnableHook post-Calc child FAILED rva=" + Hex64(kPostCalcChildren[i].rva) +
                        " status=" + std::to_string(static_cast<int>(childEnable)));
                continue;
            }
            LogLine(std::string("[") + kPostCalcChildren[i].tag +
                    "] post-Calc direct child pass-through installed rva=" + Hex64(kPostCalcChildren[i].rva) + ".");
        }
    }

    if (fvcProbes) LogFunctionBoundsOnce();

    g_installed.store(true, std::memory_order_release);
    LogLine("[M1] CalcSceneView head-look hook installed (addr base+0x4C5300).");
    if (fvcProbes)
    {
        LogLine("[BOUNDARY] FViewportClient::Draw pass-through probe installed (addr base+0x4C71A0).");
        LogLine("[FVCCHILD] direct child-call map logged; B/C seam hooks intentionally disabled after wrong-thread result.");
    }
    return true;
}

bool Installed() noexcept { return g_installed.load(std::memory_order_acquire); }

uint64_t GetHeadArmSeq() noexcept { return g_headArmSeq.load(std::memory_order_acquire); }
uint64_t GetPairArmSeq() noexcept { return g_pairArmSeq.load(std::memory_order_acquire); }

// [CINETILT] read-and-reset for the [CINEJIT] per-second line: last camera tilt + the largest
// per-frame camera basis rotation since the previous call, both in degrees.
void ReadCineCamTelemetry(float& tiltDeg, float& camStepDegMax) noexcept
{
    tiltDeg = static_cast<float>(g_cineTiltCdeg.load(std::memory_order_relaxed)) * 0.01f;
    camStepDegMax = static_cast<float>(g_cineCamStepCdegMax.exchange(0, std::memory_order_relaxed)) * 0.01f;
}

// [LOOKSTEP] head-look (UU) the main pass latched for the pair currently being rendered.
void GetPairLookUU(int& yawUU, int& pitchUU) noexcept
{
    yawUU = g_pairYawPub.load(std::memory_order_relaxed);
    pitchUU = g_pairPitchPub.load(std::memory_order_relaxed);
}


void SetHeadLook(int32_t yawUU, int32_t pitchUU, bool on) noexcept
{
    const uint64_t arm = g_headArmSeq.fetch_add(1, std::memory_order_acq_rel) + 1ull;
    g_headYawUU.store(on ? yawUU : 0, std::memory_order_relaxed);
    g_headPitchUU.store(on ? pitchUU : 0, std::memory_order_relaxed);
    g_headLookOn.store(on, std::memory_order_release);
    // [LOOKGATE 2026-08-12] The [LOOKSTEP] probe caught 10-15 deg single-frame look jumps with the
    // head STILL and the camera static - the signature of the look being zeroed and re-applied
    // mid-scene (a publish of on=false zeroes the angles, the next real publish is a full-size
    // step = a visible snap). Log every on<->off transition with the angles it left from, so the
    // next natural run names which caller is flapping. Bounded.
    static bool s_gatePrevOn = false;
    static int  s_gateLogs = 0;
    if (on != s_gatePrevOn && s_gateLogs < 200)
    {
        ++s_gateLogs;
        LogLine(std::string("[LOOKGATE] head-look ") + (on ? "ON" : "OFF") +
                " at arm=" + std::to_string(arm) +
                " yawUU=" + std::to_string(yawUU) + " pitchUU=" + std::to_string(pitchUU));
    }
    s_gatePrevOn = on;
    // [CINESAMPLE] Publish one coherent sample into the inactive slot, then flip.
    const int active = g_lookSlotIdx.load(std::memory_order_acquire);
    LookSlot& next = g_lookSlots[active ^ 1];
    next.yaw = on ? yawUU : 0;
    next.pitch = on ? pitchUU : 0;
    next.arm = arm;
    next.on = on;
    g_lookSlotIdx.store(active ^ 1, std::memory_order_release);
}

int32_t HeadLookYawUU() noexcept
{
    return g_headLookOn.load(std::memory_order_acquire)
               ? g_headYawUU.load(std::memory_order_relaxed) : 0;
}

int32_t HeadLookPitchUU() noexcept
{
    return g_headLookOn.load(std::memory_order_acquire)
               ? g_headPitchUU.load(std::memory_order_relaxed) : 0;
}

void SetInverseFix(bool enabled) noexcept
{
    const bool was = g_invFixEnabled.exchange(enabled, std::memory_order_acq_rel);
    if (was != enabled)
        LogLine(std::string("[INVMAT] fix toggled ") + (enabled ? "ON" : "OFF"));
}

bool GetRenderFovHalfAngles(float* halfHorizRad, float* halfVertRad) noexcept
{
    if (!g_fovValid.load(std::memory_order_acquire)) return false;
    if (halfHorizRad) *halfHorizRad = g_fovHalfHoriz.load(std::memory_order_relaxed);
    if (halfVertRad) *halfVertRad = g_fovHalfVert.load(std::memory_order_relaxed);
    return true;
}

bool GetLastRawProjection(float* aspect, float* halfHorizRad, float* halfVertRad) noexcept
{
    if (!g_rawProjValid.load(std::memory_order_acquire)) return false;
    if (aspect) *aspect = g_rawProjAspect.load(std::memory_order_relaxed);
    if (halfHorizRad) *halfHorizRad = g_rawProjHalfHoriz.load(std::memory_order_relaxed);
    if (halfVertRad) *halfVertRad = g_rawProjHalfVert.load(std::memory_order_relaxed);
    return true;
}

void SetTargetFovHalfAngles(float halfHorizRad, float halfVertRad, bool enabled) noexcept
{
    if (!enabled || !std::isfinite(halfHorizRad) || !std::isfinite(halfVertRad) ||
        halfHorizRad < 0.10f || halfVertRad < 0.10f)
    {
        g_targetFovEnabled.store(false, std::memory_order_release);
        return;
    }
    if (halfHorizRad > 2.20f) halfHorizRad = 2.20f;
    if (halfVertRad > 2.20f) halfVertRad = 2.20f;
    g_targetFovHalfHoriz.store(halfHorizRad, std::memory_order_relaxed);
    g_targetFovHalfVert.store(halfVertRad, std::memory_order_relaxed);
    g_targetFovEnabled.store(true, std::memory_order_release);
}

void SetViewOffset(float rightUU, float upUU, float fwdUU) noexcept
{
    SetViewOffsetEye(rightUU, upUU, fwdUU, -1, false);
}

void SetViewOffsetEye(float rightUU, float upUU, float fwdUU, int eye, bool aer) noexcept
{
    g_viewOffRight.store(rightUU, std::memory_order_relaxed);
    g_viewOffUp.store(upUU, std::memory_order_relaxed);
    g_viewOffFwd.store(fwdUU, std::memory_order_relaxed);
    g_viewOffEye.store(aer ? eye : -1, std::memory_order_relaxed);
    g_viewOffAer.store(aer, std::memory_order_release);
}

void SetAerState(bool enabled, int renderEye, float halfEyeUU, bool swapEyes) noexcept
{
    if (renderEye < 0 || renderEye > 1) renderEye = 0;
    if (!std::isfinite(halfEyeUU) || halfEyeUU < 0.0f) halfEyeUU = 0.0f;
    if (halfEyeUU > 32.0f) halfEyeUU = 32.0f;
    g_aerPrimaryRenderEye.store(renderEye, std::memory_order_relaxed);
    g_aerPrimaryHalfEyeUU.store(halfEyeUU, std::memory_order_relaxed);
    g_aerPrimarySwapEyes.store(swapEyes, std::memory_order_relaxed);
    g_aerPrimaryEnabled.store(enabled, std::memory_order_release);
    if (!enabled) g_renderStampAer.store(false, std::memory_order_release);
}

void PublishGameModeForSfr(int gameMode, bool allowConvo, bool allowCutscene, bool liveGui7) noexcept
{
    g_sfrConvoAllowed.store(allowConvo, std::memory_order_release);
    g_sfrCutsceneAllowed.store(allowCutscene, std::memory_order_release);
    g_sfrLiveGui7.store(liveGui7, std::memory_order_release);
    g_sfrGameMode.store(gameMode, std::memory_order_release);
}

void SetSfrEnabled(bool enabled, float halfEyeUU) noexcept
{
    g_sfrModeEnabled.store(enabled, std::memory_order_release);
    // [SWAPME2] negative = swap-eyes flips the render cameras (ME2 parity); magnitude still bounded.
    if (std::isfinite(halfEyeUU) && std::fabs(halfEyeUU) <= 32.0f)
    {
        g_sfrHalfEyeUU.store(halfEyeUU, std::memory_order_relaxed);
    }
}

// [SFRCONV] publish the convergence knob (off-axis per-eye projection shift). Clamped to a sane range so a
// bad ini can't shear the frustum into garbage. 0 = fusion plane at infinity (parallel eyes).
// [NOLETTERBOX] published per frame from xr_session (Stereo 2 + VR cine active).
void SetCineUnconstrain(bool on) noexcept
{
    g_cineUnconstrain.store(on, std::memory_order_release);
}

void SetSfrConvergence(float convergence) noexcept
{
    if (!std::isfinite(convergence)) return;
    const float c = convergence < -0.2f ? -0.2f : (convergence > 0.2f ? 0.2f : convergence);
    g_sfrConvergence.store(c, std::memory_order_relaxed);
}

void SetP1LayoutStereo(bool enabled, float halfEyeUU, bool swapEyes, float eyeAspect, float backbufferAspect) noexcept
{
    if (!std::isfinite(halfEyeUU) || halfEyeUU < 0.0f) halfEyeUU = 0.0f;
    if (halfEyeUU > 32.0f) halfEyeUU = 32.0f;
    if (!std::isfinite(eyeAspect) || eyeAspect <= 0.0f) eyeAspect = 1.0f;
    if (!std::isfinite(backbufferAspect) || backbufferAspect <= 0.0f) backbufferAspect = 1.0f;
    g_p1StereoHalfEyeUU.store(halfEyeUU, std::memory_order_relaxed);
    g_p1StereoSwapEyes.store(swapEyes, std::memory_order_relaxed);
    g_p1StereoEyeAspect.store(eyeAspect, std::memory_order_relaxed);
    g_p1StereoBackbufferAspect.store(backbufferAspect, std::memory_order_relaxed);
    const bool wasEnabled = g_p1StereoEnabled.exchange(enabled, std::memory_order_acq_rel);
    if (enabled && !wasEnabled)
    {
        g_p1LayoutStereoCalls.store(0, std::memory_order_release);
        LogLine("[P1STEREO] True Stereo mode enabled halfEyeUU=" + std::to_string(halfEyeUU) +
                " swap=" + std::to_string(swapEyes ? 1 : 0) +
                " eyeAspect=" + std::to_string(eyeAspect) +
                " bbAspect=" + std::to_string(backbufferAspect));
    }
    else if (!enabled && wasEnabled)
    {
        LogLine("[P1STEREO] True Stereo mode disabled.");
    }
}

void SetSyncStereoReplay(bool enabled, float replayShiftRightUU) noexcept
{
    g_childReplayShiftRightUU.store(replayShiftRightUU, std::memory_order_release);
    const bool wasEnabled = g_childReplayAuto.exchange(enabled, std::memory_order_acq_rel);
    if (enabled)
    {
        if (!wasEnabled)
        {
            g_childReplayAutoLastPresent.store(0, std::memory_order_release);
            g_childReplayAutoCaptures.store(0, std::memory_order_release);
            LogLine("[CHILDREPLAY] Sync CHILDD enabled: auto replay will capture one shifted stereo pair.");
        }
    }
    else if (wasEnabled)
    {
        g_childReplayAutoLastPresent.store(0, std::memory_order_release);
        g_childReplayAutoCaptures.store(0, std::memory_order_release);
        LogLine("[CHILDREPLAY] auto replay disarmed.");
    }
}

// [AERSHAKE] Consume the build AFTER lastSeq, oldest-first - one per present, matching swapchain
// delivery order, so the eye label always belongs to the pixels on screen. False = nothing new.
// If the wanted slot was overwritten (consumer fell a full ring behind: mode switch, menu, load),
// resync to the newest build and report it via *resynced.
bool ConsumeRenderStamp(unsigned long long lastSeq, unsigned long long* outSeq, int* outEye, bool* resynced) noexcept
{
    *resynced = false;
    if (!g_renderStampAer.load(std::memory_order_acquire)) return false;   // AER not stamping
    const uint64_t newest = g_renderStampSeq.load(std::memory_order_acquire);
    if (newest <= lastSeq) return false;
    uint64_t want = lastSeq + 1;
    const AerRingSlot* slot = &g_aerRing[want % kAerRingN];
    if (slot->seq.load(std::memory_order_acquire) != want)
    {
        want = newest;
        *resynced = true;
        slot = &g_aerRing[want % kAerRingN];
        if (slot->seq.load(std::memory_order_acquire) != want) return false;   // slot mid-write
    }
    *outSeq = want;
    *outEye = slot->eye.load(std::memory_order_relaxed);
    return true;
}

bool GetLastRenderStamp(RenderStamp* out) noexcept
{
    if (out == nullptr) return false;
    const uint64_t seq = g_renderStampSeq.load(std::memory_order_acquire);
    if (seq == 0) return false;
    out->seq = seq;
    out->eye = g_renderStampEye.load(std::memory_order_relaxed);
    out->aer = g_renderStampAer.load(std::memory_order_relaxed);
    return out->aer && out->eye >= 0 && out->eye <= 1;
}

uint64_t PresentsSinceLastPerspective() noexcept
{
    const uint64_t p = g_presentSeq.load(std::memory_order_acquire);
    const uint64_t lp = g_lastPerspectiveFsvPresent.load(std::memory_order_acquire);
    return (p > lp) ? (p - lp) : 0;
}

void NotifyPresentTick() noexcept
{
    const uint64_t present = g_presentSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_lastPresentThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (!g_fvcProbeHooksEnabled.load(std::memory_order_acquire))
    {
        g_calcCallsThisPresent.exchange(0, std::memory_order_acq_rel);
        g_replayCalcCallsThisPresent.exchange(0, std::memory_order_acq_rel);
        g_fvcDrawCallsThisPresent.exchange(0, std::memory_order_acq_rel);
        return;
    }

    const bool f9Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
    const bool wasDown = g_f9WasDown.exchange(f9Down, std::memory_order_acq_rel);
    if (f9Down && !wasDown)
    {
        g_fvcReplayArmed.store(true, std::memory_order_release);
        LogLine("[CHILDREPLAY] ARMED by F8 - next CHILDD pass will replay once with temporary FSceneView shift.");
    }

    const uint64_t calcCalls = g_calcCallsThisPresent.exchange(0, std::memory_order_acq_rel);
    const uint64_t replayCalcCalls = g_replayCalcCallsThisPresent.exchange(0, std::memory_order_acq_rel);
    const uint64_t fvcCalls = g_fvcDrawCallsThisPresent.exchange(0, std::memory_order_acq_rel);
    const uint64_t bCalls = g_sceneBCallsThisPresent.exchange(0, std::memory_order_acq_rel);
    const uint64_t cCalls = g_sceneCCallsThisPresent.exchange(0, std::memory_order_acq_rel);
    const uint64_t bCalc = g_sceneBCalcThisPresent.exchange(0, std::memory_order_acq_rel);
    const uint64_t cCalc = g_sceneCCalcThisPresent.exchange(0, std::memory_order_acq_rel);
    if (BoundaryProbeEnabled() && ((present % 120) == 1 || calcCalls < 1 || fvcCalls != 1))
    {
        LogLine("[BOUNDARY] present=" + std::to_string(present) +
                " calcCallsSinceLastPresent=" + std::to_string(calcCalls) +
                " replayCalcCallsSinceLastPresent=" + std::to_string(replayCalcCalls) +
                " fvcDrawCallsSinceLastPresent=" + std::to_string(fvcCalls) +
                " presentThread=" + std::to_string(g_lastPresentThread.load(std::memory_order_relaxed)) +
                " calcThread=" + std::to_string(g_lastCalcThread.load(std::memory_order_relaxed)) +
                " fvcDrawThread=" + std::to_string(g_lastFvcDrawThread.load(std::memory_order_relaxed)) +
                " renderStampSeq=" + std::to_string(g_renderStampSeq.load(std::memory_order_acquire)) +
                " renderStampEye=" + std::to_string(g_renderStampEye.load(std::memory_order_relaxed)));
    }
    if (BoundaryProbeEnabled() && ((present % 120) == 1 || bCalls != 1 || cCalls != 1 || bCalc != 0 || cCalc != 0 || fvcCalls != 1))
    {
        LogLine("[SEAMSUM] present=" + std::to_string(present) +
                " fvcCalls=" + std::to_string(fvcCalls) +
                " bCalls=" + std::to_string(bCalls) +
                " cCalls=" + std::to_string(cCalls) +
                " bCalcInside=" + std::to_string(bCalc) +
                " cCalcInside=" + std::to_string(cCalc) +
                " calcCalls=" + std::to_string(calcCalls) +
                " replayCalcCalls=" + std::to_string(replayCalcCalls));
    }
}

bool GetBaseViewOrigin(float* x, float* y, float* z) noexcept
{
    if (!g_baseVOValid.load(std::memory_order_acquire)) return false;
    if (x) *x = g_baseVOx.load(std::memory_order_relaxed);
    if (y) *y = g_baseVOy.load(std::memory_order_relaxed);
    if (z) *z = g_baseVOz.load(std::memory_order_relaxed);
    return true;
}

bool GetLastViewRight(float* x, float* y, float* z) noexcept
{
    if (!g_lastRightValid.load(std::memory_order_acquire)) return false;
    if (x) *x = g_lastRightX.load(std::memory_order_relaxed);
    if (y) *y = g_lastRightY.load(std::memory_order_relaxed);
    if (z) *z = g_lastRightZ.load(std::memory_order_relaxed);
    return true;
}
}
