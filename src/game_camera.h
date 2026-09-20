#pragma once

// Milestone 1 - minimal live camera access for the clean rebuild.
//
// ONE clean path to the LE1 player camera, nothing more. Finds the live
// APlayerController by scanning the engine object table, caches it (with
// slot-identity revalidation so a destroyed/recycled controller is re-found),
// and exposes exactly what M1 needs:
//   - the controller handle (refreshed cheaply every frame),
//   - the active camera-mode class name as the "weapons out" gate,
//   - read/write of the control rotation (AActor.Rotation) in raw FRotator units.
//
// Every game-memory access is SEH-guarded and address-validated: a clean no-op
// (returns false / -1) if the controller isn't live yet or a slot was recycled.

#include <cstdint>

namespace MELEVR::GameCamera
{
struct CameraSnapshot
{
    bool valid = false;
    void* controller = nullptr;
    void* camera = nullptr;
    void* cameraMode = nullptr;
    char cameraModeName[128] = {};
    char cameraModeInstance[128] = {};  // mode OBJECT name (PowerCover0, CombatCam0, BlindLeftStand0...) - cover vs combat differ by THIS, not class
    float controllerFovDeg = 0.0f;
    float modeFovDeg = 0.0f;
    float modeOffsetX = 0.0f;
    float modeOffsetY = 0.0f;
    float modeOffsetZ = 0.0f;
    int32_t controlPitchUU = 0;
    int32_t controlYawUU = 0;
    int32_t controlRollUU = 0;
    bool combatLike = false;
    bool tightAimLike = false;
    bool transitionLike = false;
    bool inCover = false;   // pawn cover-state (kPawnCoverState) - the real switch that splits cover from open combat
    uint8_t coverType   = 0;  // pawn CoverType byte (0xA55) ECoverType: 1=Standing(high), 2=MidLevel, 3=LowLevel (crouched)
    uint8_t coverAction = 0;  // pawn CoverAction byte (0xA56) ECoverAction: 0=idle, 1/2/8=blind, 3/4/7/13=lean/aim, 9/10/11=peek
    uint32_t coverProbe[4] = {};  // TEMP diagnostic: raw pawn values at 0x9A8/0xA4C/0x9B0/0x950 to confirm the cover flag live
};

// Find/refresh the live PlayerController. Cheap to call every frame: it keeps a
// cache and only rescans the object table if the cached controller has died.
// Returns true when a valid controller (with a populated camera) is cached.
bool EnsureController() noexcept;

// Weapons-out gate: reads the active camera mode (camera + kCameraCurrentMode)
// class name. Returns 1 = weapons out (name contains "Combat"/"TightAim"),
// 0 = explore/other, -1 = unreadable. Optionally copies the mode class name out.
int ReadWeaponMode(char* nameOut, int nameCap) noexcept;

// Read/write the cached controller's CONTROL rotation (AActor.Rotation) in raw
// UE FRotator units (65536 = 360 deg). The write injects head rotation as if it
// were look-stick input: the engine builds render + cull + occlusion from this
// one rotation. SEH-guarded; clean no-op (false) if no controller is cached or
// the access faults.
bool ReadControlRotationUU(int32_t* pitchUU, int32_t* yawUU, int32_t* rollUU) noexcept;
bool WriteControlRotationUU(int32_t pitchUU, int32_t yawUU, int32_t rollUU) noexcept;

// Read-only SFX camera probe. This owns nothing yet; it shows what the game
// camera pipeline is doing before camera modes/collision get patched for real FPS.
bool ReadCameraSnapshot(CameraSnapshot* out) noexcept;
void TickSfxCameraProbe(uint64_t presentId, bool flatProbeMode) noexcept;

// Flat-only write probe. Marker-gated by the caller. Writes test first-person
// offsets into gameplay SFXCameraMode objects and makes Interpolate inherit the
// last stable owned offset. Restores touched objects when disabled.
void TickSfxCameraOwnershipProbe(uint64_t presentId, bool flatProbeMode, bool ownershipEnabled) noexcept;

// Flat-only prewarm probe. Scans the object table for SFXCameraMode objects and
// owns them before they become active, so first-use transitions do not blend
// toward unmodified destination modes.
void PrewarmSfxCameraOwnershipProbe(uint64_t tickId, bool flatProbeMode, bool enabled) noexcept;

// Read-only: dump every camera-mode CLASS DEFAULT (Default__*) offset/fov to camera_defaults.csv. Finds all
// ~25 camera modes from their startup CDOs - no gameplay needed. Never writes the camera.
void ExtractCameraModeDefaults() noexcept;

// First-person pawn/gun visibility guard: MassFPS-style protection against the
// camera collision path adding the possessed pawn and attached actors to
// PlayerController.HiddenActors while tuning the camera into first person.
bool ApplyFirstPersonPawnVisibilityGuard(bool enabled) noexcept;

// First-person visibility probe: hides one material section on the possessed
// pawn's primary mesh while enabled, and restores it when disabled/changed.
bool ApplyFirstPersonHeadHide(bool enabled, int componentIndex) noexcept;

// First-person body-hide (Shepard's body skeletal mesh ONLY) and weapon-hide (the equipped weapon +
// Pawn.Attached[] actor components ONLY) - independent toggles via SetOwnerNoSee, for a clean "just the
// crosshair" cover view. Each tracks its own components and restores on disable.
bool ApplyFirstPersonBodyHide(bool enabled) noexcept;
bool ApplyFirstPersonWeaponHide(bool enabled) noexcept;

// Active first-person state's camera view ANGLE (pitch/yaw in degrees), published
// each tick by TickSfxCameraOwnershipProbe. 0 when not in a first-person state.
// Consumed by the render-side head-look hook to aim the view over/around a wall.
void GetActiveFpViewAngleDeg(float* pitchDeg, float* yawDeg) noexcept;
}
