#pragma once

#include <Windows.h>

struct ID3D11Texture2D;
struct IDXGISwapChain;

// Milestone 1 / step 0b-1: capture the game's D3D11 device + backbuffer and hook Present,
// by patching the DXGI factory's CreateSwapChain vtable slot. Logging only at this step -
// no OpenXR, no stereo, no camera. The device captured here feeds the OpenXR session in 0b-2.

namespace MELEVR::D3DCapture
{
// Called by the dxgi proxy right after the game creates a DXGI factory.
void TryInstallFactoryHooks(void* factory, REFIID requestedFactoryId) noexcept;
void SetFactoryCaptureSuppressed(bool suppressed) noexcept;   // [SVRDIAG2] guard for mod-internal device probes

// Compute the auto-resolution target and write it into GamerSettings.ini. MUST be called from DllMain
// (DLL_PROCESS_ATTACH), synchronously, BEFORE the worker thread is created: UE3 reads its config during
// engine init, which races the worker thread and wins (proven 2026-07-10 - the worker got there 0.9s late
// and the game rendered the previous launch's resolution). DllMain runs before main(), so it cannot lose.
// File I/O only. Also latches the real primary size for the display hooks below.
void ApplyAutoResEarly() noexcept;

// [DISPQ] Install from the worker thread. LE1 clamps its render resolution to the primary monitor rect (read
// via GetMonitorInfoW); these hooks report the larger target so the game renders above the display. Safe to
// run late relative to the ini write - these are live API calls the game makes during D3D init.
void InstallDisplayQueryHooks() noexcept;

// [DOF] Force GamerSettings.ini [SystemSettings] DepthOfField=False (disable=true) or =True. Preserving
// read-modify-write; takes effect on the next game launch (engine reads SystemSettings at load).
bool EnsureDepthOfFieldSetting(bool disable) noexcept;

// [ENGSMOOTH] Assert SmoothFrameRate=TRUE + MaxSmoothedFrameRate=120 in BIOEngine.ini. Must run at
// DLL attach, before the engine's config read: the game re-syncs this file between sessions, so a
// one-time edit does not survive. Tick-delta smoothing is what keeps animation-driven cine cameras
// from juddering; 120 keeps the gameplay framerate the FALSE write was bought for.
bool EnsureBioEngineSmoothing() noexcept;

// The REAL primary display size, captured at DLL attach before any spoof hook exists. Anything inside the mod
// that needs the true screen size must use this - GetSystemMetrics/GetMonitorInfo are hooked and will hand
// back the spoofed size, i.e. that would be a lie about its own state. Returns false if not captured yet.
bool GetRealPrimarySize(unsigned* outW, unsigned* outH) noexcept;

// Called by xr_session once the OpenXR session reports its recommended per-eye image size. Caches it (and the
// active VR mode) to the auto-res sidecar file so the NEXT launch can size the backbuffer to the headset
// rather than to the monitor. Cheap + idempotent; safe to call every frame.
void NoteHmdEyeSize(unsigned eyeW, unsigned eyeH, int vrMode) noexcept;

// Debug stereo-boundary probe: copy the current game backbuffer after each FViewportClient::Draw pass.
// pass 0 = normal Draw, pass 1 = replay Draw. Returns false if the swapchain/device is not captured yet.
bool CaptureStereoPass(int pass) noexcept;

// [SFR] allocate the pass-capture texture banks ahead of the first replay (the ~half-GB burst is a
// crash suspect when it happens mid-frame). One-shot from the Present hook; safe to call repeatedly.
bool PreallocateStereoPassTextures() noexcept;

// [SFR] game thread -> render-side capture machine: a replay was enqueued; arm pass capture for the
// next few presents. All actual copying happens on the render thread (draw/clear/present hooks).
void NotifySfrReplayEnqueued() noexcept;

// [CLEANVRCINE] cine pass-boundary mode: convo/cutscene post chains never run the gameplay composite
// shader the pass-0 boundary keys on, so cine switches the boundary to a composite-independent
// backbuffer-batch trigger. Published per frame from xr_session (Stereo 2 + cine context).
void SetSfrCineBoundary(bool on) noexcept;

// [REFLFIX] per-frame inputs for the mirror-camera rewrite: live head-look angles + the tracked
// camera world position (published from the CalcSceneView hook, main pass only).
void SetReflFixInputs(float yawRad, float pitchRad, float camX, float camY, float camZ) noexcept;

// [REFLLOG] true while MELEVR_ENABLE_REFLLOG.txt is present: the log-only reflection-coupling
// evidence pass is armed (lets the CalcSceneView publisher feed inputs in ANY vr mode, incl. Mono).
bool ReflLogArmed() noexcept;

// Returns an AddRef'd texture for the latest captured pass. Caller must Release().
bool GetStereoPassTexture(int pass, ID3D11Texture2D** outTexture, unsigned long long* outCopyCount = nullptr) noexcept;

// Returns AddRef'd textures for the latest fully published stereo pair. Caller must Release() both.
bool GetStereoPassPair(ID3D11Texture2D** outLeft, ID3D11Texture2D** outRight,
                       unsigned long long* outPairCount = nullptr,
                       unsigned long long* outPairArm = nullptr) noexcept;

// GPU-health probes for replay research. These are read-only.
void LogDeviceHealth(const char* label) noexcept;
void LogVideoMemory(const char* label, bool force = false) noexcept;

// Saves/restores the immediate D3D11 context state around experimental render replays.
bool SaveReplayD3DState() noexcept;
void RestoreReplayD3DState() noexcept;

// DIBR stereo: true when the menu toggle is on and depth capture is live.
bool IsDibrStereoReady() noexcept;
// Tell the depth-capture gate the LIVE render resolution (== backbuffer), so DIBR works at any resolution
// instead of only 2096. Called once at session start.
void SetRenderResolution(unsigned width, unsigned height) noexcept;
// Per-eye UI duplication (stereo only). SetStereoUiActive is pushed by xr_session RunFrame each frame
// (true only while SBS stereo is the active submit path); Set/GetUiDupEnabled backs the Insert-menu
// "Per-eye UI (stereo)" checkbox. Ported from ME2.
void SetStereoUiActive(bool active) noexcept;
void SetUiDupEnabled(bool enabled) noexcept;
bool GetUiDupEnabled() noexcept;
// Stereo UI vertical align: shift the letterboxed per-eye UI up(+)/down(-) as a fraction of eye height so the
// crosshair meets where shots land. 0 = centered. Pushed from xr_session each frame.
void SetStereoUiYShift(float frac) noexcept;

// Enhancements > "Depth map": gates the depth capture + greyscale viz (DIBR stereo is retired). Foundation
// for combining AER with the depth map.
void SetDepthMapEnabled(bool enabled) noexcept;
bool GetDepthMapEnabled() noexcept;
// Greyscale overlay sub-toggle (only shows when the master enable above is on).
void SetDepthVizShow(bool show) noexcept;
bool GetDepthVizShow() noexcept;
// Greyscale depth-map tuning (near/far window + flip + gamma), pushed from the menu each frame.
void SetDepthMapTuning(float nearD, float farD, bool flip, float gamma) noexcept;
// Depth-WARP effect tuning (gain=strength, convergence=zero plane, flip=sign).
void SetDibrWarp(float gain, float convergence, bool flip) noexcept;
// Milliseconds since Bink last decoded a movie frame (~0ull until a movie has ever played). Fresh (<250ms)
// + no 3D scene = a fullscreen prerendered movie is on screen.
unsigned long long LastBinkFrameAgeMs() noexcept;
// Live depth readout for the menu: center=subject, corners=walls (0 until the first probe).
void GetDepthProbe(float* center, float* tl, float* br, float* tr) noexcept;
// [RELIEF M0] quarter-center depth taps (per-eye view centers in SBS stereo; the true center is the seam).
void GetDepthProbeLR(float* leftCenter, float* rightCenter) noexcept;
// [RELIEF M0] depth-capture stats: copy size + cumulative copy count (diff two reads for copies/sec).
void GetDepthCaptureStats(unsigned* width, unsigned* height, unsigned* copies) noexcept;
// [RELIEF M0.1] per-resource depth-draw table snapshot, sorted by draw count desc. Diagnostic. Returns
// entries written (up to maxEntries).
int GetDepthDrawTableSnapshot(void** resOut, unsigned* drawsOut, int maxEntries) noexcept;
// [RELIEF M0.3] read-and-reset draw-attribution meter (total draws vs those attributed to a depth res).
void GetDrawAttribStats(unsigned* total, unsigned* attributed) noexcept;
// Paints the greyscale depth map over backbuffer 0 of the given swapchain (gated on depth-map enable +
// the greyscale toggle). The internal post-copy call at Present serves the flat lab; xr_session calls
// THIS wrapper PRE-copy in relief-probe mode so the HEADSET shows it. No-op when gates are off/not ready.
void PaintDepthVizNow(IDXGISwapChain* swapChain) noexcept;
// Renders the synthesized right eye (depth-reprojected from the finished frame) and returns it - same
// format/size as the backbuffer, so it CopyResource's into the OpenXR right-eye image. nullptr if not ready.
// Do NOT Release the returned texture (it is a persistent internal target). Call on the present thread.
ID3D11Texture2D* GetDibrRightEye(ID3D11Texture2D* backBuffer) noexcept;
ID3D11Texture2D* GetDibrLeftEye(ID3D11Texture2D* backBuffer) noexcept;
// [RELIEF M1] warp the SBS backbuffer with a per-eye depth-driven pop (added on top of real stereo);
// returns the warped SBS texture for CopySbsHalves, or nullptr when off / depth not ready.
ID3D11Texture2D* RenderReliefSbs(ID3D11Texture2D* backBuffer) noexcept;
// [RELIEF M1.3] AER: warp a full-frame eye (eyeSign +/-1). Returns the warped full frame, or nullptr.
ID3D11Texture2D* RenderReliefFull(ID3D11Texture2D* backBuffer, float eyeSign) noexcept;
// Live relief tunables from the Insert menu: strength (0=off, UV-shift scale), convergence (depth that
// stays put), sign (+/-1 flip), curve (depth response shaping).
void SetReliefWarp(float strength, float convergence, float sign, float curve, float edgeGuard, float nearFreeze,
                   float darkStrength, float darkRadius, float unsharpStrength, float unsharpRadius) noexcept;
bool IsReliefDepthReady() noexcept;
// [RELIEF] greyscale viz window auto-fit on/off (off = honor the manual near/far/gamma via SetDepthMapTuning).
void SetDepthVizAutoFit(bool on) noexcept;
bool GetDepthVizAutoFit() noexcept;
// AER combine: far-only warp of a freshly-captured eye by the LIVE coherent depth, per-eye sign (+/-1).

// Live DIBR warp tunables (set from the Insert menu). gain = depth strength, convergence = the depth that
// fuses at zero disparity (~[0.94,1.0] for LE1's reversed-Z), sign = +/-1 to flip depth direction.

}
