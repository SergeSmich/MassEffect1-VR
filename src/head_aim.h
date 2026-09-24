#pragma once

// Head-aim controller plumbing - the ONE sanctioned live-game-object write path
// (restored 2026-07-03 on top of the strip-firstperson-livecode read-only base).
//
// Minimal extraction from the retired game_camera.cpp: find + cache the live
// APlayerController (slot-identity revalidated on EVERY call), and read/write
// its ControlRotation (AActor.Rotation) under SEH. Nothing else came along -
// no weapon-mode reads, no camera snapshots, no probes, no FP machinery.
//
// Safety model (all Present-thread only, no synchronization needed):
//   - EnsureController(): fast path = cached pointer + object-table slot check
//     per call; full object-table rescan only on a cache miss. Returns true only
//     when a validated live PlayerController (with a camera) is cached.
//   - ControllerStable(): FPQUAR-pattern transition tripwire. If the resolved
//     controller pointer or slot index CHANGED since the previous call (load /
//     possession swap / death), or the controller was lost in between, it starts
//     a 90-call quarantine during which it returns false. Callers must not
//     write while false, and must re-latch any cached aim reference.
//   - Read/WriteControlRotationUU: SEH-guarded; clean no-op (false) when no
//     controller is cached or the access faults. UU = raw FRotator units
//     (65536 = 360 degrees).

#include <cstdint>

namespace MELEVR::HeadAim
{
// Find/refresh the live PlayerController. Cheap to call every frame: cached
// pointer + slot-identity check; rescans the object table only on a miss.
bool EnsureController() noexcept;

// Transition tripwire (call once per frame AFTER EnsureController succeeds).
// False while the 90-call post-change quarantine runs, or when no controller
// is cached. While false: NO writes, abandon cached aim references.
bool ControllerStable() noexcept;

// Read/write the cached controller's ControlRotation in raw FRotator units.
bool ReadControlRotationUU(int32_t* pitchUU, int32_t* yawUU, int32_t* rollUU) noexcept;
bool WriteControlRotationUU(int32_t pitchUU, int32_t yawUU, int32_t rollUU) noexcept;

// Weapons-out classifier (active camera-mode class name). 1 = weapon out (Combat/TightAim/HipAimCover/Sniper),
// 0 = explore/unarmed, -1 = unreadable. Gates head-AIM (weapon out) vs head-LOOK 6DOF (unarmed). Pure read.
int ReadWeaponModeSEH() noexcept;

// [NOLETTERBOX] clear ACamera.bConstrainAspectRatio for this frame (game thread, pre-Draw).
// 1 = cleared, 0 = was already clear, -1 = unreadable. See head_aim.cpp for the race history.
int ClearCameraAspectConstraintSEH() noexcept;

// Active camera-mode CLASS name (e.g. BioCameraBehaviorConversation, SFXCameraMode_Cinematic). For convo/
// cutscene detection so the flat-mono cine policy engages. Pure read; false if none.
bool GetCameraModeNameSEH(char* out, int cap) noexcept;

// (GetPawnWorldAndYawSEH + SetCineFpHide removed 2026-07-18 with CINEFP - cine is flat, permanently.)

// Stage 2 discovery, read-only: logs the live pawn/mesh attachment and component
// graph so the weapon/arms object can be identified before any new write path is
// considered. No game memory is modified.
void ProbeWeaponGraph() noexcept;

// Engine game-mode context (USFXGameModeManager::CurrentMode / EGameModes). 7 = GUI full-screen menu.
// Returns the byte 0..12, or -1 if unreadable. Used to force menus to flat mono in VR. Pure read.
int ReadGameModeSEH() noexcept;

// [LIVEGUI2] World-state telemetry for the gameMode-7 disambiguation (in-world GUI overlays like the
// Eden Prime bomb disarm report 7 exactly like the pause/front-end menus). Measured facts so far
// (2026-07-17): under the in-game ESC menu Pauser stays NULL, TimeSeconds keeps ADVANCING, and the
// world keeps rendering (SFR pairs kept streaming) - so pawn/pauser/clock CANNOT separate menu from
// minigame. This wider snapshot (dilation, per-tick delta, bPlayersOnly) is logged during every mode-7
// episode until the real discriminator is proven from data. Pure SEH-guarded reads, fail-safe false.
struct WorldTelemetry
{
    bool hasPawn;
    bool pauserSet;
    bool playersOnly;
    bool playersOnlyPending;
    float timeDilation;
    float timeSeconds;
    float realTimeSeconds;
    float deltaSeconds;
};
bool ReadWorldTelemetrySEH(WorldTelemetry* out) noexcept;

// Cinematic letterbox detector (pure read): 1 = camera aspect-constrained (fills *aspectOut, e.g.
// 1.7778), 0 = unconstrained, -1 = unreadable. The submit side crops the letterbox band when set.

// Mako turret READ-ONLY probes: [MAKOTURRET] = the appearance runtime-info (dead end, kept); [MAKOBONE] =
// the bone controller (pawn.Mesh->AnimTree->SkelControls, matches "Turret"), logs BoneRotation+units ~2/s.
void ProbeMakoTurret() noexcept;
void ProbeMakoTurretBone() noexcept;

// Write head yaw/pitch into the Mako turret bone controllers (the deterministic follow). Returns controls
// written (0 = reach failed). Call every frame while head-aiming the Mako.
int DriveMakoTurretBone(float headYawDeg, float headPitchDeg) noexcept;

// Mako head-aim (step 2, experimental): active=true writes head yaw/pitch (deg) to turret[0] + clears
// auto-track + logs readback; active=false restores auto-track once (edge-tracked stand-down).
void DriveMakoTurret(bool active, float headYawDeg, float headPitchDeg) noexcept;

// First-person camera tick (M1): owns the active SFXCameraMode's eye offset + native bFirstPerson flag
// for the four on-foot states (Explore/ExploreStorm/Combat/CombatStorm). Call once per frame with the
// frame's EnsureController/ControllerStable results; writes only when both true. fpEnabled off ->
// slot-liveness-gated restore-once. OwnsAny: true while any mode still carries injected offsets (keeps the
// tick reachable for that restore even with the feature off).
void FirstPersonTick(bool ctrlLive, bool ctrlStable, int gameMode) noexcept;
bool FirstPersonOwnsAny() noexcept;
}
