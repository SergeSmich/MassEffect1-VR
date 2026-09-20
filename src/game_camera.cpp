#include "game_camera.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "le1_game.h"
#include "logger.h"
#include "vr_config.h"

// All functions here run on the game's Present thread only (called from the XR
// present path), so the cache needs no synchronization.

using MELEVR::Logger::LogLine;

namespace
{
// ---- low-level safety -------------------------------------------------------

bool IsReadableAddress(const void* address, size_t byteCount) noexcept
{
    if (address == nullptr || byteCount == 0)
    {
        return false;
    }
    MEMORY_BASIC_INFORMATION mi = {};
    if (VirtualQuery(address, &mi, sizeof(mi)) == 0 ||
        mi.State != MEM_COMMIT ||
        (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
    {
        return false;
    }
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    const auto regionStart = reinterpret_cast<std::uintptr_t>(mi.BaseAddress);
    const auto regionEnd = regionStart + mi.RegionSize;
    return value >= regionStart && byteCount <= (regionEnd - value);
}

// UObjects are 16-aligned canonical user-mode heap allocations; recycled table
// slots have produced misaligned/noncanonical "objects" that AV on deref.
bool PointerLooksCanonicalAligned(const void* p) noexcept
{
    const std::uintptr_t bits = reinterpret_cast<std::uintptr_t>(p);
    return p != nullptr && (bits & 0xF) == 0 && bits < 0x0000800000000000ull;
}

void SafeCopyCString(char* dst, size_t cap, const char* src) noexcept
{
    if (dst == nullptr || cap == 0)
    {
        return;
    }
    if (src == nullptr)
    {
        dst[0] = '\0';
        return;
    }
    size_t i = 0;
    for (; src[i] != '\0' && i + 1 < cap; ++i)
    {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

// POD-only snapshot of the fields used to classify a controller candidate.
struct ControllerInfo
{
    char className[64];
    char objectName[64];
    void* camera;
    float fovAngle;
    bool ok;
};

struct OwnedModeState
{
    void* mode = nullptr;
    float originalX = 0.0f;
    float originalY = 0.0f;
    float originalZ = 0.0f;
    std::int32_t tableIndex = -1;  // object-table slot captured at own-time (-1 = unknown/tick-owned).
                                   // Restore-liveness key: only write back if the slot still holds this ptr.
    char className[64] = {};       // mode class -> look up TRUE vanilla from the class-default cache at restore.
    bool valid = false;
};

struct OwnedOffset
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// ---- SEH-guarded reads (POD locals only; no C++ unwinding inside __try) ------

bool ReadObjectSlotSEH(void* const* data, std::int32_t index, void** objOut) noexcept
{
    __try
    {
        *objOut = data[index];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *objOut = nullptr;
        return false;
    }
}

bool InspectObjectSEH(void* obj, ControllerInfo* info) noexcept
{
    __try
    {
        void* classObj = MELEVR::LE1::ReadPtr(obj, MELEVR::LE1::kUObjectClass);
        SafeCopyCString(info->className, sizeof(info->className), MELEVR::LE1::ObjectName(classObj));
        SafeCopyCString(info->objectName, sizeof(info->objectName), MELEVR::LE1::ObjectName(obj));
        info->camera = MELEVR::LE1::ReadPtr(obj, MELEVR::LE1::kPlayerCameraPtr);
        info->fovAngle = MELEVR::LE1::ReadF32(obj, MELEVR::LE1::kPlayerFOVAngle);
        info->ok = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        info->ok = false;
        return false;
    }
}

bool ReadControlRotationSEH(void* controller, std::int32_t* pitch, std::int32_t* yaw, std::int32_t* roll) noexcept
{
    __try
    {
        *pitch = MELEVR::LE1::ReadI32(controller, MELEVR::LE1::kActorRotation + MELEVR::LE1::kRotPitch);
        *yaw = MELEVR::LE1::ReadI32(controller, MELEVR::LE1::kActorRotation + MELEVR::LE1::kRotYaw);
        *roll = MELEVR::LE1::ReadI32(controller, MELEVR::LE1::kActorRotation + MELEVR::LE1::kRotRoll);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool WriteControlRotationSEH(void* controller, std::int32_t pitch, std::int32_t yaw, std::int32_t roll) noexcept
{
    __try
    {
        BYTE* base = reinterpret_cast<BYTE*>(controller) + MELEVR::LE1::kActorRotation;
        *reinterpret_cast<std::int32_t*>(base + MELEVR::LE1::kRotPitch) = pitch;
        *reinterpret_cast<std::int32_t*>(base + MELEVR::LE1::kRotYaw) = yaw;
        *reinterpret_cast<std::int32_t*>(base + MELEVR::LE1::kRotRoll) = roll;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Weapons-out classifier: camera mode class name contains "Combat"/"TightAim".
// Returns 1 = out, 0 = explore/other, -1 = unreadable. POD-only inside __try.
int ReadWeaponModeSEH(void* controller, char* nameOut, int nameCap) noexcept
{
    __try
    {
        if (nameOut != nullptr && nameCap > 0)
        {
            nameOut[0] = '\0';
        }
        void* camera = MELEVR::LE1::ReadPtr(controller, MELEVR::LE1::kPlayerCameraPtr);
        if (camera == nullptr) return -1;
        void* mode = MELEVR::LE1::ReadPtr(camera, MELEVR::LE1::kCameraCurrentMode);
        if (mode == nullptr) return -1;
        const char* cls = MELEVR::LE1::ClassName(mode);
        if (cls == nullptr) return -1;

        char tmp[64];
        int i = 0;
        for (; i + 1 < static_cast<int>(sizeof(tmp)) && cls[i] != '\0'; ++i) tmp[i] = cls[i];
        tmp[i] = '\0';
        if (i == 0) return -1;
        if (nameOut != nullptr && nameCap > 0)
        {
            int j = 0;
            for (; j + 1 < nameCap && tmp[j] != '\0'; ++j) nameOut[j] = tmp[j];
            nameOut[j] = '\0';
        }
        // HipAimCover = blind-fire-over-the-top from cover; SniperAim = scoped sniper. Both ARE weapon-out, but their
        // names have neither "Combat" nor "TightAim", so they were misclassified as not-weapon-out -> the head drove
        // the VIEW (crosshair) instead of the AIM (weapon) -> the gun didn't track (corner blind fire = Combat, ADS =
        // TightAim, which DID track). SniperAim down-the-sights now tracks the head like regular ADS.
        const bool combat = (std::strstr(tmp, "Combat") != nullptr) || (std::strstr(tmp, "TightAim") != nullptr) ||
                            (std::strstr(tmp, "HipAimCover") != nullptr) || (std::strstr(tmp, "Sniper") != nullptr);
        return combat ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

void* ReadPossessedPawnSEH(void* controller) noexcept;  // fwd decl (defined below) - the exact pawn the cover finder read

bool ReadCameraSnapshotSEH(void* controller, MELEVR::GameCamera::CameraSnapshot* out) noexcept
{
    __try
    {
        *out = MELEVR::GameCamera::CameraSnapshot{};
        out->controller = controller;
        out->camera = MELEVR::LE1::ReadPtr(controller, MELEVR::LE1::kPlayerCameraPtr);
        if (out->camera == nullptr) return false;

        out->controllerFovDeg = MELEVR::LE1::ReadF32(controller, MELEVR::LE1::kPlayerFOVAngle);
        out->cameraMode = MELEVR::LE1::ReadPtr(out->camera, MELEVR::LE1::kCameraCurrentMode);
        if (out->cameraMode == nullptr) return false;

        const char* modeName = MELEVR::LE1::ClassName(out->cameraMode);
        if (modeName == nullptr || modeName[0] == '\0') return false;
        SafeCopyCString(out->cameraModeName, sizeof(out->cameraModeName), modeName);
        const char* modeInst = MELEVR::LE1::ObjectName(out->cameraMode);  // instance name: PowerCover0 vs CombatCam0 (same class)
        SafeCopyCString(out->cameraModeInstance, sizeof(out->cameraModeInstance), modeInst != nullptr ? modeInst : "");

        out->modeFovDeg = MELEVR::LE1::ReadF32(out->cameraMode, MELEVR::LE1::kSFXCameraModeFOV);
        out->modeOffsetX = MELEVR::LE1::ReadF32(out->cameraMode, MELEVR::LE1::kSFXCameraModeOffset + 0);
        out->modeOffsetY = MELEVR::LE1::ReadF32(out->cameraMode, MELEVR::LE1::kSFXCameraModeOffset + 4);
        out->modeOffsetZ = MELEVR::LE1::ReadF32(out->cameraMode, MELEVR::LE1::kSFXCameraModeOffset + 8);
        out->controlPitchUU = MELEVR::LE1::ReadI32(controller, MELEVR::LE1::kActorRotation + MELEVR::LE1::kRotPitch);
        out->controlYawUU = MELEVR::LE1::ReadI32(controller, MELEVR::LE1::kActorRotation + MELEVR::LE1::kRotYaw);
        out->controlRollUU = MELEVR::LE1::ReadI32(controller, MELEVR::LE1::kActorRotation + MELEVR::LE1::kRotRoll);

        // Pawn cover-state: the real switch for cover (the camera mode can't tell cover from open combat, since
        // both reuse one per-class camera object). Read the pawn flags word at kPawnCoverState and test the
        // cover mask - set whenever pinned/aiming/firing from cover, clear in the open. Independent of the
        // camera offset, so it works with FP ownership on.
        void* coverPawn = ReadPossessedPawnSEH(controller);  // same pawn (0x3DC, else AcknowledgedPawn 0x6C0) the finder used
        const std::uint32_t coverFlags = (coverPawn != nullptr) ? MELEVR::LE1::ReadU32(coverPawn, MELEVR::LE1::kPawnCoverState) : 0u;
        out->inCover = (coverPawn != nullptr) && ((coverFlags & MELEVR::LE1::kPawnCoverMask) != 0u);
        out->coverType   = (coverPawn != nullptr) ? static_cast<std::uint8_t>((coverFlags >> 8)  & 0xFFu) : 0u;  // 0xA55 ECoverType (1=Standing,2=Mid,3=Low)
        out->coverAction = (coverPawn != nullptr) ? static_cast<std::uint8_t>((coverFlags >> 16) & 0xFFu) : 0u;  // 0xA56 ECoverAction
        out->coverProbe[0] = coverFlags;                                // raw pawn[0xA54] (cov9A8 in the log)
        out->coverProbe[1] = coverFlags & MELEVR::LE1::kPawnCoverMask;  // masked cover bits (A4C in the log)

        out->combatLike = (std::strstr(out->cameraModeName, "Combat") != nullptr);
        out->tightAimLike = (std::strstr(out->cameraModeName, "TightAim") != nullptr);
        out->transitionLike = (std::strstr(out->cameraModeName, "Interpolate") != nullptr) ||
                              (std::strstr(out->cameraModeName, "Transition") != nullptr);
        out->valid = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *out = MELEVR::GameCamera::CameraSnapshot{};
        return false;
    }
}

bool WriteSfxCameraModeOffsetSEH(void* mode, float x, float y, float z) noexcept
{
    if (mode == nullptr) return false;
    __try
    {
        BYTE* base = reinterpret_cast<BYTE*>(mode) + MELEVR::LE1::kSFXCameraModeOffset;
        *reinterpret_cast<float*>(base + 0) = x;
        *reinterpret_cast<float*>(base + 4) = y;
        *reinterpret_cast<float*>(base + 8) = z;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Flip the GAME's native bFirstPerson bit on a camera mode. ON = game renders first person itself; OFF = the
// mode's normal third-person camera, untouched (Offset is never written here, so there is nothing to restore).
bool WriteSfxCameraModeFirstPersonSEH(void* mode, bool on) noexcept
{
    if (mode == nullptr) return false;
    __try
    {
        std::uint32_t* flags = reinterpret_cast<std::uint32_t*>(
            reinterpret_cast<BYTE*>(mode) + MELEVR::LE1::kSFXCameraModeFlags);
        if (on) *flags |= MELEVR::LE1::kSFXCameraModeFirstPersonMask;
        else    *flags &= ~MELEVR::LE1::kSFXCameraModeFirstPersonMask;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadSfxCameraModeObjectSEH(void* obj, char* className, size_t classCap, char* objectName, size_t objectCap,
                                float* x, float* y, float* z) noexcept
{
    if (obj == nullptr || className == nullptr || objectName == nullptr || x == nullptr || y == nullptr || z == nullptr)
    {
        return false;
    }
    __try
    {
        void* classObj = MELEVR::LE1::ReadPtr(obj, MELEVR::LE1::kUObjectClass);
        SafeCopyCString(className, classCap, MELEVR::LE1::ObjectName(classObj));
        SafeCopyCString(objectName, objectCap, MELEVR::LE1::ObjectName(obj));
        *x = MELEVR::LE1::ReadF32(obj, MELEVR::LE1::kSFXCameraModeOffset + 0);
        *y = MELEVR::LE1::ReadF32(obj, MELEVR::LE1::kSFXCameraModeOffset + 4);
        *z = MELEVR::LE1::ReadF32(obj, MELEVR::LE1::kSFXCameraModeOffset + 8);
        return className[0] != '\0' && objectName[0] != '\0';
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Map an SFXCameraMode class name to its First-Person STATE config (Insert > First Person tab), or nullptr if
// the mode is not an on-foot first-person state - Vehicle (Mako) / Photo / Cinematic / Conversation /
// transitions all return nullptr and stay third-person. More specific names are checked first.
const MELEVR::Config::FpState* FpStateForModeName(const char* cls, const char* inst, bool inCover,
                                                  int coverType, int coverAction) noexcept
{
    if (cls == nullptr) return nullptr;
    if (std::strstr(cls, "Vehicle") != nullptr) return nullptr;  // Mako -> 3rd person (excludes VehicleTightAim*)
    const MELEVR::Config::VrConfig& c = MELEVR::Config::Get();
    (void)inst;
    // SNIPER SCOPE only gets its own eye when NOT in cover (free-standing). Sniping FROM cover falls through to the
    // regular cover routing below - dedicated cover-sniper states didn't help, regular cover aim is fine.
    if (!inCover && std::strstr(cls, "SniperAim") != nullptr) return &c.fpSniper;
    // COVER: the camera mode reuses one per-class object (Combat/TightAim) for both cover and open combat, so the
    // SWITCH is the pawn cover-state, NOT the mode name. In cover the precise sub-state is the pawn's ECoverType
    // (0xA55: 1=Standing, 2=MidLevel, 3=LowLevel) + ECoverAction (0xA56). Crouched (mid/low) cover gets its own
    // eye; standing cover splits by ACTION: lean/popup/aimback = aim down sights, blind/peek = fire, else hug.
    if (inCover)
    {
        const bool crouched = (coverType >= 2);  // CT_MidLevel / CT_LowLevel = crouched in low cover (own eye height)
        switch (coverAction)
        {
            case 7:                                                     // CA_PopUp = pop up + aim down sights OVER the top
                return crouched ? &c.fpCrouchAimAbove : &c.fpCoverAimAbove;
            case 3: case 4: case 13:                                    // CA_Lean L/R, CA_Aimback = aim down sights, the corner
                return crouched ? &c.fpCrouchAim : &c.fpCoverAim;
            case 8: case 11:                                            // CA_BlindUp, CA_PeekUp = blind fire OVER the top (stand up)
                return crouched ? &c.fpCrouchFireAbove : &c.fpCoverFireAbove;
            case 1: case 2:                                             // CA_Blind L/R = blind fire AROUND the corner
            case 9: case 10:                                            // CA_Peek  L/R = peek  fire AROUND the corner
                return crouched ? &c.fpCrouchFire : &c.fpCoverFire;
            default: break;                                             // CA_Default/Step/SwatTurn -> class fallback, then hug
        }
        if (std::strstr(cls, "HipAimCover") != nullptr) return crouched ? &c.fpCrouchFire : &c.fpCoverFire;
        if (std::strstr(cls, "TightAim")    != nullptr) return crouched ? &c.fpCrouchAim  : &c.fpCoverAim;
        return crouched ? &c.fpCrouch : &c.fpCover;  // pinned/hug
    }
    if (std::strstr(cls, "CombatStorm")  != nullptr) return &c.fpSprintCombat;  // sprint with weapon out
    if (std::strstr(cls, "ExploreStorm") != nullptr) return &c.fpSprint;        // sprint in exploration
    if (std::strstr(cls, "SniperAim")   != nullptr) return &c.fpSniper;
    if (std::strstr(cls, "TightAim")    != nullptr) return &c.fpAim;
    if (std::strstr(cls, "HipAimCover") != nullptr) return &c.fpCoverFire;
    if (std::strstr(cls, "EnterCover")  != nullptr) return &c.fpCover;
    if (std::strstr(cls, "Combat")      != nullptr) return &c.fpCombat;
    if (std::strstr(cls, "Explore")     != nullptr) return &c.fpExplore;
    return nullptr;
}

// Through a cover transition (interpolate / grace-hold) the camera mode is briefly not a stable FP mode, so the
// tick inherits the LAST STABLE state. But the pawn's cover flag (0xA54) flips to in-cover before the camera
// settles, and on a combat->cover ENTER the last stable state is Combat (hideBody=false) - which left the whole
// body visible through the enter. If the pawn is already in cover, resolve the hide from the LIVE cover state so
// body/head hide snap to cover immediately; otherwise inherit the last stable state as before.
const MELEVR::Config::FpState* ResolveTransitionHoldState(const MELEVR::GameCamera::CameraSnapshot& before,
                                                          const MELEVR::Config::FpState* lastStable) noexcept
{
    if (before.inCover)
    {
        const MELEVR::Config::FpState* cov = FpStateForModeName(before.cameraModeName, before.cameraModeInstance,
                                                                before.inCover, before.coverType, before.coverAction);
        if (cov != nullptr) return cov;
    }
    return lastStable;
}

bool IsOwnableStableSfxMode(const MELEVR::GameCamera::CameraSnapshot& snap) noexcept
{
    if (!snap.valid || snap.cameraMode == nullptr) return false;
    if (snap.transitionLike) return false;
    const MELEVR::Config::FpState* fp = FpStateForModeName(snap.cameraModeName, snap.cameraModeInstance, snap.inCover, snap.coverType, snap.coverAction);
    return fp != nullptr && fp->on;   // own only on-foot states whose first person is enabled (mix-aware)
}

bool IsOwnableStableSfxModeClass(const char* className, const char* instanceName) noexcept
{
    const MELEVR::Config::FpState* fp = FpStateForModeName(className, instanceName, false, 0, 0);
    return fp != nullptr && fp->on;
}

OwnedOffset StableOwnedOffsetForMode(const MELEVR::GameCamera::CameraSnapshot& snap) noexcept
{
    // The active state's first-person eye offset from the menu (X=fwd, Y=right, Z=up). Falls back to the mode's
    // current offset only if it has no state config (shouldn't happen for an owned mode).
    const MELEVR::Config::FpState* fp = FpStateForModeName(snap.cameraModeName, snap.cameraModeInstance, snap.inCover, snap.coverType, snap.coverAction);
    if (fp != nullptr)
    {
        // Left/right cover are mirror images. Each cover state is tuned for the LEFT side; for a RIGHT lean/blind/peek
        // (CA_*Right = 2/4/10) mirror the lateral (Y) offset so the camera sits on the correct side of the wall instead
        // of pushing Shepard's weapon into the cover. X (fwd) and Z (up) are unchanged.
        const bool mirrorRight = snap.inCover &&
            (snap.coverAction == 2 || snap.coverAction == 4 || snap.coverAction == 10);
        return { fp->x, mirrorRight ? -fp->y : fp->y, fp->z };
    }
    return { snap.modeOffsetX, snap.modeOffsetY, snap.modeOffsetZ };
}

bool ObjectTableSlotHoldsSEH(std::int32_t index, const void* expected) noexcept
{
    __try
    {
        MELEVR::LE1::TArrayHeader* objects = MELEVR::LE1::Objects();
        if (objects == nullptr || objects->data == nullptr || index < 0 || index >= objects->count)
        {
            return false;
        }
        return objects->data[index] == expected;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// ---- classification ---------------------------------------------------------

bool LooksLikePlayerController(const char* className) noexcept
{
    return className != nullptr && std::strstr(className, "PlayerController") != nullptr;
}

// Class-default / archetype ("Default__BioPlayerController") objects exist from
// startup with a null camera and shadow the live instance.
bool IsTemplateObjectName(const char* objectName) noexcept
{
    return objectName != nullptr && std::strncmp(objectName, "Default__", 9) == 0;
}

// Rejects partially-initialized / freed-slot poison: real live controllers read
// a sane FOV (~85). NaN compares false on both bounds, so NaN is rejected too.
bool ControllerInfoLooksLive(const ControllerInfo& info) noexcept
{
    return info.objectName[0] != '\0' &&
           std::strcmp(info.objectName, "None") != 0 &&
           info.fovAngle > 1.0f && info.fovAngle < 170.0f;
}

// ---- the scan ---------------------------------------------------------------

// Best live player controller: a non-template *PlayerController class with a
// populated camera pointer (the real in-world instance). Aborts cleanly if the
// object table reallocates mid-scan (loader TOCTOU).
void* FindPlayerControllerOnce(ControllerInfo* infoOut, std::int32_t* foundIndexOut) noexcept
{
    *foundIndexOut = -1;

    MELEVR::LE1::TArrayHeader* objects = MELEVR::LE1::Objects();
    if (!IsReadableAddress(objects, sizeof(MELEVR::LE1::TArrayHeader)))
    {
        return nullptr;
    }
    void** data = objects->data;
    const std::int32_t count = objects->count;
    if (data == nullptr || count <= 0 || count > 8'000'000 ||
        !IsReadableAddress(data, static_cast<size_t>(count) * sizeof(void*)))
    {
        return nullptr;
    }

    for (std::int32_t index = 0; index < count; ++index)
    {
        void* obj = nullptr;
        if (!ReadObjectSlotSEH(data, index, &obj))
        {
            return nullptr;  // table reallocated mid-scan - retry next frame
        }
        if (!PointerLooksCanonicalAligned(obj) ||
            !IsReadableAddress(obj, MELEVR::LE1::kPlayerCameraPtr + sizeof(void*)))
        {
            continue;
        }

        ControllerInfo info = {};
        if (!InspectObjectSEH(obj, &info) || !LooksLikePlayerController(info.className))
        {
            continue;
        }
        if (IsTemplateObjectName(info.objectName) || !ControllerInfoLooksLive(info))
        {
            continue;
        }
        if (info.camera == nullptr || !PointerLooksCanonicalAligned(info.camera))
        {
            continue;
        }

        *infoOut = info;
        *foundIndexOut = index;
        return obj;  // live instance with a camera
    }
    return nullptr;
}

// ---- cache (Present-thread only) --------------------------------------------

void* g_controller = nullptr;
std::int32_t g_controllerIndex = -1;
bool g_loggedFound = false;
char g_lastProbeModeName[128] = {};
void* g_lastProbeModePtr = nullptr;
uint64_t g_probeLogCount = 0;
OwnedModeState g_ownedModes[512] = {};
int g_ownedModeCount = 0;
bool g_ownershipWasEnabled = false;
std::atomic<float> g_activeFpPitchDeg{0.0f};
std::atomic<float> g_activeFpYawDeg{0.0f};
bool g_prewarmWasEnabled = false;
uint64_t g_prewarmLogCount = 0;
void* g_lastOwnershipModePtr = nullptr;
char g_lastOwnershipModeName[128] = {};
OwnedOffset g_lastStableOwnedOffset = {};
bool g_lastStableOwnedValid = false;
uint64_t g_ownershipLogCount = 0;

OwnedModeState* FindOwnedMode(void* mode) noexcept
{
    for (int i = 0; i < g_ownedModeCount; ++i)
    {
        if (g_ownedModes[i].valid && g_ownedModes[i].mode == mode) return &g_ownedModes[i];
    }
    return nullptr;
}

// ---- Class-default (CDO) vanilla cache -------------------------------------------------------------------
// Poison-proof source of a mode's TRUE original offset. Per-instance capture gets poisoned: the prewarm can
// grab a mode already at the owned target (35,0,65) or freshly zeroed (0,0,0) and store THAT as the
// "original", so restore writes the owned value back and the camera never leaves first person. The
// Default__SFXCameraMode_* CLASS TEMPLATE always holds the game's real default (Explore=200,0,40 etc.) and it's
// never written (excluded from owning), so it stays pristine. Each class default is snapshotted during the
// prewarm scan and restore instances to it instead of the captured offset.
struct ClassVanilla { char cls[64]; float x; float y; float z; };
ClassVanilla g_classVanilla[48] = {};
int g_classVanillaCount = 0;

void RecordClassVanilla(const char* cls, float x, float y, float z) noexcept
{
    if (cls == nullptr || cls[0] == '\0') return;
    for (int i = 0; i < g_classVanillaCount; ++i)
        if (std::strcmp(g_classVanilla[i].cls, cls) == 0) return;  // first (pristine) record wins; never overwrite
    if (g_classVanillaCount >= static_cast<int>(sizeof(g_classVanilla) / sizeof(g_classVanilla[0]))) return;
    ClassVanilla& v = g_classVanilla[g_classVanillaCount++];
    SafeCopyCString(v.cls, sizeof(v.cls), cls);
    v.x = x; v.y = y; v.z = z;
}

bool LookupClassVanilla(const char* cls, float* x, float* y, float* z) noexcept
{
    if (cls == nullptr || x == nullptr || y == nullptr || z == nullptr) return false;
    for (int i = 0; i < g_classVanillaCount; ++i)
        if (std::strcmp(g_classVanilla[i].cls, cls) == 0)
        {
            *x = g_classVanilla[i].x; *y = g_classVanilla[i].y; *z = g_classVanilla[i].z;
            return true;
        }
    return false;
}

// an injected owned target - never a real game vanilla. Refusing to capture it as an "original" stops the
// poison where restore writes the owned value back (= no revert).
bool OffsetIsOwnedTarget(float x, float y, float z) noexcept
{
    return x > 34.0f && x < 36.0f && y > -1.0f && y < 1.0f && z > 64.0f && z < 66.0f;
}
// All-zero = a freshly allocated mode the game hasn't set its real per-encounter offset on yet. Capturing it
// would store a wrong (0,0,0) original; own it later from a frame where it shows its real offset.
bool OffsetLooksUninitialized(float x, float y, float z) noexcept
{
    return x == 0.0f && y == 0.0f && z == 0.0f;
}

OwnedModeState* RememberOwnedMode(const MELEVR::GameCamera::CameraSnapshot& snap) noexcept
{
    if (snap.cameraMode == nullptr) return nullptr;
    if (OwnedModeState* existing = FindOwnedMode(snap.cameraMode))
    {
        return existing;  // already tracked - restore uses the per-class switch, not the live offset
    }
    if (g_ownedModeCount >= static_cast<int>(sizeof(g_ownedModes) / sizeof(g_ownedModes[0])))
    {
        return nullptr;
    }
    OwnedModeState& s = g_ownedModes[g_ownedModeCount++];
    s.mode = snap.cameraMode;
    s.originalX = snap.modeOffsetX;
    s.originalY = snap.modeOffsetY;
    s.originalZ = snap.modeOffsetZ;
    SafeCopyCString(s.className, sizeof(s.className), snap.cameraModeName);  // lets the restore switch match
    s.valid = true;
    return &s;
}

OwnedModeState* RememberOwnedModeRaw(void* mode, float x, float y, float z, std::int32_t tableIndex,
                                     const char* className) noexcept
{
    if (mode == nullptr) return nullptr;
    if (OwnedModeState* existing = FindOwnedMode(mode))
    {
        return existing;
    }
    if (g_ownedModeCount >= static_cast<int>(sizeof(g_ownedModes) / sizeof(g_ownedModes[0])))
    {
        return nullptr;
    }
    OwnedModeState& s = g_ownedModes[g_ownedModeCount++];
    s.mode = mode;
    s.originalX = x;
    s.originalY = y;
    s.originalZ = z;
    s.tableIndex = tableIndex;  // captured from the prewarm scan -> lets restore verify liveness
    SafeCopyCString(s.className, sizeof(s.className), className != nullptr ? className : "");
    s.valid = true;
    return &s;
}

// THE RESTORE SWITCH: a fixed table keyed by SFX camera-mode class -> the game's real default offset for that
// mode. Values come from the CDO extraction (camera_defaults.csv) with the dynamic combat/aim entries set to
// the representative REAL value seen in the harvest (the CDO base reads too close for those). Restore just
// looks up the class and writes its number - no remembering the game's live value, so nothing to corrupt.
// Complete coverage of the camera modes owned here; anything not here falls back to the captured/class value.
bool DefaultOffsetForMode(const char* className, float* x, float* y, float* z) noexcept
{
    if (className == nullptr || x == nullptr || y == nullptr || z == nullptr) return false;
    struct Entry { const char* cls; float x; float y; float z; };
    static const Entry kTable[] = {
        { "SFXCameraMode_Explore",          200.0f,   0.0f, 40.0f },
        { "SFXCameraMode_ExploreStorm",     210.0f,   0.0f, 40.0f },
        { "SFXCameraMode_Combat",           125.0f,   0.0f, 40.0f },  // harvest (CDO base 80,-44,15 reads too close)
        { "SFXCameraMode_CombatStorm",      135.0f, -17.0f, 45.0f },
        { "SFXCameraMode_EnterCover",       135.0f, -40.0f, 35.0f },
        { "SFXCameraMode_HipAimCover",       80.0f, -44.0f, 35.0f },
        { "SFXCameraMode_TightAim",          50.0f, -20.0f, 15.0f },  // harvest
        { "SFXCameraMode_SniperAim",         80.0f, -44.0f, 15.0f },
        { "SFXCameraMode_Vehicle",         1100.0f,   0.0f,  0.0f },
        { "SFXCameraMode_VehicleTightAim",    0.0f,   0.0f,  0.0f },
        { "SFXCameraMode_VehicleTightAimEx",  0.0f,   0.0f,  0.0f },
    };
    for (const Entry& e : kTable)
    {
        if (std::strcmp(className, e.cls) == 0)
        {
            *x = e.x; *y = e.y; *z = e.z;
            return true;
        }
    }
    return false;
}

void RestoreOwnedModes() noexcept
{
    int restored = 0;
    int skippedStale = 0;
    int fromClassDefault = 0;
    int kept = 0;
    for (int i = 0; i < g_ownedModeCount; ++i)
    {
        OwnedModeState& s = g_ownedModes[i];
        if (!s.valid || s.mode == nullptr) continue;
        // LIVENESS GATE = the disable-crash fix. UE3's GC frees/recycles SFXCameraMode objects during play,
        // so a stored pointer can be dangling; the blind write below used to scribble into freed memory and
        // corrupt the heap (the ~15s-later crash after toggling off). The object table is authoritative: if
        // this exact pointer still occupies its captured slot, the object is alive & unchanged -> safe to
        // restore. Otherwise it was recycled (its recreated version is already vanilla) -> skip. This is the
        // SAME validation already used for the cached controller (ObjectTableSlotHoldsSEH(g_controllerIndex)).
        // Liveness check before writing (never scribble into freed memory). Prewarm-owned modes have a table
        // index -> O(1) slot check. Tick-owned modes (the ACTIVE camera) have NO index -> verify the pointer
        // still reads as a camera-mode object. The old code skipped ALL index-less modes, so with the prewarm
        // off EVERY owned mode got skipped -> restore did nothing -> camera stuck in first person.
        bool live;
        if (s.tableIndex >= 0)
        {
            live = ObjectTableSlotHoldsSEH(s.tableIndex, s.mode);
        }
        else
        {
            char cls[128] = {}, nm[128] = {};
            float cx = 0.0f, cy = 0.0f, cz = 0.0f;
            (void)ReadSfxCameraModeObjectSEH(s.mode, cls, sizeof(cls), nm, sizeof(nm), &cx, &cy, &cz);
            live = (std::strstr(cls, "CameraMode") != nullptr) || (std::strstr(cls, "CameraBehavior") != nullptr);
        }
        if (!live)
        {
            ++skippedStale;
            continue;
        }
        // Restore via the SWITCH: look up this mode's class in the fixed default table and write that offset.
        // Reliable - a known number, not the game's live value that has to be remembered. Only if the class isn't in
        // the table does it fall back to the captured value / class default.
        // Prefer this mode's OWN captured original. Combat/TightAim cover MANY distinct instances (PowerCover0,
        // CombatCam0, BlindLeftStand0 ...), each with its OWN authored offset - a class-keyed value would snap
        // them all to one generic combat position (that was the cover-restore half of the bug). The per-instance
        // original is clean now: captured once, before the FP write, and the poisoning update-on-real is gone.
        // Only if it looks bad (an injected target / uninitialised) does it fall back to the class default.
        float vx = s.originalX, vy = s.originalY, vz = s.originalZ;
        if (OffsetIsOwnedTarget(vx, vy, vz) || OffsetLooksUninitialized(vx, vy, vz))
        {
            if (DefaultOffsetForMode(s.className, &vx, &vy, &vz)) { ++fromClassDefault; }
            else { LookupClassVanilla(s.className, &vx, &vy, &vz); }
        }
        if (WriteSfxCameraModeOffsetSEH(s.mode, vx, vy, vz))
        {
            ++restored;
        }
        // PRESERVE this live entry (with its REAL captured original) for the next toggle, so re-owning never
        // re-captures the injected (35,0,65) as the "original". Compacting also drops stale entries.
        if (kept != i) g_ownedModes[kept] = s;
        ++kept;
    }
    if (g_ownedModeCount > 0)
    {
        LogLine("[SFXOWN_WRITE] disabled restored=" + std::to_string(restored) +
                " fromClassDefault=" + std::to_string(fromClassDefault) +
                " skippedStale=" + std::to_string(skippedStale) +
                " kept=" + std::to_string(kept) +
                " touched=" + std::to_string(g_ownedModeCount));
    }
    g_ownedModeCount = kept;  // compacted; real originals preserved across the toggle (NOT wiped)
    g_lastStableOwnedValid = false;
    g_lastOwnershipModePtr = nullptr;
    g_lastOwnershipModeName[0] = '\0';
}

// ---- first-person pawn/gun visibility guard -------------------------------------------------------------

struct HeadMeshGroup
{
    void* part[5];
    int originalForceWireframe[5];
    int count;
};

struct PrimitiveVisibilityState
{
    void* component;
    std::uint32_t originalFlags2;
};

struct FunctionInfo
{
    char className[64];
    char objectName[64];
    char outerName[64];
    bool ok;
};

PrimitiveVisibilityState g_fpsVisibleComponents[128] = {};
int g_fpsVisibleComponentCount = 0;
bool g_fpsVisibilityApplied = false;
uint64_t g_fpsVisibilityLogCounter = 0;

HeadMeshGroup g_hiddenHeadGroup = {};
void* g_hiddenHeadBodyMesh = nullptr;
bool g_hiddenHeadApplied = false;
uint64_t g_headHideLogCounter = 0;

// First-person weapon/body hide tracking: every primitive component that gets OwnerNoSee=true set on it
// (body skeletal mesh + each attached actor's primitive components) so they can be restored.
void* g_hiddenWeaponComponents[128] = {};
int g_hiddenWeaponComponentCount = 0;
uint64_t g_weaponHideLogCounter = 0;
void* g_hiddenBodyMesh = nullptr;   // body-hide tracker (independent of the weapon-hide tracker above)

bool InspectFunctionObjectSEH(void* obj, FunctionInfo* info) noexcept
{
    __try
    {
        void* outer = MELEVR::LE1::ReadPtr(obj, MELEVR::LE1::kUObjectOuter);
        SafeCopyCString(info->className, sizeof(info->className), MELEVR::LE1::ClassName(obj));
        SafeCopyCString(info->objectName, sizeof(info->objectName), MELEVR::LE1::ObjectName(obj));
        SafeCopyCString(info->outerName, sizeof(info->outerName), MELEVR::LE1::ObjectName(outer));
        info->ok = true;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        info->ok = false;
        return false;
    }
}

void* FindFunctionByOuterAndName(const char* outerName, const char* functionName) noexcept
{
    MELEVR::LE1::TArrayHeader* objects = MELEVR::LE1::Objects();
    if (!IsReadableAddress(objects, sizeof(MELEVR::LE1::TArrayHeader)))
    {
        return nullptr;
    }
    void** data = objects->data;
    const std::int32_t count = objects->count;
    if (data == nullptr || count <= 0 || count > 8'000'000 ||
        !IsReadableAddress(data, static_cast<size_t>(count) * sizeof(void*)))
    {
        return nullptr;
    }

    for (std::int32_t index = 0; index < count; ++index)
    {
        void* obj = nullptr;
        if (!ReadObjectSlotSEH(data, index, &obj))
        {
            return nullptr;
        }
        if (!PointerLooksCanonicalAligned(obj) ||
            !IsReadableAddress(obj, MELEVR::LE1::kUFunctionFunctionFlags + sizeof(std::uint32_t)))
        {
            continue;
        }

        FunctionInfo info = {};
        if (!InspectFunctionObjectSEH(obj, &info) ||
            std::strcmp(info.className, "Function") != 0 ||
            std::strcmp(info.objectName, functionName) != 0)
        {
            continue;
        }

        if (std::strcmp(info.outerName, outerName) == 0)
        {
            return obj;
        }
    }
    return nullptr;
}

using ProcessEventFn = void(__fastcall*)(void*, void*, void*, void*);

bool ProcessEventSEH(void* object, void* function, void* params) noexcept
{
    if (!PointerLooksCanonicalAligned(object) || !PointerLooksCanonicalAligned(function) || params == nullptr)
    {
        return false;
    }
    __try
    {
        void** vt = *reinterpret_cast<void***>(object);
        if (vt == nullptr) return false;
        auto processEvent = reinterpret_cast<ProcessEventFn>(vt[0x250 / sizeof(void*)]);
        if (processEvent == nullptr) return false;
        processEvent(object, function, params, nullptr);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void* ReadPossessedPawnSEH(void* controller) noexcept
{
    if (!PointerLooksCanonicalAligned(controller))
    {
        return nullptr;
    }
    __try
    {
        void* pawn = MELEVR::LE1::ReadPtr(controller, MELEVR::LE1::kControllerPawn);
        if (PointerLooksCanonicalAligned(pawn)) return pawn;
        pawn = MELEVR::LE1::ReadPtr(controller, MELEVR::LE1::kPlayerAcknowledgedPawn);
        return PointerLooksCanonicalAligned(pawn) ? pawn : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

void* ReadPawnMeshSEH(void* pawn) noexcept
{
    __try
    {
        void* mesh = MELEVR::LE1::ReadPtr(pawn, MELEVR::LE1::kPawnMesh);
        return PointerLooksCanonicalAligned(mesh) ? mesh : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

void* GetPrimarySkelMeshComponentSEH(void* pawn) noexcept
{
    if (!PointerLooksCanonicalAligned(pawn))
    {
        return nullptr;
    }

    void* mesh = ReadPawnMeshSEH(pawn);
    if (mesh != nullptr)
    {
        return mesh;
    }

    static void* fn = nullptr;
    if (fn == nullptr)
    {
        fn = FindFunctionByOuterAndName("Pawn", "GetPrimarySkelMeshComponent");
        if (fn != nullptr) LogLine("[FPSHEAD] found Function Engine.Pawn.GetPrimarySkelMeshComponent");
    }
    struct Params
    {
        void* returnValue;
    } params = {};
    return ProcessEventSEH(pawn, fn, &params) ? params.returnValue : nullptr;
}

bool SetPrimitiveOwnerNoSeeSEH(void* component, bool ownerNoSee) noexcept
{
    if (!PointerLooksCanonicalAligned(component))
    {
        return false;
    }
    static void* fn = nullptr;
    if (fn == nullptr)
    {
        fn = FindFunctionByOuterAndName("PrimitiveComponent", "SetOwnerNoSee");
        if (fn != nullptr) LogLine("[FPSHEAD] found Function Engine.PrimitiveComponent.SetOwnerNoSee");
    }
    struct Params
    {
        unsigned long bNewOwnerNoSee;
    } params = { ownerNoSee ? 1ul : 0ul };
    return ProcessEventSEH(component, fn, &params);
}

bool SetActorHiddenSEH(void* actor, bool hidden) noexcept
{
    if (!PointerLooksCanonicalAligned(actor))
    {
        return false;
    }
    static void* fn = nullptr;
    if (fn == nullptr)
    {
        fn = FindFunctionByOuterAndName("Actor", "SetHidden");
        if (fn != nullptr) LogLine("[FPSVIS] found Function Engine.Actor.SetHidden");
    }
    struct Params
    {
        int hidden;
    } params = { hidden ? 1 : 0 };
    return ProcessEventSEH(actor, fn, &params);
}

bool ReadPointerArrayElementSEH(void* owner, std::uintptr_t offset, int index, void** valueOut) noexcept
{
    if (valueOut == nullptr)
    {
        return false;
    }
    *valueOut = nullptr;
    if (!PointerLooksCanonicalAligned(owner) || index < 0)
    {
        return false;
    }

    __try
    {
        auto* arr = reinterpret_cast<MELEVR::LE1::TArrayHeader*>(reinterpret_cast<BYTE*>(owner) + offset);
        void** data = arr->data;
        const int count = arr->count;
        if (data == nullptr || count <= 0 || count > 512 || index >= count ||
            !IsReadableAddress(data, static_cast<size_t>(count) * sizeof(void*)))
        {
            return false;
        }
        *valueOut = data[index];
        return PointerLooksCanonicalAligned(*valueOut);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *valueOut = nullptr;
        return false;
    }
}

int ReadPointerArrayCountSEH(void* owner, std::uintptr_t offset) noexcept
{
    if (!PointerLooksCanonicalAligned(owner))
    {
        return 0;
    }
    __try
    {
        auto* arr = reinterpret_cast<MELEVR::LE1::TArrayHeader*>(reinterpret_cast<BYTE*>(owner) + offset);
        const int count = arr->count;
        return (arr->data != nullptr && count > 0 && count <= 512) ? count : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

int RemoveActorFromHiddenActorsSEH(void* controller, void* actor) noexcept
{
    if (!PointerLooksCanonicalAligned(controller) || !PointerLooksCanonicalAligned(actor))
    {
        return 0;
    }

    __try
    {
        auto* arr = reinterpret_cast<MELEVR::LE1::TArrayHeader*>(
            reinterpret_cast<BYTE*>(controller) + MELEVR::LE1::kPlayerHiddenActors);
        void** data = arr->data;
        const int count = arr->count;
        if (data == nullptr || count <= 0 || count > 512 ||
            !IsReadableAddress(data, static_cast<size_t>(count) * sizeof(void*)))
        {
            return 0;
        }

        int write = 0;
        int removed = 0;
        for (int read = 0; read < count; ++read)
        {
            if (data[read] == actor)
            {
                ++removed;
                continue;
            }
            if (write != read)
            {
                data[write] = data[read];
            }
            ++write;
        }
        arr->count = write;
        return removed;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

bool ReadPrimitiveFlags2SEH(void* component, std::uint32_t* flagsOut) noexcept
{
    if (flagsOut == nullptr || !PointerLooksCanonicalAligned(component))
    {
        return false;
    }
    __try
    {
        *flagsOut = *reinterpret_cast<std::uint32_t*>(
            reinterpret_cast<BYTE*>(component) + MELEVR::LE1::kPrimitiveFlags2);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *flagsOut = 0;
        return false;
    }
}

bool WritePrimitiveFlags2SEH(void* component, std::uint32_t flags) noexcept
{
    if (!PointerLooksCanonicalAligned(component))
    {
        return false;
    }
    __try
    {
        *reinterpret_cast<std::uint32_t*>(
            reinterpret_cast<BYTE*>(component) + MELEVR::LE1::kPrimitiveFlags2) = flags;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool AddFpsVisibleComponent(void* component) noexcept
{
    if (!PointerLooksCanonicalAligned(component))
    {
        return false;
    }
    for (int i = 0; i < g_fpsVisibleComponentCount; ++i)
    {
        if (g_fpsVisibleComponents[i].component == component)
        {
            std::uint32_t flags = 0;
            return ReadPrimitiveFlags2SEH(component, &flags) &&
                   WritePrimitiveFlags2SEH(component, flags | MELEVR::LE1::kPrimitiveIgnoreHiddenActorsMembership);
        }
    }
    if (g_fpsVisibleComponentCount >=
        static_cast<int>(sizeof(g_fpsVisibleComponents) / sizeof(g_fpsVisibleComponents[0])))
    {
        return false;
    }

    std::uint32_t original = 0;
    if (!ReadPrimitiveFlags2SEH(component, &original))
    {
        return false;
    }
    g_fpsVisibleComponents[g_fpsVisibleComponentCount++] = { component, original };
    return WritePrimitiveFlags2SEH(component, original | MELEVR::LE1::kPrimitiveIgnoreHiddenActorsMembership);
}

void RestoreFpsVisibleComponents() noexcept
{
    for (int i = 0; i < g_fpsVisibleComponentCount; ++i)
    {
        WritePrimitiveFlags2SEH(g_fpsVisibleComponents[i].component, g_fpsVisibleComponents[i].originalFlags2);
    }
    g_fpsVisibleComponentCount = 0;
    g_fpsVisibilityApplied = false;
}

int MarkActorComponentsIgnoreHiddenActors(void* actor) noexcept
{
    int changed = 0;
    const std::uintptr_t arrays[2] =
    {
        MELEVR::LE1::kActorComponents,
        MELEVR::LE1::kActorAllComponents,
    };

    for (std::uintptr_t arrayOffset : arrays)
    {
        const int count = ReadPointerArrayCountSEH(actor, arrayOffset);
        for (int i = 0; i < count; ++i)
        {
            void* component = nullptr;
            if (ReadPointerArrayElementSEH(actor, arrayOffset, i, &component) &&
                AddFpsVisibleComponent(component))
            {
                ++changed;
            }
        }
    }
    return changed;
}

// Track a single component as weapon-hidden (OwnerNoSee=true) so it can be restored later.
// Idempotent: re-hiding an already-tracked component is a no-op (returns false, no double-add).
bool AddHiddenWeaponComponent(void* component) noexcept
{
    if (!PointerLooksCanonicalAligned(component))
    {
        return false;
    }
    for (int i = 0; i < g_hiddenWeaponComponentCount; ++i)
    {
        if (g_hiddenWeaponComponents[i] == component)
        {
            return false;  // already tracked
        }
    }
    if (g_hiddenWeaponComponentCount >=
        static_cast<int>(sizeof(g_hiddenWeaponComponents) / sizeof(g_hiddenWeaponComponents[0])))
    {
        return false;
    }
    if (!SetPrimitiveOwnerNoSeeSEH(component, true))
    {
        return false;
    }
    g_hiddenWeaponComponents[g_hiddenWeaponComponentCount++] = component;
    return true;
}

// Set OwnerNoSee=true on every primitive component of an actor (both the Components and
// AllComponents arrays), tracking each. Models MarkActorComponentsIgnoreHiddenActors's
// iteration, but applies the OwnerNoSee hide instead of the IgnoreHiddenActors flag.
int HideActorComponentsOwnerNoSee(void* actor) noexcept
{
    int hidden = 0;
    const std::uintptr_t arrays[2] =
    {
        MELEVR::LE1::kActorComponents,
        MELEVR::LE1::kActorAllComponents,
    };

    for (std::uintptr_t arrayOffset : arrays)
    {
        const int count = ReadPointerArrayCountSEH(actor, arrayOffset);
        for (int i = 0; i < count; ++i)
        {
            void* component = nullptr;
            if (ReadPointerArrayElementSEH(actor, arrayOffset, i, &component) &&
                AddHiddenWeaponComponent(component))
            {
                ++hidden;
            }
        }
    }
    return hidden;
}

// Restore every weapon-hidden component (OwnerNoSee=false) and clear the tracking.
void RestoreHiddenWeaponComponents() noexcept
{
    for (int i = 0; i < g_hiddenWeaponComponentCount; ++i)
    {
        SetPrimitiveOwnerNoSeeSEH(g_hiddenWeaponComponents[i], false);
    }
    g_hiddenWeaponComponentCount = 0;
}

// Read one FAttachment.Component from a SkeletalMeshComponent.Attachments[] (TArray<FAttachment>; element
// stride 0x34, Component pointer at offset +0). The equipped weapon + bone-attached gear live HERE, not in
// Pawn.Attached[] (which carries no weapon mesh).
bool ReadMeshAttachmentComponentSEH(void* mesh, std::uintptr_t arrayOffset, int index, void** out) noexcept
{
    if (out != nullptr) *out = nullptr;
    if (!PointerLooksCanonicalAligned(mesh) || out == nullptr) return false;
    __try
    {
        const BYTE* arr = reinterpret_cast<const BYTE*>(mesh) + arrayOffset;
        void* data = *reinterpret_cast<void* const*>(arr);
        const int count = *reinterpret_cast<const int*>(arr + 8);
        if (data == nullptr || index < 0 || index >= count) return false;
        constexpr std::uintptr_t kFAttachmentStride = 0x34;  // sizeof(FAttachment)
        const BYTE* elem = reinterpret_cast<const BYTE*>(data) +
                           static_cast<std::uintptr_t>(index) * kFAttachmentStride;
        void* comp = *reinterpret_cast<void* const*>(elem);  // FAttachment.Component @ +0
        if (!PointerLooksCanonicalAligned(comp)) return false;
        *out = comp;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool ReadForceWireframeSEH(void* component, int* value) noexcept
{
    if (value == nullptr || !PointerLooksCanonicalAligned(component))
    {
        return false;
    }
    __try
    {
        *value = MELEVR::LE1::ReadI32(component, MELEVR::LE1::kSkelMeshForceWireframe);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *value = 0;
        return false;
    }
}

bool WriteForceWireframeSEH(void* component, int value) noexcept
{
    if (!PointerLooksCanonicalAligned(component))
    {
        return false;
    }
    __try
    {
        *reinterpret_cast<int*>(reinterpret_cast<BYTE*>(component) + MELEVR::LE1::kSkelMeshForceWireframe) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool FindPackedNameSEH(const char* target, std::uint32_t* packedOut) noexcept
{
    if (target == nullptr || packedOut == nullptr)
    {
        return false;
    }
    *packedOut = 0;
    const size_t targetLen = std::strlen(target);
    if (targetLen == 0 || targetLen > 96)
    {
        return false;
    }

    __try
    {
        BYTE** pools = MELEVR::LE1::NamePools();
        for (std::uint32_t chunk = 0; chunk < 8; ++chunk)
        {
            BYTE* pool = pools[chunk];
            if (pool == nullptr)
            {
                continue;
            }

            MEMORY_BASIC_INFORMATION mi = {};
            if (VirtualQuery(pool, &mi, sizeof(mi)) == 0 ||
                mi.State != MEM_COMMIT ||
                (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
            {
                continue;
            }

            const auto regionStart = reinterpret_cast<std::uintptr_t>(mi.BaseAddress);
            const auto poolStart = reinterpret_cast<std::uintptr_t>(pool);
            const auto regionEnd = regionStart + mi.RegionSize;
            if (poolStart < regionStart || poolStart >= regionEnd)
            {
                continue;
            }

            size_t scanBytes = static_cast<size_t>(regionEnd - poolStart);
            if (scanBytes > 32u * 1024u * 1024u)
            {
                scanBytes = 32u * 1024u * 1024u;
            }
            if (scanBytes <= targetLen + MELEVR::LE1::kFNameEntryString)
            {
                continue;
            }

            for (size_t i = MELEVR::LE1::kFNameEntryString; i + targetLen < scanBytes; ++i)
            {
                const char* s = reinterpret_cast<const char*>(pool + i);
                if (s[0] == target[0] &&
                    std::memcmp(s, target, targetLen) == 0 &&
                    s[targetLen] == '\0')
                {
                    const std::uint32_t nameOffset =
                        static_cast<std::uint32_t>(i - MELEVR::LE1::kFNameEntryString);
                    *packedOut = ((chunk & 0x7u) << 29) | (nameOffset & 0x1FFFFFFFu);
                    return true;
                }
            }
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *packedOut = 0;
        return false;
    }
}

std::uint32_t PackedNameCached(const char* target) noexcept
{
    struct CacheEntry
    {
        const char* target;
        std::uint32_t packed;
        bool searched;
        bool found;
    };
    static CacheEntry cache[3] =
    {
        { "Eye_Left", 0, false, false },
        { "Eye_Right", 0, false, false },
        { "Tongue", 0, false, false },
    };

    for (auto& e : cache)
    {
        if (std::strcmp(e.target, target) == 0)
        {
            if (!e.searched)
            {
                e.searched = true;
                e.found = FindPackedNameSEH(e.target, &e.packed);
                LogLine(std::string("[FPSHEAD] FName ") + e.target +
                        (e.found ? " resolved" : " NOT resolved"));
            }
            return e.found ? e.packed : 0;
        }
    }
    return 0;
}

bool SetBoneHiddenSEH(void* mesh, const char* boneName, bool hidden) noexcept
{
    if (!PointerLooksCanonicalAligned(mesh) || boneName == nullptr)
    {
        return false;
    }
    const std::uint32_t packed = PackedNameCached(boneName);
    if (packed == 0)
    {
        return false;
    }

    if (hidden)
    {
        static void* fn = nullptr;
        if (fn == nullptr)
        {
            fn = FindFunctionByOuterAndName("SkeletalMeshComponent", "HideBoneByName");
            if (fn != nullptr) LogLine("[FPSHEAD] found Function Engine.SkeletalMeshComponent.HideBoneByName");
        }
        struct Params
        {
            std::uint32_t BoneName[2];
            unsigned char PhysBodyOption;
        } params = { { packed, 0u }, 0u };
        return ProcessEventSEH(mesh, fn, &params);
    }

    static void* fn = nullptr;
    if (fn == nullptr)
    {
        fn = FindFunctionByOuterAndName("SkeletalMeshComponent", "UnHideBoneByName");
        if (fn != nullptr) LogLine("[FPSHEAD] found Function Engine.SkeletalMeshComponent.UnHideBoneByName");
    }
    struct Params
    {
        std::uint32_t BoneName[2];
    } params = { { packed, 0u } };
    return ProcessEventSEH(mesh, fn, &params);
}

bool ReadBioPawnHeadMeshGroupSEH(void* pawn, HeadMeshGroup* group) noexcept
{
    if (group == nullptr)
    {
        return false;
    }
    *group = {};
    if (!PointerLooksCanonicalAligned(pawn))
    {
        return false;
    }
    __try
    {
        const std::uintptr_t offsets[5] =
        {
            MELEVR::LE1::kBioPawnHeadMesh,
            MELEVR::LE1::kBioPawnHairMesh,
            MELEVR::LE1::kBioPawnHeadGearMesh,
            MELEVR::LE1::kBioPawnVisorMesh,
            MELEVR::LE1::kBioPawnFacePlateMesh,
        };

        for (int i = 0; i < 5; ++i)
        {
            void* component = MELEVR::LE1::ReadPtr(pawn, offsets[i]);
            if (!PointerLooksCanonicalAligned(component))
            {
                continue;
            }

            bool alreadyAdded = false;
            for (int j = 0; j < group->count; ++j)
            {
                if (group->part[j] == component)
                {
                    alreadyAdded = true;
                    break;
                }
            }
            if (!alreadyAdded && group->count < 5)
            {
                group->part[group->count] = component;
                int wf = 0;
                ReadForceWireframeSEH(component, &wf);
                group->originalForceWireframe[group->count] = wf;
                ++group->count;
            }
        }
        return group->count > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *group = {};
        return false;
    }
}

bool HeadMeshGroupSame(const HeadMeshGroup& a, const HeadMeshGroup& b) noexcept
{
    if (a.count != b.count)
    {
        return false;
    }
    for (int i = 0; i < a.count; ++i)
    {
        if (a.part[i] != b.part[i])
        {
            return false;
        }
    }
    return true;
}
}

namespace MELEVR::GameCamera
{
bool EnsureController() noexcept
{
    // Fast path: the cached controller still sits in its original slot and still
    // validates as a live PlayerController. Slot identity catches destroy/reuse
    // cheaply, without a full rescan every frame.
    if (g_controller != nullptr && ObjectTableSlotHoldsSEH(g_controllerIndex, g_controller))
    {
        ControllerInfo info = {};
        if (InspectObjectSEH(g_controller, &info) && LooksLikePlayerController(info.className) &&
            ControllerInfoLooksLive(info) && info.camera != nullptr)
        {
            return true;
        }
    }

    // Cache is empty or stale - rescan.
    ControllerInfo info = {};
    std::int32_t foundIndex = -1;
    void* found = FindPlayerControllerOnce(&info, &foundIndex);
    if (found != nullptr)
    {
        g_controller = found;
        g_controllerIndex = foundIndex;
        if (!g_loggedFound)
        {
            g_loggedFound = true;
            LogLine(std::string("[HEADTRACK] found live PlayerController: class='") + info.className +
                    "' name='" + info.objectName + "' (camera reachable). Control rotation is now readable.");
        }
        return true;
    }

    g_controller = nullptr;
    g_controllerIndex = -1;
    return false;
}

int ReadWeaponMode(char* nameOut, int nameCap) noexcept
{
    if (g_controller == nullptr)
    {
        return -1;
    }
    return ReadWeaponModeSEH(g_controller, nameOut, nameCap);
}

bool ReadControlRotationUU(int32_t* pitchUU, int32_t* yawUU, int32_t* rollUU) noexcept
{
    if (g_controller == nullptr)
    {
        return false;
    }
    std::int32_t p = 0, y = 0, r = 0;
    if (!ReadControlRotationSEH(g_controller, &p, &y, &r))
    {
        return false;
    }
    if (pitchUU != nullptr) *pitchUU = p;
    if (yawUU != nullptr) *yawUU = y;
    if (rollUU != nullptr) *rollUU = r;
    return true;
}

bool WriteControlRotationUU(int32_t pitchUU, int32_t yawUU, int32_t rollUU) noexcept
{
    if (g_controller == nullptr)
    {
        return false;
    }
    return WriteControlRotationSEH(g_controller, pitchUU, yawUU, rollUU);
}

bool ReadCameraSnapshot(CameraSnapshot* out) noexcept
{
    if (out == nullptr) return false;
    *out = CameraSnapshot{};
    if (!EnsureController())
    {
        return false;
    }
    return ReadCameraSnapshotSEH(g_controller, out);
}

void TickSfxCameraProbe(uint64_t presentId, bool flatProbeMode) noexcept
{
    if (!flatProbeMode)
    {
        return;
    }

    CameraSnapshot snap = {};
    const bool ok = ReadCameraSnapshot(&snap) && snap.valid;
    if (!ok)
    {
        if ((presentId % 300ull) == 1ull)
        {
            LogLine("[SFXOWN_PROBE] unreadable camera snapshot");
        }
        return;
    }

    const bool modeChanged =
        snap.cameraMode != g_lastProbeModePtr ||
        std::strcmp(snap.cameraModeName, g_lastProbeModeName) != 0;
    const bool periodic = (presentId % 300ull) == 1ull;
    if (!modeChanged && !periodic)
    {
        return;
    }

    SafeCopyCString(g_lastProbeModeName, sizeof(g_lastProbeModeName), snap.cameraModeName);
    g_lastProbeModePtr = snap.cameraMode;
    ++g_probeLogCount;

    char line[512] = {};
    sprintf_s(line,
              "[SFXOWN_PROBE] n=%llu present=%llu mode=%s ptr=%p combat=%d ads=%d transition=%d modeFov=%.2f pcFov=%.2f offset=(%.2f,%.2f,%.2f) ctrl=(%d,%d,%d)",
              static_cast<unsigned long long>(g_probeLogCount),
              static_cast<unsigned long long>(presentId),
              snap.cameraModeName,
              snap.cameraMode,
              snap.combatLike ? 1 : 0,
              snap.tightAimLike ? 1 : 0,
              snap.transitionLike ? 1 : 0,
              snap.modeFovDeg,
              snap.controllerFovDeg,
              snap.modeOffsetX,
              snap.modeOffsetY,
              snap.modeOffsetZ,
              snap.controlPitchUU,
              snap.controlYawUU,
              snap.controlRollUU);
    LogLine(line);
}

// Read-only: scan the whole object table and dump EVERY camera-mode CLASS DEFAULT (Default__*) to
// camera_defaults.csv. Catches all ~25 modes (Explore..Combat..cover..sniper..vehicle..photo..conversation),
// including ones never triggered in gameplay - because their CDO templates exist in memory from startup.
// Matches the whole family by class-name pattern (CameraMode/CameraBehavior/CameraTransition/CameraUtility),
// NOT the narrow Explore/Combat/TightAim filter, so nothing is missed. NEVER writes the camera.
void ExtractCameraModeDefaults() noexcept
{
    MELEVR::LE1::TArrayHeader* objects = MELEVR::LE1::Objects();
    if (!IsReadableAddress(objects, sizeof(MELEVR::LE1::TArrayHeader))) return;
    void** data = objects->data;
    const std::int32_t count = objects->count;
    if (data == nullptr || count <= 0 || count > 8'000'000 ||
        !IsReadableAddress(data, static_cast<size_t>(count) * sizeof(void*)))
        return;

    FILE* f = nullptr;
    MELEVR::Logger::EnsureDataFolderExists();
    const std::wstring csvPath = MELEVR::Logger::GetDataFolderPath() + L"\\camera_defaults.csv";
    if (_wfopen_s(&f, csvPath.c_str(), L"w") != 0 || f == nullptr)
        return;
    fprintf(f, "objectName,className,offX,offY,offZ,fovDeg\n");

    int found = 0;
    for (std::int32_t index = 0; index < count; ++index)
    {
        void* obj = nullptr;
        if (!ReadObjectSlotSEH(data, index, &obj)) break;
        if (!PointerLooksCanonicalAligned(obj) ||
            !IsReadableAddress(obj, MELEVR::LE1::kSFXCameraModeFOV + sizeof(float)))
            continue;
        char cls[128] = {}, name[128] = {};
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (!ReadSfxCameraModeObjectSEH(obj, cls, sizeof(cls), name, sizeof(name), &x, &y, &z)) continue;
        if (std::strstr(cls, "CameraMode") == nullptr && std::strstr(cls, "CameraBehavior") == nullptr &&
            std::strstr(cls, "CameraTransition") == nullptr && std::strstr(cls, "CameraUtility") == nullptr)
            continue;
        if (std::strncmp(name, "Default__", 9) != 0) continue;  // CDO templates only = the authored defaults
        const float fov = MELEVR::LE1::ReadF32(obj, MELEVR::LE1::kSFXCameraModeFOV);
        fprintf(f, "%s,%s,%.1f,%.1f,%.1f,%.1f\n", name, cls, x, y, z, fov);
        // Seed the in-memory class->default map (restore's safety net). Skip zero-offset bridges/cutscenes so
        // they can never become a bad fallback value.
        if (!(x == 0.0f && y == 0.0f && z == 0.0f)) RecordClassVanilla(cls, x, y, z);
        ++found;
    }
    fclose(f);
    LogLine("[CDOEXTRACT] wrote " + std::to_string(found) + " camera-mode defaults to camera_defaults.csv");
}

void PrewarmSfxCameraOwnershipProbe(uint64_t tickId, bool flatProbeMode, bool enabled) noexcept
{
    (void)flatProbeMode;  // prewarm runs in VR too now (was flat-only)

    if (!enabled)
    {
        g_prewarmWasEnabled = false;
        return;
    }

    // Once per process: snapshot every camera mode's CLASS DEFAULT from the pristine CDOs into the
    // class->default map, so restore has a complete, poison-free safety net per mode class.
    static bool s_defaultsExtracted = false;
    if (!s_defaultsExtracted)
    {
        ExtractCameraModeDefaults();
        s_defaultsExtracted = true;
    }

    // Scan aggressively at first, then periodically to catch newly loaded mode
    // objects without turning every CalcSceneView into an object-table walk.
    const bool initial = !g_prewarmWasEnabled;
    g_prewarmWasEnabled = true;
    if (!initial && (tickId % 240ull) != 1ull)
    {
        return;
    }

    MELEVR::LE1::TArrayHeader* objects = MELEVR::LE1::Objects();
    if (!IsReadableAddress(objects, sizeof(MELEVR::LE1::TArrayHeader)))
    {
        return;
    }
    void** data = objects->data;
    const std::int32_t count = objects->count;
    if (data == nullptr || count <= 0 || count > 8'000'000 ||
        !IsReadableAddress(data, static_cast<size_t>(count) * sizeof(void*)))
    {
        return;
    }

    int scanned = 0;
    int matched = 0;
    int newlyOwned = 0;
    int wrote = 0;
    for (std::int32_t index = 0; index < count; ++index)
    {
        void* obj = nullptr;
        if (!ReadObjectSlotSEH(data, index, &obj))
        {
            return;
        }
        if (!PointerLooksCanonicalAligned(obj) ||
            !IsReadableAddress(obj, MELEVR::LE1::kSFXCameraModeFOV + sizeof(float)))
        {
            continue;
        }
        ++scanned;

        char className[128] = {};
        char objectName[128] = {};
        float x = 0.0f, y = 0.0f, z = 0.0f;
        if (!ReadSfxCameraModeObjectSEH(obj, className, sizeof(className), objectName, sizeof(objectName), &x, &y, &z))
        {
            continue;
        }
        if (!IsOwnableStableSfxModeClass(className, objectName))
        {
            continue;
        }
        // Never own the class-default TEMPLATE (Default__SFXCameraMode_*). Writing the owned offset into the
        // template makes the game spawn fresh camera instances already at (35,0,65), so the prewarm then
        // captures (35,0,65) as their "original" (poison) and they never revert on toggle-off -> the camera
        // stays stuck in first person. Leave templates vanilla; own only live instances.
        if (IsTemplateObjectName(objectName))
        {
            // The template IS the game's real default for this class -> snapshot it as the poison-proof
            // vanilla, then leave it untouched.
            RecordClassVanilla(className, x, y, z);
            continue;
        }
        ++matched;

        // Skip the mode the per-state TICK is actively managing this frame. Prewarming it would fight the tick's
        // per-state write (cover vs combat, etc.) and cause a periodic 1-frame blip. It is already owned + FP via the
        // tick anyway.
        if (obj == g_lastOwnershipModePtr)
        {
            continue;
        }

        // Pre-own with a NEUTRAL first-person placeholder (proven values 35,0,65). The tick refines it to the exact
        // per-state offset the instant the mode goes active - but it is already FIRST person, which kills the
        // third-person flash. (A per-class FP placeholder was tried and HARD-CRASHED during the holster transition --
        // the FP offsets have NEGATIVE X, which likely breaks the game's preset-interpolation math where the uniform
        // positive 35,0,65 does not. Reverted to the proven value; single-variable isolation.)
        if (OwnedModeState* existing = FindOwnedMode(obj))
        {
            // Already owned: the original is LOCKED (captured pre-write at first own). NEVER re-capture it - the
            // per-state tick writes real per-state offsets here that would otherwise be mistaken for the game's vanilla
            // "original" and poison the restore (camera stuck in first person on toggle-off). Just re-assert FP.
            (void)existing;
            if (WriteSfxCameraModeOffsetSEH(obj, 35.0f, 0.0f, 65.0f)) ++wrote;  // camera to head
            continue;
        }

        OwnedModeState* remembered = RememberOwnedModeRaw(obj, x, y, z, index, className);
        if (remembered == nullptr)
        {
            continue;
        }
        ++newlyOwned;
        if (WriteSfxCameraModeOffsetSEH(obj, 35.0f, 0.0f, 65.0f))  // camera to head
        {
            ++wrote;
        }

        if (newlyOwned <= 32)
        {
            ++g_prewarmLogCount;
            char line[768] = {};
            sprintf_s(line,
                      "[SFXOWN_PREWARM] n=%llu tick=%llu index=%d class=%s name=%s ptr=%p original=(%.2f,%.2f,%.2f) firstPerson=1",
                      static_cast<unsigned long long>(g_prewarmLogCount),
                      static_cast<unsigned long long>(tickId),
                      index,
                      className,
                      objectName,
                      obj,
                      remembered->originalX,
                      remembered->originalY,
                      remembered->originalZ);
            LogLine(line);
        }
    }

    if (initial || newlyOwned > 0)
    {
        LogLine("[SFXOWN_PREWARM] summary tick=" + std::to_string(tickId) +
                " scanned=" + std::to_string(scanned) +
                " matched=" + std::to_string(matched) +
                " newlyOwned=" + std::to_string(newlyOwned) +
                " wrote=" + std::to_string(wrote) +
                " totalTouched=" + std::to_string(g_ownedModeCount));
    }
}

// ---- COVER-STATE HUNT (temporary probe) -----------------------------------------------------------------
// Cover can't be told from open combat by camera mode (shared per-class object). The real "in cover" flag is
// on the pawn. This scans the possessed pawn and logs ONLY small enum/flag fields (or pointer null-flips) that
// CHANGE between samples - so popping in/out of cover makes the responsible offset jump out of the diff.
// Remove once the offset is known.
bool ReadPawnWindowSEH(void* pawn, std::uintptr_t start, std::uint32_t* out, int count) noexcept
{
    if (!PointerLooksCanonicalAligned(pawn)) return false;
    __try
    {
        const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(pawn) + start;
        for (int i = 0; i < count; ++i)
            out[i] = *reinterpret_cast<const std::uint32_t*>(base + static_cast<std::size_t>(i) * 4u);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void CoverStateProbe() noexcept
{
    void* pawn = (g_controller != nullptr) ? ReadPossessedPawnSEH(g_controller) : nullptr;
    if (pawn == nullptr) return;
    static std::uint64_t frame = 0;
    if ((++frame % 6ull) != 0ull) return;  // sample ~every 6th tick
    constexpr std::uintptr_t kStart = 0x100;
    constexpr int kCount = 896;             // 0x100 .. 0xF00
    static std::uint32_t prev[kCount] = {};
    static std::uint32_t cur[kCount] = {};
    static bool primed = false;
    if (!ReadPawnWindowSEH(pawn, kStart, cur, kCount)) return;
    if (primed)
    {
        char line[1900] = {};
        int len = 0;
        bool any = false;
        for (int i = 0; i < kCount; ++i)
        {
            const std::uint32_t o = prev[i], n = cur[i];
            if (o == n) continue;
            const bool smallChange = (n <= 0xFFFFu && o <= 0xFFFFu);
            const bool ptrFlip = ((o == 0u) != (n == 0u)) && (o > 0x10000u || n > 0x10000u);
            if (!smallChange && !ptrFlip) continue;
            const int w = sprintf_s(line + len, sizeof(line) - static_cast<size_t>(len),
                                    " 0x%03llX:%u>%u",
                                    static_cast<unsigned long long>(kStart + static_cast<std::uintptr_t>(i) * 4u), o, n);
            if (w <= 0 || len + w >= static_cast<int>(sizeof(line)) - 1) break;
            len += w;
            any = true;
        }
        if (any) LogLine(std::string("[COVERPROBE]") + line);
    }
    for (int i = 0; i < kCount; ++i) prev[i] = cur[i];
    primed = true;
}

// ---- COVER CORRELATION FINDER (temporary) ---------------------------------------------------------------
// Ground truth for "in cover" without a known flag: the game pulls the camera over the shoulder in cover
// (offset |Y| >= 45) vs centered in open combat (|Y| <= 22). Only frames are trusted where the GAME's preset is
// in the offset (forward X >= 80; the FP eye offsets are < 60). For every pawn field, a count is kept of how often
// "field is a small nonzero" matches cover. The real cover flag correlates ~100%; everything else averages
// out. Run with FP OFF so the offset is the game's value every frame (fast, clean). Remove once the offset's known.
void CoverCorrelSample() noexcept
{
    CameraSnapshot s = {};
    if (!ReadCameraSnapshot(&s) || !s.valid) return;
    const bool combatish = (std::strstr(s.cameraModeName, "Combat") != nullptr ||
                            std::strstr(s.cameraModeName, "TightAim") != nullptr ||
                            std::strstr(s.cameraModeName, "HipAimCover") != nullptr);
    if (!combatish) return;
    if (s.modeOffsetX < 80.0f) return;                       // an FP write, not a game preset -> no ground truth
    const float ay = std::fabs(s.modeOffsetY);
    bool cover;
    if (ay >= 50.0f) cover = true;                           // clearly over-the-shoulder -> cover
    else if (ay <= 25.0f) cover = false;                     // clearly centered -> open combat
    else return;                                             // ambiguous middle -> skip for clean ground truth
    void* pawn = (g_controller != nullptr) ? ReadPossessedPawnSEH(g_controller) : nullptr;
    if (pawn == nullptr) return;
    constexpr int N = 1280;                                  // 0x100 .. 0x1500
    constexpr std::uintptr_t START = 0x100;
    static std::uint32_t cur[N];
    if (!ReadPawnWindowSEH(pawn, START, cur, N)) return;
    // Per-BIT correlation: the cover flag is one bit that is set in EVERY cover sample and NO open sample (or
    // the reverse). coverAND = bits always set in cover; openOR = bits ever set in open; coverAND & ~openOR =
    // the cover-only bits. Whole-word "nonzero" can't see this when the word holds other always-on bits.
    static std::uint32_t coverAND[N], coverOR[N], openAND[N], openOR[N];
    static std::uint32_t coverS = 0, openS = 0;
    static bool init = false;
    if (!init)
    {
        for (int i = 0; i < N; ++i) { coverAND[i] = 0xFFFFFFFFu; coverOR[i] = 0u; openAND[i] = 0xFFFFFFFFu; openOR[i] = 0u; }
        init = true;
    }
    if (cover) { ++coverS; for (int i = 0; i < N; ++i) { coverAND[i] &= cur[i]; coverOR[i] |= cur[i]; } }
    else       { ++openS;  for (int i = 0; i < N; ++i) { openAND[i]  &= cur[i]; openOR[i]  |= cur[i]; } }
    if (((coverS + openS) % 120u) == 0u && coverS >= 20u && openS >= 20u)
    {
        char line[1900] = {};
        int len = sprintf_s(line, sizeof(line), "[COVERCORR] cov=%u open=%u coverBits:", coverS, openS);
        for (int i = 0; i < N && len > 0; ++i)
        {
            const std::uint32_t cb = coverAND[i] & ~openOR[i];   // set in EVERY cover, NO open
            if (cb != 0u)
            {
                const int w = sprintf_s(line + len, sizeof(line) - static_cast<size_t>(len), " 0x%03llX=0x%X",
                                        static_cast<unsigned long long>(START + static_cast<std::uintptr_t>(i) * 4u), cb);
                if (w <= 0 || len + w >= static_cast<int>(sizeof(line)) - 1) break;
                len += w;
            }
        }
        if (len > 0 && len < static_cast<int>(sizeof(line)) - 12) len += sprintf_s(line + len, sizeof(line) - static_cast<size_t>(len), " | openBits:");
        for (int i = 0; i < N && len > 0; ++i)
        {
            const std::uint32_t ob = openAND[i] & ~coverOR[i];   // set in EVERY open, NO cover (inverse flag)
            if (ob != 0u)
            {
                const int w = sprintf_s(line + len, sizeof(line) - static_cast<size_t>(len), " 0x%03llX=0x%X",
                                        static_cast<unsigned long long>(START + static_cast<std::uintptr_t>(i) * 4u), ob);
                if (w <= 0 || len + w >= static_cast<int>(sizeof(line)) - 1) break;
                len += w;
            }
        }
        LogLine(line);
    }
}

// SEH-guarded ownership of an Interpolate transition's DESTINATION mode. The To pointer and its class/offset fields
// are live GAME memory that is occasionally stale/garbage on a given transition frame - the cheap readability check
// passes but the deref still faults. Wrapping the whole read in SEH means a bad To is caught and skipped (no flash fix
// that one frame) instead of crashing (the unguarded reads in the inline version were the crash). Sets To to its OWN
// per-state first-person offset so the blend glides straight there; restores like any owned mode on toggle-off.
void OwnInterpolateDestinationSEH(void* interpolateMode, const OwnedOffset& fallback,
                                  bool inCover, int coverType, int coverAction) noexcept
{
    if (interpolateMode == nullptr) return;
    __try
    {
        void* toMode = MELEVR::LE1::ReadPtr(interpolateMode, 0x168);  // USFXCameraMode_Interpolate.To (LE1 SDK)
        if (toMode == nullptr || !PointerLooksCanonicalAligned(toMode) ||
            !IsReadableAddress(toMode, MELEVR::LE1::kSFXCameraModeFOV + sizeof(float)))
        {
            return;
        }
        void* toClassPtr = MELEVR::LE1::ReadPtr(toMode, MELEVR::LE1::kUObjectClass);
        const char* toClass = (toClassPtr != nullptr && PointerLooksCanonicalAligned(toClassPtr) &&
                               IsReadableAddress(toClassPtr, MELEVR::LE1::kUObjectName + sizeof(std::uint32_t)))
                              ? MELEVR::LE1::ObjectName(toClassPtr) : nullptr;
        const MELEVR::Config::FpState* toFp = (toClass != nullptr)
            ? FpStateForModeName(toClass, toClass, inCover, coverType, coverAction) : nullptr;
        // ONLY own the destination when it resolves to a recognized, ENABLED first-person mode. If the To is a non-FP
        // destination - the Mako / any Vehicle camera mode, a cutscene, a conversation - or a .on=false state, LEAVE
        // IT ALONE: do not remember it (so restore-on-disable won't touch it either) and do not write it. The vehicle
        // camera mode is NOT an SFXCameraMode, so writing a Shepard FP offset into its 0xB4 hit a different field and
        // corrupted the object -> hard heap fast-fail crash entering the Mako (no autopsy; 1383 caught To-read AVs here
        // were the tell; restore=7 fired right before the crash). FP entries are still owned below.
        (void)fallback;
        if (toFp == nullptr || !toFp->on) return;
        if (FindOwnedMode(toMode) == nullptr)
        {
            const float ox = MELEVR::LE1::ReadF32(toMode, MELEVR::LE1::kSFXCameraModeOffset + 0);
            const float oy = MELEVR::LE1::ReadF32(toMode, MELEVR::LE1::kSFXCameraModeOffset + 4);
            const float oz = MELEVR::LE1::ReadF32(toMode, MELEVR::LE1::kSFXCameraModeOffset + 8);
            RememberOwnedModeRaw(toMode, ox, oy, oz, -1, "InterpTo");  // capture the vanilla original ONCE
        }
        // Match the STABLE path's L/R mirror (StableOwnedOffsetForMode): right cover actions (CA_*Right = 2/4/10) negate
        // the lateral Y - without it a RIGHT peek blended toward the un-mirrored LEFT, then stable snapped to RIGHT.
        const bool mirrorRight = inCover && (coverAction == 2 || coverAction == 4 || coverAction == 10);
        const float tx = toFp->x;
        const float ty = mirrorRight ? -toFp->y : toFp->y;
        const float tz = toFp->z;
        WriteSfxCameraModeOffsetSEH(toMode, tx, ty, tz);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // Bad To pointer this frame - skip it. No flash fix this frame, but never a crash.
    }
}

void TickSfxCameraOwnershipProbe(uint64_t presentId, bool flatProbeMode, bool ownershipEnabled) noexcept
{
    (void)flatProbeMode;  // ownership runs in VR too now (was flat-only)
    (void)&CoverStateProbe;     // earlier broad probe kept for reference
    (void)&CoverCorrelSample;   // cover flag found (pawn 0xA54 & 0xA0000); finder kept for reference, not called

    if (!ownershipEnabled)
    {
        if (g_ownershipWasEnabled)
        {
            RestoreOwnedModes();
        }
        g_ownershipWasEnabled = false;
        // First person is off -> release the pawn/gun visibility guard and head-hide so the game's normal
        // HiddenActors membership and head meshes are restored.
        ApplyFirstPersonPawnVisibilityGuard(false);
        ApplyFirstPersonHeadHide(false, 0);
        ApplyFirstPersonWeaponHide(false);
        ApplyFirstPersonBodyHide(false);
        g_activeFpPitchDeg.store(0.0f, std::memory_order_relaxed);
        g_activeFpYawDeg.store(0.0f, std::memory_order_relaxed);
        return;
    }
    g_ownershipWasEnabled = true;

    // First-person hide state, held across frames. s_lastStableFp = the last stable FP state's settings; s_fpGraceFrames
    // = a short window that keeps the hides + anti-cull guard alive through BRIEF non-owned frames (the cover enter /
    // peek-out animation passes through modes that are neither stable-ownable nor flagged Interpolate). Without this the
    // tick's not-owned release fires mid-animation and Shepard flashes visible (body) / wireframe (head).
    constexpr int kFpGraceFrames = 30;
    static const MELEVR::Config::FpState* s_lastStableFp = nullptr;
    static int s_fpGraceFrames = 0;

    CameraSnapshot before = {};
    const bool ok = ReadCameraSnapshot(&before) && before.valid;
    if (!ok)
    {
        if ((presentId % 300ull) == 1ull)
        {
            LogLine("[SFXOWN_WRITE] unreadable camera snapshot");
        }
        return;
    }

    bool shouldWrite = false;
    bool stable = false;
    OwnedOffset target = {};
    const char* policy = "skip";
    if (IsOwnableStableSfxMode(before))
    {
        target = StableOwnedOffsetForMode(before);
        g_lastStableOwnedOffset = target;
        g_lastStableOwnedValid = true;
        shouldWrite = true;
        stable = true;
        policy = "stable";
    }
    else if (before.transitionLike && g_lastStableOwnedValid)
    {
        target = g_lastStableOwnedOffset;
        shouldWrite = true;
        policy = "interpolate-inherit";

        // NO-SCAN FLASH KILL: the camera blends From -> To, and the DESTINATION (To) mode is still at its vanilla
        // THIRD-PERSON offset -> the blend dips to third person = the flash. Own that one mode off the LIVE Interpolate
        // object (no global table walk, no GC race). SEH-guarded: the To pointer is live game memory that's occasionally
        // stale on a frame - the previous UNGUARDED inline reads faulted on that and crashed; now they're caught + skipped.
        OwnInterpolateDestinationSEH(before.cameraMode, g_lastStableOwnedOffset,
                                     before.inCover, before.coverType, before.coverAction);
    }

    const bool modeChanged =
        before.cameraMode != g_lastOwnershipModePtr ||
        std::strcmp(before.cameraModeInstance, g_lastOwnershipModeName) != 0;  // track INSTANCE (cover shares the Combat class)
    const bool periodic = (presentId % 240ull) == 1ull;

    if (!shouldWrite)
    {
        if (modeChanged || periodic)
        {
            SafeCopyCString(g_lastOwnershipModeName, sizeof(g_lastOwnershipModeName), before.cameraModeInstance);
            g_lastOwnershipModePtr = before.cameraMode;
            char line[512] = {};
            sprintf_s(line,
                      "[SFXOWN_WRITE] present=%llu policy=%s mode=%s inst=%s ptr=%p reason=not-owned before=(%.2f,%.2f,%.2f)",
                      static_cast<unsigned long long>(presentId),
                      policy,
                      before.cameraModeName,
                      before.cameraModeInstance,
                      before.cameraMode,
                      before.modeOffsetX,
                      before.modeOffsetY,
                      before.modeOffsetZ);
            LogLine(line);
        }
        // VEHICLE (Mako etc.): the possessed pawn is NOT Shepard. The grace-hold below would keep applying the last FP
        // state's hides to the VEHICLE's body mesh + its attachments (a different actor / layout) - which HARD-CRASHED
        // entering the Mako in first person (last log line before death was [FPSWEAPON] hiding the Mako's 5 attachments).
        // The Mako is third person ALWAYS: drop every hide (these calls restore Shepard and never hide the vehicle),
        // cancel the grace window, and leave the vehicle completely untouched.
        if (std::strstr(before.cameraModeName, "Vehicle") != nullptr)
        {
            s_fpGraceFrames = 0;
            ApplyFirstPersonPawnVisibilityGuard(false);
            ApplyFirstPersonHeadHide(false, 0);
            ApplyFirstPersonWeaponHide(false);
            ApplyFirstPersonBodyHide(false);
            g_activeFpPitchDeg.store(0.0f, std::memory_order_relaxed);
            g_activeFpYawDeg.store(0.0f, std::memory_order_relaxed);
            return;
        }
        // Per-state THIRD PERSON: a RECOGNIZED state with first person disabled (.on=false). The cover/combat
        // sub-states share ONE mode object, so merely skipping the write leaves the previous FP offset stale (camera
        // stuck in first person). Actively restore the mode's ORIGINAL offset + drop all hides so THIS sub-state
        // collapses cleanly to third person; cancel the grace hold so it does not keep Shepard hidden.
        const MELEVR::Config::FpState* fpHere = FpStateForModeName(before.cameraModeName, before.cameraModeInstance,
                                                                   before.inCover, before.coverType, before.coverAction);
        if (fpHere != nullptr && !fpHere->on)
        {
            OwnedModeState* rem = RememberOwnedMode(before);
            if (rem != nullptr)
                WriteSfxCameraModeOffsetSEH(before.cameraMode, rem->originalX, rem->originalY, rem->originalZ);
            s_fpGraceFrames = 0;
            ApplyFirstPersonPawnVisibilityGuard(false);
            ApplyFirstPersonHeadHide(false, 0);
            ApplyFirstPersonWeaponHide(false);
            ApplyFirstPersonBodyHide(false);
            g_activeFpPitchDeg.store(0.0f, std::memory_order_relaxed);
            g_activeFpYawDeg.store(0.0f, std::memory_order_relaxed);
            return;
        }
        // Owned, but the active mode is not a stable first-person state (or no inherited transition). GRACE HOLD:
        // if this was just in a first-person state, keep the hides + anti-cull guard alive for a short window so the
        // cover enter / peek-out animation (which passes through un-flagged transition modes) doesn't flash Shepard
        // back visible / wireframe. Releases once the window expires (a real exit to cutscene/vehicle/menu).
        if (s_lastStableFp != nullptr && s_fpGraceFrames > 0)
        {
            --s_fpGraceFrames;
            // In cover, hold the LIVE cover state (body hidden) instead of a stale non-cover state that shows it.
            const MELEVR::Config::FpState* hold = ResolveTransitionHoldState(before, s_lastStableFp);
            ApplyFirstPersonPawnVisibilityGuard(true);
            ApplyFirstPersonHeadHide(hold->hideHead, 0);
            ApplyFirstPersonBodyHide(hold->hideBody);
            ApplyFirstPersonWeaponHide(hold->hideWeapon);
            return;
        }
        ApplyFirstPersonPawnVisibilityGuard(false);
        ApplyFirstPersonHeadHide(false, 0);
        ApplyFirstPersonWeaponHide(false);
        ApplyFirstPersonBodyHide(false);
        g_activeFpPitchDeg.store(0.0f, std::memory_order_relaxed);
        g_activeFpYawDeg.store(0.0f, std::memory_order_relaxed);
        return;
    }

    OwnedModeState* remembered = RememberOwnedMode(before);
    const bool wrote = remembered != nullptr &&
                       WriteSfxCameraModeOffsetSEH(before.cameraMode, target.x, target.y, target.z);  // camera to head

    // Diagnostic logging is THROTTLED to mode-changes + every 240th frame. The post-write "after" snapshot and the
    // file write happen ONLY inside this gate - never per-frame - so head/camera motion no longer hitches on a
    // redundant full snapshot + log I/O every moving frame (was: `|| changedEnough`, true whenever the offset moved).
    if (modeChanged || periodic)
    {
        SafeCopyCString(g_lastOwnershipModeName, sizeof(g_lastOwnershipModeName), before.cameraModeInstance);
        g_lastOwnershipModePtr = before.cameraMode;
        ++g_ownershipLogCount;
        CameraSnapshot after = {};
        const bool readAfter = ReadCameraSnapshot(&after) && after.valid && after.cameraMode == before.cameraMode;
        char line[768] = {};
        sprintf_s(line,
                  "[SFXOWN_WRITE] n=%llu present=%llu policy=%s stable=%d wrote=%d mode=%s inst=%s ptr=%p original=(%.2f,%.2f,%.2f) before=(%.2f,%.2f,%.2f) target=(%.2f,%.2f,%.2f) after=(%.2f,%.2f,%.2f) cov9A8=%u A4C=%u 9B0=%u 950=%u inCov=%d",
                  static_cast<unsigned long long>(g_ownershipLogCount),
                  static_cast<unsigned long long>(presentId),
                  policy,
                  stable ? 1 : 0,
                  wrote ? 1 : 0,
                  before.cameraModeName,
                  before.cameraModeInstance,
                  before.cameraMode,
                  remembered != nullptr ? remembered->originalX : 0.0f,
                  remembered != nullptr ? remembered->originalY : 0.0f,
                  remembered != nullptr ? remembered->originalZ : 0.0f,
                  before.modeOffsetX,
                  before.modeOffsetY,
                  before.modeOffsetZ,
                  target.x,
                  target.y,
                  target.z,
                  readAfter ? after.modeOffsetX : -9999.0f,
                  readAfter ? after.modeOffsetY : -9999.0f,
                  readAfter ? after.modeOffsetZ : -9999.0f,
                  before.coverProbe[0],
                  before.coverProbe[1],
                  before.coverProbe[2],
                  before.coverProbe[3],
                  before.inCover ? 1 : 0);
        LogLine(line);
    }

    // This mode is owned this frame. Force the pawn/gun visible (and hide the head) only when actually
    // sitting in a stable first-person state; release the guard otherwise. Runs on this same Present thread.
    // The mesh hide must NOT flicker off during the game's cover<->fire interpolate transition (the mode
    // becomes Interpolate for a few frames, which is not a "stable" FP state). Hold the LAST stable state's
    // settings through the transition so Shepard doesn't pop back visible (wireframe flash) mid-animation.
    const MELEVR::Config::FpState* fpNow = nullptr;
    if (ownershipEnabled && stable)
    {
        fpNow = FpStateForModeName(before.cameraModeName, before.cameraModeInstance, before.inCover, before.coverType, before.coverAction);
        s_lastStableFp = fpNow;
        s_fpGraceFrames = kFpGraceFrames;  // re-arm the grace window each stable FP frame
    }
    else if (ownershipEnabled && before.transitionLike)
    {
        // interpolate-inherit: keep the previous stable state's hide/angle through the transition - EXCEPT when the
        // pawn is already in cover, where the live cover state is resolved so the body hides through the cover enter.
        fpNow = ResolveTransitionHoldState(before, s_lastStableFp);
    }
    // The owned path was reached, so first person is active this frame -> keep the pawn/gun visible (anti-cull)
    // across both stable and transition frames, matching the camera ownership.
    ApplyFirstPersonPawnVisibilityGuard(ownershipEnabled);
    // Mirror the look-yaw for a RIGHT-side cover action (CA_*Right = 2/4/10) so the over-the-wall view tilts to the
    // correct side, matching the mirrored lateral offset in StableOwnedOffsetForMode. Pitch never mirrors.
    const bool mirrorYawRight = before.inCover &&
        (before.coverAction == 2 || before.coverAction == 4 || before.coverAction == 10);
    g_activeFpPitchDeg.store(fpNow ? fpNow->pitch : 0.0f, std::memory_order_relaxed);
    g_activeFpYawDeg.store(fpNow ? (mirrorYawRight ? -fpNow->yaw : fpNow->yaw) : 0.0f, std::memory_order_relaxed);
    ApplyFirstPersonHeadHide(fpNow != nullptr && fpNow->hideHead, 0);   // per-state head hide
    ApplyFirstPersonBodyHide(fpNow != nullptr && fpNow->hideBody);      // per-state body hide
    ApplyFirstPersonWeaponHide(fpNow != nullptr && fpNow->hideWeapon);  // per-state weapon (mesh-attachment) hide
}

void GetActiveFpViewAngleDeg(float* pitchDeg, float* yawDeg) noexcept
{
    if (pitchDeg) *pitchDeg = g_activeFpPitchDeg.load(std::memory_order_relaxed);
    if (yawDeg)   *yawDeg   = g_activeFpYawDeg.load(std::memory_order_relaxed);
}

bool ApplyFirstPersonPawnVisibilityGuard(bool enabled) noexcept
{
    if (!enabled)
    {
        if (g_fpsVisibilityApplied || g_fpsVisibleComponentCount > 0)
        {
            RestoreFpsVisibleComponents();
            LogLine("[FPSVIS] restored primitive HiddenActors membership flags");
        }
        return false;
    }

    void* pawn = (g_controller != nullptr) ? ReadPossessedPawnSEH(g_controller) : nullptr;
    if (pawn == nullptr)
    {
        const uint64_t n = ++g_fpsVisibilityLogCounter;
        if ((n % 180ull) == 1ull)
        {
            LogLine("[FPSVIS] enabled=1 pawn=0");
        }
        return false;
    }

    int removedHidden = 0;
    int actorVisibleCalls = 0;
    int componentMarks = 0;

    removedHidden += RemoveActorFromHiddenActorsSEH(g_controller, pawn);
    actorVisibleCalls += SetActorHiddenSEH(pawn, false) ? 1 : 0;
    componentMarks += MarkActorComponentsIgnoreHiddenActors(pawn);

    void* bodyMesh = GetPrimarySkelMeshComponentSEH(pawn);
    if (bodyMesh != nullptr && AddFpsVisibleComponent(bodyMesh))
    {
        ++componentMarks;
    }

    HeadMeshGroup headGroup = {};
    if (ReadBioPawnHeadMeshGroupSEH(pawn, &headGroup))
    {
        for (int i = 0; i < headGroup.count; ++i)
        {
            if (AddFpsVisibleComponent(headGroup.part[i]))
            {
                ++componentMarks;
            }
        }
    }

    const int attachedCount = ReadPointerArrayCountSEH(pawn, MELEVR::LE1::kActorAttached);
    for (int i = 0; i < attachedCount; ++i)
    {
        void* attachedActor = nullptr;
        if (!ReadPointerArrayElementSEH(pawn, MELEVR::LE1::kActorAttached, i, &attachedActor))
        {
            continue;
        }
        removedHidden += RemoveActorFromHiddenActorsSEH(g_controller, attachedActor);
        actorVisibleCalls += SetActorHiddenSEH(attachedActor, false) ? 1 : 0;
        componentMarks += MarkActorComponentsIgnoreHiddenActors(attachedActor);
    }

    g_fpsVisibilityApplied = componentMarks > 0;

    const uint64_t n = ++g_fpsVisibilityLogCounter;
    if ((n % 180ull) == 1ull)
    {
        LogLine(std::string("[FPSVIS] enabled=1 pawn=1") +
                " attached=" + std::to_string(attachedCount) +
                " hiddenRemoved=" + std::to_string(removedHidden) +
                " actorVisible=" + std::to_string(actorVisibleCalls) +
                " componentMarks=" + std::to_string(componentMarks) +
                " cached=" + std::to_string(g_fpsVisibleComponentCount));
    }

    return componentMarks > 0 || removedHidden > 0 || actorVisibleCalls > 0;
}

bool ApplyFirstPersonHeadHide(bool enabled, int componentIndex) noexcept
{
    (void)componentIndex; // Retained only for old call sites/config; BioPawn head fields are fixed.

    void* pawn = (g_controller != nullptr) ? ReadPossessedPawnSEH(g_controller) : nullptr;
    void* mesh = (pawn != nullptr) ? GetPrimarySkelMeshComponentSEH(pawn) : nullptr;
    HeadMeshGroup group = {};
    const bool haveHeadGroup = (pawn != nullptr) ? ReadBioPawnHeadMeshGroupSEH(pawn, &group) : false;
    const bool wantHidden = enabled && haveHeadGroup && group.count > 0;

    if (g_hiddenHeadApplied && (!wantHidden || !HeadMeshGroupSame(g_hiddenHeadGroup, group) || mesh != g_hiddenHeadBodyMesh))
    {
        for (int i = 0; i < g_hiddenHeadGroup.count; ++i)
        {
            SetPrimitiveOwnerNoSeeSEH(g_hiddenHeadGroup.part[i], false);
            // Restore to 0, NOT the captured "original": bForceWireframe is a debug flag that is always 0 on Shepard's
            // meshes, and the captured original gets polluted with a forced 1 if the head group is re-read while already
            // hidden - which would write 1 back here and leave Shepard stuck as a wireframe after an FP<->TP switch.
            WriteForceWireframeSEH(g_hiddenHeadGroup.part[i], 0);
        }
        if (g_hiddenHeadBodyMesh != nullptr)
        {
            SetBoneHiddenSEH(g_hiddenHeadBodyMesh, "Eye_Left", false);
            SetBoneHiddenSEH(g_hiddenHeadBodyMesh, "Eye_Right", false);
            SetBoneHiddenSEH(g_hiddenHeadBodyMesh, "Tongue", false);
        }
        LogLine("[FPSHEAD] restored BioPawn head group count=" + std::to_string(g_hiddenHeadGroup.count));
        g_hiddenHeadApplied = false;
        g_hiddenHeadGroup = {};
        g_hiddenHeadBodyMesh = nullptr;
    }

    if (!wantHidden)
    {
        const uint64_t n = ++g_headHideLogCounter;
        if ((n % 180ull) == 1ull)
        {
            LogLine(std::string("[FPSHEAD] enabled=") + std::to_string(enabled ? 1 : 0) +
                    " mode=biopawn_head_group" +
                    " pawn=" + (pawn != nullptr ? "1" : "0") +
                    " mesh=" + (mesh != nullptr ? "1" : "0") +
                    " headParts=" + std::to_string(group.count) +
                    " haveHeadGroup=" + std::to_string(haveHeadGroup ? 1 : 0));
        }
        return false;
    }

    bool meshOk = true;
    for (int i = 0; i < group.count; ++i)
    {
        meshOk = SetPrimitiveOwnerNoSeeSEH(group.part[i], true) && meshOk;
        meshOk = WriteForceWireframeSEH(group.part[i], 1) && meshOk;
    }
    bool boneOk = true;
    if (mesh != nullptr)
    {
        boneOk = SetBoneHiddenSEH(mesh, "Eye_Left", true) && boneOk;
        boneOk = SetBoneHiddenSEH(mesh, "Eye_Right", true) && boneOk;
        boneOk = SetBoneHiddenSEH(mesh, "Tongue", true) && boneOk;
    }
    if (meshOk && !g_hiddenHeadApplied)
    {
        LogLine("[FPSHEAD] hiding BioPawn head group count=" + std::to_string(group.count) +
                " method=OwnerNoSee+Wireframe+Bones");
    }
    if (meshOk)
    {
        g_hiddenHeadGroup = group;
        g_hiddenHeadBodyMesh = mesh;
        g_hiddenHeadApplied = true;
    }

    const uint64_t n = ++g_headHideLogCounter;
    if ((n % 180ull) == 1ull)
    {
        LogLine(std::string("[FPSHEAD] enabled=") + std::to_string(enabled ? 1 : 0) +
                " mode=biopawn_head_group" +
                " pawn=" + (pawn != nullptr ? "1" : "0") +
                " mesh=" + (mesh != nullptr ? "1" : "0") +
                " headParts=" + std::to_string(group.count) +
                " haveHeadGroup=" + std::to_string(haveHeadGroup ? 1 : 0) +
                " method=OwnerNoSee+Wireframe+Bones" +
                " meshOk=" + std::to_string(meshOk ? 1 : 0) +
                " boneOk=" + std::to_string(boneOk ? 1 : 0));
    }
    return meshOk;
}

bool ApplyFirstPersonWeaponHide(bool enabled) noexcept
{
    void* pawn = (g_controller != nullptr) ? ReadPossessedPawnSEH(g_controller) : nullptr;
    void* bodyMesh = (pawn != nullptr) ? GetPrimarySkelMeshComponentSEH(pawn) : nullptr;
    const bool wantHidden = enabled && bodyMesh != nullptr;

    // Desired hide-set = components bone-attached to Shepard's BODY MESH (the equipped weapon + gear), read from
    // SkeletalMeshComponent.Attachments[].Component (0x3B4). The weapon is NOT a Pawn.Attached[] actor - those
    // carry no weapon mesh - it's a mesh attachment. Collect first, then diff vs tracked.
    void* desired[128] = {};
    int desiredCount = 0;
    auto pushDesired = [&](void* component) noexcept
    {
        if (!PointerLooksCanonicalAligned(component) ||
            desiredCount >= static_cast<int>(sizeof(desired) / sizeof(desired[0])))
        {
            return;
        }
        for (int i = 0; i < desiredCount; ++i)
        {
            if (desired[i] == component) return;  // dedupe
        }
        desired[desiredCount++] = component;
    };

    int attachCount = 0;
    if (wantHidden)
    {
        attachCount = ReadPointerArrayCountSEH(bodyMesh, MELEVR::LE1::kSkelMeshAttachments);
        for (int i = 0; i < attachCount; ++i)
        {
            void* comp = nullptr;
            if (ReadMeshAttachmentComponentSEH(bodyMesh, MELEVR::LE1::kSkelMeshAttachments, i, &comp))
            {
                pushDesired(comp);
            }
        }
    }

    // Has the tracked set changed? (count or any member differs from what's already hidden)
    bool setSame = (desiredCount == g_hiddenWeaponComponentCount);
    if (setSame)
    {
        for (int i = 0; i < desiredCount && setSame; ++i)
        {
            bool found = false;
            for (int k = 0; k < g_hiddenWeaponComponentCount; ++k)
            {
                if (g_hiddenWeaponComponents[k] == desired[i]) { found = true; break; }
            }
            setSame = found;
        }
    }

    // Restore previously-tracked components when disabling or when the set changed.
    if (g_hiddenWeaponComponentCount > 0 && (!wantHidden || !setSame))
    {
        const int restoredCount = g_hiddenWeaponComponentCount;
        RestoreHiddenWeaponComponents();
        LogLine("[FPSWEAPON] restored count=" + std::to_string(restoredCount));
    }

    if (!wantHidden)
    {
        const uint64_t n = ++g_weaponHideLogCounter;
        if ((n % 180ull) == 1ull)
        {
            LogLine(std::string("[FPSWEAPON] enabled=") + std::to_string(enabled ? 1 : 0) +
                    " mesh=" + (bodyMesh != nullptr ? "1" : "0") +
                    " count=0");
        }
        return false;
    }

    // Apply: OwnerNoSee each bone-attached component (AddHiddenWeaponComponent sets the flag + tracks; dedupes).
    if (!setSame)
    {
        for (int i = 0; i < desiredCount; ++i)
        {
            AddHiddenWeaponComponent(desired[i]);
        }
        LogLine("[FPSWEAPON] hiding count=" + std::to_string(g_hiddenWeaponComponentCount) +
                " (meshAttachments=" + std::to_string(attachCount) + ")");
    }

    const uint64_t n = ++g_weaponHideLogCounter;
    if ((n % 180ull) == 1ull)
    {
        LogLine(std::string("[FPSWEAPON] enabled=1 mesh=1 count=") +
                std::to_string(g_hiddenWeaponComponentCount));
    }
    return g_hiddenWeaponComponentCount > 0;
}

// Hide ONLY Shepard's body skeletal mesh (independent of weapon-hide). OwnerNoSee toggles the body out of the
// first-person view; its own tracker so it restores separately from the weapon.
bool ApplyFirstPersonBodyHide(bool enabled) noexcept
{
    void* pawn = (g_controller != nullptr) ? ReadPossessedPawnSEH(g_controller) : nullptr;
    void* bodyMesh = (pawn != nullptr) ? GetPrimarySkelMeshComponentSEH(pawn) : nullptr;
    const bool wantHidden = enabled && bodyMesh != nullptr;
    if (g_hiddenBodyMesh != nullptr && (!wantHidden || g_hiddenBodyMesh != bodyMesh))
    {
        SetPrimitiveOwnerNoSeeSEH(g_hiddenBodyMesh, false);
        LogLine("[FPSBODY] restored body mesh");
        g_hiddenBodyMesh = nullptr;
    }
    if (!wantHidden) return false;
    if (g_hiddenBodyMesh != bodyMesh && SetPrimitiveOwnerNoSeeSEH(bodyMesh, true))
    {
        g_hiddenBodyMesh = bodyMesh;
        LogLine("[FPSBODY] hiding body mesh");
    }
    return g_hiddenBodyMesh != nullptr;
}

}
