#pragma once

// Clean-room minimal LE1 game interface (clean rebuild copy).
//
// Expresses ONLY the factual memory layout of Mass Effect LE1's own engine
// objects (offsets, the name-pool decode, the global object table address)
// needed to read/write the player camera. No code copied from the ME3Tweaks
// LE1-SDK (AGPL) - it was a reference for these facts only. Offsets are for
// LE1 build v2.0.0.48602 and are validated at runtime before any use.
// Offsets and class layouts verified live against the shipped executable.
// (tag research-archive-2026-06-15).

#include <Windows.h>

#include <cstdint>

namespace MELEVR::LE1
{
// Module-base-relative addresses (MassEffect1.exe).
constexpr std::uintptr_t kNamePoolsOffset = 0x16A2090;   // FNameEntry** [chunk]
constexpr std::uintptr_t kGObjObjectsOffset = 0x1770670; // TArray<UObject*>

// UObject field offsets.
constexpr std::uintptr_t kUObjectName = 0x48;   // FName (packed dword)
constexpr std::uintptr_t kUObjectClass = 0x50;  // UClass*
constexpr std::uintptr_t kUObjectOuter = 0x40;  // UObject* - the object's Outer (for a UFunction = its class)

// FNameEntry: name string begins at +0x0C (after flags + hash-next).
constexpr std::uintptr_t kFNameEntryString = 0x0C;

// AActor (base of APlayerController).
constexpr std::uintptr_t kActorComponents = 0x0060; // TArray<UActorComponent*>
constexpr std::uintptr_t kActorAllComponents = 0x0080; // TArray<UActorComponent*>
constexpr std::uintptr_t kActorAttached = 0x00D0; // TArray<AActor*>
constexpr std::uintptr_t kActorLocation = 0x108;  // FVector
constexpr std::uintptr_t kActorRotation = 0x114;  // FRotator (3x int32, 65536 = 360 deg)
                                                  // - the look state the stick drives and the camera reads.
// AActor.WorldInfo (CPF_Const|CPF_Transient - the engine keeps every actor's pointer current). The route
// to the live AWorldInfo without any GObjects scan: controller -> WorldInfo. (Engine_classes.h AActor)
constexpr std::uintptr_t kActorWorldInfo = 0x01AC;   // AWorldInfo*

// APlayerController.
constexpr std::uintptr_t kPlayerCameraPtr = 0x6B0; // ACamera*
constexpr std::uintptr_t kPlayerFOVAngle = 0x758;  // float
constexpr std::uintptr_t kPlayerHiddenActors = 0x05C8; // TArray<AActor*>
constexpr std::uintptr_t kControllerPawn = 0x03DC; // AController::Pawn
constexpr std::uintptr_t kPlayerAcknowledgedPawn = 0x06C0; // APlayerController::AcknowledgedPawn

// MAKO turret aim (2026-07-08). The Mako (ABioVehicleWheeled) aims its cannon NOT via ControlRotation
// (that only moves the crosshair + chase camera) but via a runtime yaw/pitch on the vehicle's APPEARANCE
// object, reached: possessedPawn -> m_oBehavior(0x820) -> m_oAppearanceType(0x1CC) -> m_aTurretRuntimeInfo
// TArray(0x7C) -> element[0] { yaw@0x00, pitch@0x04 (float, rel body), autoTrackFlags@0x08 (0x1 yaw|0x2 pitch) }.
// Camera is separate (ASVehicle DriverViewYaw/Pitch 0x804/0x800). SDK: SFXGame_classes.h 31271/10560/12735,
// struct FBioVehicleTurretRunTimeInfo SFXGame_structs.h:2072. UNITS unverified (probe first).
// MAKO turret BONE CONTROLLER (2026-07-08, the deterministic path). The cannon's final pose is the
// USkelControlSingleBone.BoneRotation the mesh composes; the game only overwrites DesiredBoneRotation
// (the lag target) from the stick each tick. Reach: pawn.Mesh(0x480) -> Animations(0x26C, live UAnimTree)
// -> SkelControlLists(0x140) -> walk NextControl chains, match class "TurretConstrained". SDK:
// Engine_classes.h 17522/17693/26814/40953/17170, SFXGame_classes.h:9864. Offsets UNVERIFIED live -> probe.
constexpr std::uintptr_t kSkelMeshAnimTree        = 0x026C; // USkeletalMeshComponent.Animations (UAnimTree*)
constexpr std::uintptr_t kAnimTreeSkelCtrlLists   = 0x0140; // UAnimTree.SkelControlLists TArray<FSkelControlListHead>
constexpr std::uintptr_t kSkelCtrlListStride      = 0x0010; // FSkelControlListHead {BoneName@0, ControlHead@8}
constexpr std::uintptr_t kSkelCtrlListHead        = 0x0008; // USkelControlBase* ControlHead
constexpr std::uintptr_t kSkelCtrlName            = 0x0098; // FName ControlName
constexpr std::uintptr_t kSkelCtrlNext            = 0x00A0; // USkelControlBase* NextControl
constexpr std::uintptr_t kSkelCtrlStrength        = 0x00A8; // float ControlStrength (0..1; need >0)
constexpr std::uintptr_t kSingleBoneRotation      = 0x00E0; // FRotator BoneRotation (APPLIED pose; int pitch/yaw/roll)
constexpr std::uintptr_t kSingleBoneApplyFlags    = 0x00FC; // bitfield: bApplyRotation 0x2, bAddRotation 0x8
constexpr std::uint32_t  kSingleBoneApplyRotMask  = 0x0002;
constexpr std::uintptr_t kTurretDesiredBoneRot    = 0x011C; // FRotator DesiredBoneRotation (lag target the game writes)
constexpr std::uintptr_t kTurretConstrainFlags    = 0x0130; // bConstrainPitch 0x1, bConstrainYaw 0x2, ...

constexpr std::uintptr_t kBioVehicleBehavior      = 0x0820; // ABioVehicleWheeled -> UBioVehicleBehaviorBase*
constexpr std::uintptr_t kBioBehaviorAppearance   = 0x01CC; // UBioActorBehavior -> UBioInterfaceAppearanceVehicle*
constexpr std::uintptr_t kAppearanceTurretArray   = 0x007C; // TArray<FBioVehicleTurretRunTimeInfo> (data@0, count@8? see note)
constexpr std::uintptr_t kTurretInfoYaw           = 0x0000; // float, relative yaw
constexpr std::uintptr_t kTurretInfoPitch         = 0x0004; // float, relative pitch
constexpr std::uintptr_t kTurretInfoFlags         = 0x0008; // bitfield: 0x1 autoTrackYaw, 0x2 autoTrackPitch
constexpr std::uint32_t  kTurretInfoStride        = 0x000C; // 3 floats/element (yaw,pitch,flags-word)

// ABioPlayerController -> USFXGameModeManager -> CurrentMode byte. This is the engine's single
// authoritative "what UI context is active" enum (EGameModes): 0 Default(gameplay), 1 Vehicle,
// 2 PowerWheel, 3 WeaponWheel, 4 Command, 5 Conversation, 6 Cinematic, 7 GUI (full-screen
// front-end menu: pause/inventory/squad/journal/map/options), 8 Movie, 9 Galaxy, 10 Minigame,
// 11 Photo, 12 FlyCam. Menu-exclusive: 7 is never gameplay/convo/cutscene. (SFXGame_classes.h:5660,5976,4060)
constexpr std::uintptr_t kSFXGameModeManager   = 0x0A70; // USFXGameModeManager* on ABioPlayerController
constexpr std::uintptr_t kGameModeCurrentMode  = 0x00B8; // unsigned char CurrentMode (EGameModes)

// APawn.
constexpr std::uintptr_t kPawnMesh = 0x0480; // USkeletalMeshComponent*
// Pawn "in cover" = ABioPawn.CoverAction byte (SDK SFXGame_classes.h ABioPawn @0x0A56): 0 in open combat,
// non-zero while pinned/leaning/firing from cover. The uint32 at 0xA54 is read (= CoverLeanBias|CoverType|
// CoverAction|.. packed) and mask the CoverAction byte (bits 16..23). The camera mode can't tell cover from
// open combat (cover reuses the per-class Combat/TightAim camera object by name) - only the pawn knows.
// (Earlier 0xA0000 mask caught only CoverAction bits 1|3, so cover-actions like 1/4/5 read as open -> the
// "combat still controls cover" bug. The full byte catches every cover state.)
constexpr std::uintptr_t kPawnCoverState = 0x0A54;       // read uint32: bytes = CoverLeanBias|CoverType|CoverAction|..
// CoverType(0xA55, bits 8..15) is the STATE (set while pinned in cover, idle or not); CoverAction(0xA56,
// bits 16..23) is the ACTION (lean/fire only). Mask BOTH. CoverAction alone missed pinned-idle cover (the
// "combat still controls cover" bug - 254 idle-cover frames had CoverAction=0 but CoverType=1).
constexpr std::uint32_t  kPawnCoverMask  = 0x00FFFF00u;  // CoverType OR CoverAction byte != 0  =>  in cover

// ABioPawn head-part mesh components. These are separate from the body mesh and
// separate from weapon/accessory attachments.
constexpr std::uintptr_t kBioPawnHeadMesh = 0x07EC;      // USkeletalMeshComponent*
constexpr std::uintptr_t kBioPawnHairMesh = 0x07F4;      // USkeletalMeshComponent*
constexpr std::uintptr_t kBioPawnHeadGearMesh = 0x07FC;  // USkeletalMeshComponent*
constexpr std::uintptr_t kBioPawnVisorMesh = 0x0804;     // USkeletalMeshComponent*
constexpr std::uintptr_t kBioPawnFacePlateMesh = 0x080C; // USkeletalMeshComponent*

// USkeletalMeshComponent.
constexpr std::uintptr_t kSkelMeshAttachments = 0x03B4; // TArray<FAttachment>
constexpr std::uintptr_t kSkelMeshForceWireframe = 0x03F8; // int bForceWireframe

// UPrimitiveComponent second flag word. bIgnoreHiddenActorsMembership lets a
// primitive draw even when its owning actor is in PlayerController.HiddenActors.
constexpr std::uintptr_t kPrimitiveFlags2 = 0x016C;
constexpr std::uint32_t kPrimitiveIgnoreHiddenActorsMembership = 0x00080000u;

// UFunction.
constexpr std::uintptr_t kUFunctionFunctionFlags = 0x00D8; // DWORD

// AWorldInfo (the global world actor; LE1's is a BioWorldInfo subclass - TimeDilation is inherited at the
// SAME offset). TimeDilation scales the per-tick DeltaTime for the WHOLE sim: 0 = world frozen, 1 = normal,
// 2 = double. The synchronized-sequential lever - freeze the world for the held-eye frame so both eyes of a
// submitted pair render the SAME instant (kills the AER eye-time-gap "skip" at high IPD). Source of facts:
// refs/LE1-ASI-Plugins/LE1-SDK Engine_classes.h AWorldInfo (validated at runtime before any write).
constexpr std::uintptr_t kWorldInfoTimeDilation = 0x0718;  // float
constexpr std::uintptr_t kWorldInfoPauser = 0x0644;  // APlayerReplicationInfo* - non-null the instant any
                                                     // menu pauses the sim (read-only pause detector)
constexpr std::uintptr_t kWorldInfoTimeSeconds  = 0x0720;  // float (read-only; proves the world is advancing/frozen)
constexpr std::uintptr_t kWorldInfoRealTimeSeconds = 0x0728;  // float (wall-clock; advances even when sim frozen)
constexpr std::uintptr_t kWorldInfoDeltaSeconds = 0x0730;  // float (per-tick sim delta - 0 when sim frozen)
// AWorldInfo bitfield word holding bPlayersOnly (UE3's "freeze everything but players" tick mode - a
// menu-pause candidate mechanism). [LIVEGUI2] telemetry reads it to fingerprint what ME1 menus actually
// do; measured 2026-07-17 because Pauser stayed null and TimeSeconds kept advancing under the ESC menu.
constexpr std::uintptr_t kWorldInfoFlagsWord790 = 0x0790;         // uint32 bitfield
constexpr std::uint32_t  kWorldInfoPlayersOnlyMask = 0x00000080u; // bPlayersOnly : 1
constexpr std::uint32_t  kWorldInfoPlayersOnlyPendingMask = 0x00000100u; // bPlayersOnlyPending : 1

// ULocalPlayer - UE3 DynamicResolution supersample fraction. >1.0 renders the SCENE at higher INTERNAL
// resolution (sharper) without changing the window/backbuffer; the engine resolves it back down. The clean
// fix for "fill the headset sharply." SDK: refs LExSDKv2 LE1 ULocalPlayer (2026-06-16).
constexpr std::uintptr_t kLocalPlayerDynResFraction = 0x05C4;  // float (1.0 native; 1.25/1.5/2.0 supersample)

// ACamera.
constexpr std::uintptr_t kCameraCache = 0x45C;       // FTCameraCache
constexpr std::uintptr_t kCameraCurrentMode = 0x568; // USFXCameraMode* - its class name is the
                                                     // engine's "weapons out" state (Combat/TightAim
                                                     // vs Explore/...). Used to gate head-aim.

// USFXCameraMode (the active mode object at kCameraCurrentMode): its FOV is the
// authored field-of-view the camera builds POV.FOV from - the upstream FOV source.
constexpr std::uintptr_t kSFXCameraModeFOV = 0x10C;  // float (degrees)
// Authored camera offset relative to the pawn hook: X=fwd/back, Y=lateral (over-the-shoulder),
// Z=up. The Y component shifts the camera sideways - the per-eye AER eye offset rides on it.
constexpr std::uintptr_t kSFXCameraModeOffset = 0xB4;  // FVector (3 float); Y = +4
// Camera-mode flags bitfield. bFirstPerson is bit 0x20 - the GAME's NATIVE first-person switch. Set it and
// the game renders this mode first-person itself (off the head HookOffset @0xC0) WITHOUT touching Offset,
// so third-person restores perfectly by just clearing the bit.
constexpr std::uintptr_t kSFXCameraModeFlags = 0x124;           // uint32 bitfield
constexpr std::uint32_t  kSFXCameraModeFirstPersonMask = 0x20;  // bFirstPerson : 1

// USFXCameraMode_Interpolate.To - destination mode of a camera-mode blend (LE1 SDK).
constexpr std::uintptr_t kInterpolateToMode = 0x168;

// ACamera flags word (SDK Engine_classes.h ACamera @0x0514; layout cross-checked: CameraCache @0x45C
// matches kCameraCache). bConstrainAspectRatio (bit 0x2) is what LETTERBOXES conversations/cutscenes:
// the cinematic camera constrains to 16:9 and paints black bars into the (square) render. Gameplay
// leaves it clear and fills the viewport. Clearing the bit makes cines render full-frame exactly like
// gameplay - the "squished cutscene" fix at square (AER/DIBR) resolutions.
constexpr std::uintptr_t kCameraFlagsWord = 0x0514;              // uint32 bitfield on ACamera
constexpr std::uint32_t  kCameraConstrainAspectMask = 0x2;       // bConstrainAspectRatio : 1
constexpr std::uintptr_t kCameraConstrainedAspect = 0x04E8;      // float ConstrainedAspectRatio (16:9 = 1.7778)

// FTPOV (inside FTViewTarget and FTCameraCache, at their offset 0).
constexpr std::uintptr_t kPOVLocation = 0x00;  // FVector (3 float)
constexpr std::uintptr_t kPOVRotation = 0x0C;  // FRotator (3 int: Pitch, Yaw, Roll)
constexpr std::uintptr_t kPOVFOV = 0x18;       // float

// FRotator component layout (int32 each).
constexpr std::uintptr_t kRotPitch = 0x00;
constexpr std::uintptr_t kRotYaw = 0x04;
constexpr std::uintptr_t kRotRoll = 0x08;

struct TArrayHeader
{
    void** data;
    std::int32_t count;
    std::int32_t max;
};

inline BYTE* GameBase() noexcept
{
    return reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
}

inline BYTE** NamePools() noexcept
{
    return reinterpret_cast<BYTE**>(GameBase() + kNamePoolsOffset);
}

inline TArrayHeader* Objects() noexcept
{
    return reinterpret_cast<TArrayHeader*>(GameBase() + kGObjObjectsOffset);
}

// Decode a packed FName dword (low 29 bits = byte offset into chunk, top 3 =
// chunk index) to its ANSI string. Returns nullptr if anything is off.
inline const char* DecodeFName(std::uint32_t packed) noexcept
{
    const std::uint32_t offset = packed & 0x1FFFFFFFu;
    const std::uint32_t chunk = (packed >> 29) & 0x7u;
    BYTE* pool = NamePools()[chunk];
    if (pool == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<const char*>(pool + offset + kFNameEntryString);
}

inline std::uint32_t ReadU32(const void* base, std::uintptr_t offset) noexcept
{
    return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const BYTE*>(base) + offset);
}

inline void* ReadPtr(const void* base, std::uintptr_t offset) noexcept
{
    return *reinterpret_cast<void* const*>(reinterpret_cast<const BYTE*>(base) + offset);
}

inline float ReadF32(const void* base, std::uintptr_t offset) noexcept
{
    return *reinterpret_cast<const float*>(reinterpret_cast<const BYTE*>(base) + offset);
}

inline std::int32_t ReadI32(const void* base, std::uintptr_t offset) noexcept
{
    return *reinterpret_cast<const std::int32_t*>(reinterpret_cast<const BYTE*>(base) + offset);
}

inline const char* ObjectName(const void* obj) noexcept
{
    if (obj == nullptr)
    {
        return nullptr;
    }
    return DecodeFName(ReadU32(obj, kUObjectName));
}

inline const char* ClassName(const void* obj) noexcept
{
    if (obj == nullptr)
    {
        return nullptr;
    }
    return ObjectName(ReadPtr(obj, kUObjectClass));
}

// FRotator int32 units <-> degrees (65536 units = 360 degrees).
constexpr float kUnrealRotToDegrees = 360.0f / 65536.0f;
constexpr float kDegreesToUnrealRot = 65536.0f / 360.0f;
}
