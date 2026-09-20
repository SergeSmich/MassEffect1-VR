#pragma once

#include <cstdint>

// ============================================================================
// MELE VR - CLEAN REBUILD, Milestone 1a: head-look render hook.
//
// ONE MinHook on the game's CalcSceneView (the function that builds each frame's FSceneView, the
// view+projection the GPU renders with). Each render, AFTER the game builds its view, this rotates that
// view by the head's yaw/pitch delta - camera-only, so the pawn never turns and the third-person rig
// never orbits (this is the decoupled "free look", NOT the character-coupled ControlRotation path).
// The rotation recomputes the view-projection products so CULLING follows the look (no pop-in).
//
// This is the ONLY game-side hook in M1a. No AER, no eye shift, no FOV fill, no positional lean (that's
// M1b), no camera-mode reads. The proven mechanism, lifted clean and stripped to just the head rotation.
//
// Offsets are LE1 build v2.0.0.48602 facts, validated at runtime (SEH-guarded, clean no-op on fault).
// ============================================================================

namespace MELEVR::RenderHook
{
struct RenderStamp
{
    uint64_t seq = 0;
    int eye = -1;
    bool aer = false;
};

// Install the MinHook on CalcSceneView. Idempotent; safe to call every Present (no-op once installed).
bool Install() noexcept;
bool Installed() noexcept;

// Per-frame head-look: the yaw/pitch delta to rotate the rendered view by, in UE FRotator units
// (65536 = 360 deg), ALREADY SIGNED by the caller. on=false is a clean no-op (game renders untouched).
void SetHeadLook(int32_t yawUU, int32_t pitchUU, bool on) noexcept;

// The head-look yaw currently APPLIED to the rendered view (UU, positive = view turned LEFT), 0 when
// head-look is off (menus, cine, Mako, weapon-out head-aim). This is the exact angle the view is rotated
// away from the game's movement basis (ControlRotation), so the XInput hook rotates the movement stick
// by it to make the character run where the head is looking ([MOVEFIX], the FP-storm inversion fix).
int32_t HeadLookYawUU() noexcept;
// Same for the applied head-look pitch (UU, positive = view turned UP). [AIMSEED] reads both at the
// look->aim handoff so the aim injection starts equal to the look offset = zero view jump.
int32_t HeadLookPitchUU() noexcept;

// (CINEFP + CINEDOLLY removed 2026-07-18: conversations/cutscenes are flat, permanently.)

// [INVMAT] gated candidate fix for the head-locked dark-panel bug: after the CalcSceneView edits,
// recompute the FSceneView inverse-matrix slots the one-shot scan validated, so screen->world
// reconstruction (height fog, light shafts) agrees with the edited camera. OFF = probes only run.
void SetInverseFix(bool enabled) noexcept;

// The half-angles (radians) of the FOV the game ACTUALLY rendered this frame, read from the live
// projection matrix (xScale/yScale). xr_session declares a matching projection-layer FOV so the
// submitted view is geometrically exact. Returns false until the first render has been seen.
bool GetRenderFovHalfAngles(float* halfHorizRad, float* halfVertRad) noexcept;

// Last raw game projection seen before the target-FOV override. Useful for classifying cinematic
// paths that do not expose a named camera mode but still render with a native 16:9 frustum.
bool GetLastRawProjection(float* aspect, float* halfHorizRad, float* halfVertRad) noexcept;

// Optional render-side projection override. When enabled, CalcSceneView rewrites the game's perspective
// projection matrix to these symmetric half-angles and recomputes ViewProjection products. This must match
// the FOV submitted to OpenXR; otherwise stereo/AER will not fuse.
void SetTargetFovHalfAngles(float halfHorizRad, float halfVertRad, bool enabled) noexcept;

// Static camera-view offset in UE units, view-relative: right(+)/up(+)/forward(+). Applied in
// CalcSceneView as a PreViewTranslation so the GAME re-renders from the shifted viewpoint (a real
// camera move with parallax) - NOT a slid flat image. All-zero is a clean no-op.
void SetViewOffset(float rightUU, float upUU, float fwdUU) noexcept;
void SetViewOffsetEye(float rightUU, float upUU, float fwdUU, int eye, bool aer) noexcept;
// [AERFULL-P1OWN] ME2/ME3-style direct AER ownership. The Present thread arms the next eye and
// effective half-separation; only the cached primary ULocalPlayer CalcSceneView may consume/stamp it.
void SetAerState(bool enabled, int renderEye, float halfEyeUU, bool swapEyes) noexcept;
void SetSyncStereoReplay(bool enabled, float replayShiftRightUU) noexcept;
void SetP1LayoutStereo(bool enabled, float halfEyeUU, bool swapEyes, float eyeAspect, float backbufferAspect) noexcept;
// [SFR] EGameModes byte (0..4 gameplay, 5 convo, 6 cine, 7 menu, 8 movie, 9 galaxy, -1 unreadable),
// published per frame from xr_session's existing read; gates the same-frame double render.
// allowConvo (= cfg.cineVrConvo) / allowCutscene (= cfg.cineVrCutscene) [CINEVR2]: each context
// opts its game mode (5 / 6) into the double render. Movie 8 / galaxy 9 stay excluded (the 16:31
// crash territory).
// liveGui7 [LIVEGUI]: xr_session's per-present verdict that a mode-7 "menu" is really a live in-world
// GUI overlay (possessed pawn + world unpaused + sim clock advancing - the Eden Prime bomb disarm);
// only then does mode 7 replay. Static menus keep the full exclusion.
void PublishGameModeForSfr(int gameMode, bool allowConvo, bool allowCutscene, bool liveGui7) noexcept;
// [SFR] Stereo 2 mode master switch (vrMode 4) + the symmetric per-eye half separation (same tuned
// stereoHalfEyeUU regular Stereo uses), published per frame from xr_session.
void SetSfrEnabled(bool enabled, float halfEyeUU) noexcept;
// [SFRCONV] Stereo 2 convergence: opposite per-eye off-axis projection shift that pulls the fusion plane in
// from infinity. Published per frame from xr_session (cfg.sfr2Convergence). 0 = fusion at infinity.
void SetSfrConvergence(float convergence) noexcept;
// [NOLETTERBOX] arm/disarm the pre-Draw clear of LE1's cine 16:9 aspect constraint.
void SetCineUnconstrain(bool on) noexcept;
// [POSEEXACT] arm counter (bumped per present by SetHeadLook) and the arm the live render latched.
uint64_t GetHeadArmSeq() noexcept;
uint64_t GetPairArmSeq() noexcept;
// [CINETILT] last cine camera tilt (deg) + max per-frame camera basis rotation since the previous
// call (deg, reset on read). Feeds the [CINEJIT] per-second line.
void ReadCineCamTelemetry(float& tiltDeg, float& camStepDegMax) noexcept;
// [LOOKSTEP] head-look (UU) the main pass latched for the pair currently being rendered.
void GetPairLookUU(int& yawUU, int& pitchUU) noexcept;
bool GetLastRenderStamp(RenderStamp* out) noexcept;
// [AERSHAKE] FIFO stamp consume: oldest build after lastSeq, one per present - label matches pixels.
bool ConsumeRenderStamp(unsigned long long lastSeq, unsigned long long* outSeq, int* outEye, bool* resynced) noexcept;
void NotifyPresentTick() noexcept;
// Presents since the last 3D perspective view was rendered. 0 = a 3D view rendered this present; a growing
// value = flat 2D on screen (prerendered movie / loading / main menu) -> caller can force mono.
uint64_t PresentsSinceLastPerspective() noexcept;

// The game's base camera world position (ViewOrigin before the applied offset), for the AER comfort motion score.
// Returns false until the first main view has been seen.
bool GetBaseViewOrigin(float* x, float* y, float* z) noexcept;

// Last main-view right axis in world/UE units, read from FSceneView::ViewMatrix.
// This is the same basis used by the proven view-relative AER offset path.
bool GetLastViewRight(float* x, float* y, float* z) noexcept;
}
