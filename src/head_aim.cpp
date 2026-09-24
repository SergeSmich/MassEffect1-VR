#include "head_aim.h"

#include <Windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include "le1_game.h"
#include "logger.h"
#include "vr_config.h"

// Extracted verbatim from the retired game_camera.cpp (which stays OUT of the
// build): only the controller-cache plumbing + ControlRotation access survive.
// All functions run on the game's Present thread only (called from the XR
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

// Slot-identity liveness: the object's table slot still holds this exact pointer.
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

// ---- transition tripwire (FPQUAR pattern) ------------------------------------
// After world churn (load / possession swap / controller death), do NOT write
// for ~90 frames: the quarantine rides out the window where freed-but-committed
// objects still read plausibly and mid-transition writes have caused crashes before.
void* g_stablePrevController = nullptr;
std::int32_t g_stablePrevIndex = -1;
bool g_controllerWasLost = false;   // EnsureController fully failed since the last stability check
int g_quarantineCallsLeft = 0;

// ---- First-person camera ownership (M1, 2026-07-08) --------------------------
// Owns the ACTIVE SFXCameraMode's eye offset (0xB4) + native bFirstPerson flag for the four on-foot
// states. Deliberately MINIMAL vs the retired game_camera.cpp machinery: no full-table prewarm scans
// (those hitched gameplay), no hide machinery (M2, separate). Crash lessons baked in:
//   - restore writes are gated on OBJECT-TABLE SLOT IDENTITY (freed mode object -> abandon, never write)
//   - the Interpolate destination is only owned when it classifies as an ENABLED FP state; anything
//     unrecognized (Mako/Vehicle, cutscene, conversation) is never written (the Mako 0xB4 write was a
//     heap-corruption crash in the old build)
//   - originals are captured ONCE per owned object and never re-captured (poison-proofing: a self-
//     written FP offset must never be mistaken for the game's vanilla)
//   - all writes skip during the ControllerStable quarantine (world churn window)

// ---- FP hide/anti-cull helpers (M2, 2026-07-08; ported from retired game_camera.cpp) ----------------
// STATELESS redesign of the old FPSVIS/FPSHEAD machinery: the old code cached component pointers for
// restore and that cache wrote FREED memory during load GC (the Therum CTD). Here NOTHING is remembered
// across frames - apply and clear both re-walk controller->pawn->components fresh, and "restore" writes
// the known VANILLA values (flag bit clear, wireframe 0, OwnerNoSee false), which is idempotent and can
// never be poisoned. The HiddenActors ARRAY MUTATION from the old guard is NOT ported (deleted crash
// path); the per-component IgnoreHiddenActorsMembership bit gives the same anti-cull without it.

struct FunctionInfo
{
    char className[64];
    char objectName[64];
    char outerName[64];
};

bool InspectFunctionObjectSEH(void* obj, FunctionInfo* info) noexcept
{
    __try
    {
        void* outer = MELEVR::LE1::ReadPtr(obj, MELEVR::LE1::kUObjectOuter);
        SafeCopyCString(info->className, sizeof(info->className), MELEVR::LE1::ClassName(obj));
        SafeCopyCString(info->objectName, sizeof(info->objectName), MELEVR::LE1::ObjectName(obj));
        SafeCopyCString(info->outerName, sizeof(info->outerName), MELEVR::LE1::ObjectName(outer));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void* FindFunctionByOuterAndName(const char* outerName, const char* functionName) noexcept
{
    MELEVR::LE1::TArrayHeader* objects = MELEVR::LE1::Objects();
    if (!IsReadableAddress(objects, sizeof(MELEVR::LE1::TArrayHeader)))
        return nullptr;
    void** data = objects->data;
    const std::int32_t count = objects->count;
    if (data == nullptr || count <= 0 || count > 8'000'000 ||
        !IsReadableAddress(data, static_cast<size_t>(count) * sizeof(void*)))
        return nullptr;

    for (std::int32_t index = 0; index < count; ++index)
    {
        void* obj = nullptr;
        if (!ReadObjectSlotSEH(data, index, &obj))
            return nullptr;
        if (!PointerLooksCanonicalAligned(obj) ||
            !IsReadableAddress(obj, MELEVR::LE1::kUFunctionFunctionFlags + sizeof(std::uint32_t)))
            continue;
        FunctionInfo info = {};
        if (!InspectFunctionObjectSEH(obj, &info) ||
            std::strcmp(info.className, "Function") != 0 ||
            std::strcmp(info.objectName, functionName) != 0)
            continue;
        if (std::strcmp(info.outerName, outerName) == 0)
            return obj;
    }
    return nullptr;
}

using ProcessEventFn = void(__fastcall*)(void*, void*, void*, void*);

bool ProcessEventSEH(void* object, void* function, void* params) noexcept
{
    if (!PointerLooksCanonicalAligned(object) || !PointerLooksCanonicalAligned(function) || params == nullptr)
        return false;
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
    if (!PointerLooksCanonicalAligned(controller)) return nullptr;
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

struct ObjectLabel
{
    char className[96];
    char objectName[96];
    char outerName[96];
};

bool ReadObjectLabelSEH(void* object, ObjectLabel* out) noexcept
{
    if (out == nullptr) return false;
    out->className[0] = '\0';
    out->objectName[0] = '\0';
    out->outerName[0] = '\0';
    if (!PointerLooksCanonicalAligned(object) ||
        !IsReadableAddress(object, MELEVR::LE1::kUObjectClass + sizeof(void*))) return false;
    __try
    {
        void* classObject = MELEVR::LE1::ReadPtr(object, MELEVR::LE1::kUObjectClass);
        void* outerObject = MELEVR::LE1::ReadPtr(object, MELEVR::LE1::kUObjectOuter);
        SafeCopyCString(out->className, sizeof(out->className), MELEVR::LE1::ObjectName(classObject));
        SafeCopyCString(out->objectName, sizeof(out->objectName), MELEVR::LE1::ObjectName(object));
        SafeCopyCString(out->outerName, sizeof(out->outerName), MELEVR::LE1::ObjectName(outerObject));
        return out->className[0] != '\0' || out->objectName[0] != '\0';
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out->className[0] = '\0';
        out->objectName[0] = '\0';
        out->outerName[0] = '\0';
        return false;
    }
}

bool ReadObjectOuterSEH(void* object, void** out) noexcept
{
    if (out == nullptr) return false;
    *out = nullptr;
    if (!PointerLooksCanonicalAligned(object)) return false;
    __try
    {
        *out = MELEVR::LE1::ReadPtr(object, MELEVR::LE1::kUObjectOuter);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *out = nullptr;
        return false;
    }
}

struct PointerArrayView
{
    void* data;
    std::int32_t count;
    std::int32_t max;
};

bool ReadPointerArrayViewSEH(void* owner, std::uintptr_t offset, PointerArrayView* out) noexcept
{
    if (out == nullptr) return false;
    out->data = nullptr;
    out->count = 0;
    out->max = 0;
    if (!PointerLooksCanonicalAligned(owner)) return false;
    __try
    {
        const auto* header = reinterpret_cast<const MELEVR::LE1::TArrayHeader*>(
            reinterpret_cast<const BYTE*>(owner) + offset);
        out->data = header->data;
        out->count = header->count;
        out->max = header->max;
        if (out->count < 0 || out->count > 4096 || out->max < out->count || out->max > 1'000'000)
        {
            out->data = nullptr;
            return false;
        }
        if (out->count > 0 &&
            (!PointerLooksCanonicalAligned(out->data) ||
             !IsReadableAddress(out->data, static_cast<size_t>(out->count) * sizeof(void*))))
        {
            out->data = nullptr;
            return false;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out->data = nullptr;
        out->count = 0;
        out->max = 0;
        return false;
    }
}

bool ReadPointerArrayElementSEH(void* data, int index, void** out) noexcept
{
    if (out == nullptr) return false;
    *out = nullptr;
    if (!PointerLooksCanonicalAligned(data) || index < 0) return false;
    __try
    {
        *out = reinterpret_cast<void**>(data)[index];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *out = nullptr;
        return false;
    }
}

std::uint64_t PointerArraySignature(const PointerArrayView& view) noexcept
{
    if (view.data == nullptr || view.count <= 0) return 0;
    const int n = (view.count < 128) ? view.count : 128;
    std::uint64_t hash = 1469598103934665603ull;
    for (int i = 0; i < n; ++i)
    {
        void* object = nullptr;
        ReadPointerArrayElementSEH(view.data, i, &object);
        const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(object);
        hash ^= static_cast<std::uint64_t>(value) + static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ull;
        hash *= 1099511628211ull;
    }
    return hash;
}

std::string ObjectPointerText(const void* object) noexcept
{
    char text[32] = {};
    sprintf_s(text, sizeof(text), "%p", object);
    return text;
}

void LogWeaponProbeObject(const char* listName, int index, void* object) noexcept
{
    ObjectLabel label = {};
    if (!ReadObjectLabelSEH(object, &label))
    {
        LogLine(std::string("[WEAPONPROBE] ") + listName + "[" + std::to_string(index) +
                "] ptr=" + ObjectPointerText(object) + " unreadable");
        return;
    }
    LogLine(std::string("[WEAPONPROBE] ") + listName + "[" + std::to_string(index) +
            "] ptr=" + ObjectPointerText(object) +
            " class='" + label.className + "' name='" + label.objectName +
            "' outer='" + label.outerName + "'");
}

void LogWeaponProbeArray(const char* listName, void* owner, std::uintptr_t offset,
                         int maxEntries) noexcept
{
    PointerArrayView view = {};
    if (!ReadPointerArrayViewSEH(owner, offset, &view))
    {
        LogLine(std::string("[WEAPONPROBE] ") + listName + " unreadable");
        return;
    }
    LogLine(std::string("[WEAPONPROBE] ") + listName + " count=" +
            std::to_string(view.count) + " max=" + std::to_string(view.max));
    const int n = (view.count < maxEntries) ? view.count : maxEntries;
    for (int i = 0; i < n; ++i)
    {
        void* object = nullptr;
        if (ReadPointerArrayElementSEH(view.data, i, &object) &&
            PointerLooksCanonicalAligned(object))
            LogWeaponProbeObject(listName, i, object);
    }
}

void LogWeaponProbeWeaponCandidates(const PointerArrayView& components) noexcept
{
    if (components.data == nullptr || components.count <= 0) return;
    const int n = (components.count < 128) ? components.count : 128;
    void* loggedActors[16] = {};
    int loggedActorCount = 0;
    constexpr int kMaxLoggedActors = static_cast<int>(sizeof(loggedActors) / sizeof(loggedActors[0]));
    for (int i = 0; i < n; ++i)
    {
        void* component = nullptr;
        if (!ReadPointerArrayElementSEH(components.data, i, &component) ||
            !PointerLooksCanonicalAligned(component))
            continue;

        ObjectLabel label = {};
        if (!ReadObjectLabelSEH(component, &label) ||
            std::strcmp(label.className, "SkeletalMeshComponent") != 0 ||
            std::strcmp(label.outerName, "BioWeaponRanged") != 0)
            continue;

        void* weaponActor = nullptr;
        const bool outerReadable = ReadObjectOuterSEH(component, &weaponActor);
        LogLine(std::string("[WEAPONPROBE] weapon component candidate index=") +
                std::to_string(i) + " component=" + ObjectPointerText(component) +
                " outer=" + ObjectPointerText(weaponActor));
        LogWeaponProbeObject("weapon.component", i, component);

        bool alreadyLogged = false;
        for (int j = 0; j < loggedActorCount; ++j)
        {
            if (loggedActors[j] == weaponActor)
            {
                alreadyLogged = true;
                break;
            }
        }
        if (!alreadyLogged && outerReadable && PointerLooksCanonicalAligned(weaponActor))
        {
            if (loggedActorCount < kMaxLoggedActors)
                loggedActors[loggedActorCount++] = weaponActor;
            LogWeaponProbeObject("weapon.actor", 0, weaponActor);
        }
    }
}

// In-cover from the possessed pawn's cover-state flag (0xA54; CoverType|CoverAction byte non-zero).
// Cover reuses the SAME Combat/TightAim mode object as open combat, so the mode NAME can't tell them
// apart - this pawn flag is the only reliable "hugging a wall" signal. Pure read, SEH-guarded.
// Returns true only when definitively in cover; any read failure returns false (fail to third-person...
// which for cover means "not in cover" -> FP stays as the mode says; safe direction: never forces FP).
bool ReadPawnInCoverSEH(void* controller) noexcept
{
    void* pawn = ReadPossessedPawnSEH(controller);
    if (pawn == nullptr) return false;
    __try
    {
        const std::uint32_t flags = static_cast<std::uint32_t>(
            MELEVR::LE1::ReadI32(pawn, MELEVR::LE1::kPawnCoverState));
        return (flags & MELEVR::LE1::kPawnCoverMask) != 0u;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool SetPrimitiveOwnerNoSeeSEH(void* component, bool ownerNoSee) noexcept
{
    if (!PointerLooksCanonicalAligned(component)) return false;
    static void* fn = nullptr;
    if (fn == nullptr)
    {
        fn = FindFunctionByOuterAndName("PrimitiveComponent", "SetOwnerNoSee");
        if (fn != nullptr) LogLine("[FPHIDE] found Function Engine.PrimitiveComponent.SetOwnerNoSee");
    }
    struct Params { unsigned long bNewOwnerNoSee; } params = { ownerNoSee ? 1ul : 0ul };
    return ProcessEventSEH(component, fn, &params);
}

bool SetActorHiddenSEH(void* actor, bool hidden) noexcept
{
    if (!PointerLooksCanonicalAligned(actor)) return false;
    static void* fn = nullptr;
    if (fn == nullptr)
    {
        fn = FindFunctionByOuterAndName("Actor", "SetHidden");
        if (fn != nullptr) LogLine("[FPHIDE] found Function Engine.Actor.SetHidden");
    }
    struct Params { int hidden; } params = { hidden ? 1 : 0 };
    return ProcessEventSEH(actor, fn, &params);
}

bool ReadPointerArrayElementSEH(void* owner, std::uintptr_t offset, int index, void** valueOut) noexcept
{
    if (valueOut == nullptr) return false;
    *valueOut = nullptr;
    if (!PointerLooksCanonicalAligned(owner) || index < 0) return false;
    __try
    {
        auto* arr = reinterpret_cast<MELEVR::LE1::TArrayHeader*>(reinterpret_cast<BYTE*>(owner) + offset);
        void** data = arr->data;
        const int count = arr->count;
        if (data == nullptr || count <= 0 || count > 512 || index >= count ||
            !IsReadableAddress(data, static_cast<size_t>(count) * sizeof(void*)))
            return false;
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
    if (!PointerLooksCanonicalAligned(owner)) return 0;
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

// Validate a pointer really is a live primitive component before writing to it or calling a UFunction on
// it (2026-07-08 crash hardening). A combat attached-actor / component array can transiently hold a
// stale or half-initialized entry that merely LOOKS canonical; writing flags2 into that (or invoking
// ProcessEvent with it as `this`) corrupts an unrelated object. Requiring the object's class name to
// contain "Component" rejects those - real primitive components are *Component classes.
bool ComponentLooksLiveSEH(void* comp) noexcept
{
    if (!PointerLooksCanonicalAligned(comp)) return false;
    __try
    {
        void* cls = MELEVR::LE1::ReadPtr(comp, MELEVR::LE1::kUObjectClass);
        if (!PointerLooksCanonicalAligned(cls)) return false;
        const char* name = MELEVR::LE1::ObjectName(cls);
        return name != nullptr && std::strstr(name, "Component") != nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool SetPrimitiveIgnoreHiddenBitSEH(void* component, bool set) noexcept
{
    if (!ComponentLooksLiveSEH(component)) return false;
    __try
    {
        std::uint32_t* flags = reinterpret_cast<std::uint32_t*>(
            reinterpret_cast<BYTE*>(component) + MELEVR::LE1::kPrimitiveFlags2);
        if (set) *flags |= MELEVR::LE1::kPrimitiveIgnoreHiddenActorsMembership;
        else     *flags &= ~MELEVR::LE1::kPrimitiveIgnoreHiddenActorsMembership;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Set/clear the anti-cull bit on every primitive component of an actor. Stateless: vanilla is 0, so
// clearing on a fresh walk restores exactly, no tracking table needed.
int MarkActorComponentsIgnoreHidden(void* actor, bool set) noexcept
{
    int changed = 0;
    const std::uintptr_t arrays[2] = { MELEVR::LE1::kActorComponents, MELEVR::LE1::kActorAllComponents };
    for (std::uintptr_t arrayOffset : arrays)
    {
        const int count = ReadPointerArrayCountSEH(actor, arrayOffset);
        for (int i = 0; i < count; ++i)
        {
            void* component = nullptr;
            if (ReadPointerArrayElementSEH(actor, arrayOffset, i, &component) &&
                SetPrimitiveIgnoreHiddenBitSEH(component, set))
                ++changed;
        }
    }
    return changed;
}

bool WriteForceWireframeSEH(void* component, int value) noexcept
{
    if (!PointerLooksCanonicalAligned(component)) return false;
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
    if (target == nullptr || packedOut == nullptr) return false;
    *packedOut = 0;
    const size_t targetLen = std::strlen(target);
    if (targetLen == 0 || targetLen > 96) return false;
    __try
    {
        BYTE** pools = MELEVR::LE1::NamePools();
        for (std::uint32_t chunk = 0; chunk < 8; ++chunk)
        {
            BYTE* pool = pools[chunk];
            if (pool == nullptr) continue;
            MEMORY_BASIC_INFORMATION mi = {};
            if (VirtualQuery(pool, &mi, sizeof(mi)) == 0 || mi.State != MEM_COMMIT ||
                (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
                continue;
            const auto regionStart = reinterpret_cast<std::uintptr_t>(mi.BaseAddress);
            const auto poolStart = reinterpret_cast<std::uintptr_t>(pool);
            const auto regionEnd = regionStart + mi.RegionSize;
            if (poolStart < regionStart || poolStart >= regionEnd) continue;
            size_t scanBytes = static_cast<size_t>(regionEnd - poolStart);
            if (scanBytes > 32u * 1024u * 1024u) scanBytes = 32u * 1024u * 1024u;
            if (scanBytes <= targetLen + MELEVR::LE1::kFNameEntryString) continue;
            for (size_t i = MELEVR::LE1::kFNameEntryString; i + targetLen < scanBytes; ++i)
            {
                const char* s = reinterpret_cast<const char*>(pool + i);
                if (s[0] == target[0] && std::memcmp(s, target, targetLen) == 0 && s[targetLen] == '\0')
                {
                    const std::uint32_t nameOffset = static_cast<std::uint32_t>(i - MELEVR::LE1::kFNameEntryString);
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
    struct CacheEntry { const char* target; std::uint32_t packed; bool searched; bool found; };
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
                LogLine(std::string("[FPHIDE] FName ") + e.target + (e.found ? " resolved" : " NOT resolved"));
            }
            return e.found ? e.packed : 0;
        }
    }
    return 0;
}

bool SetBoneHiddenSEH(void* mesh, const char* boneName, bool hidden) noexcept
{
    if (!PointerLooksCanonicalAligned(mesh) || boneName == nullptr) return false;
    const std::uint32_t packed = PackedNameCached(boneName);
    if (packed == 0) return false;
    if (hidden)
    {
        static void* fn = nullptr;
        if (fn == nullptr)
        {
            fn = FindFunctionByOuterAndName("SkeletalMeshComponent", "HideBoneByName");
            if (fn != nullptr) LogLine("[FPHIDE] found Function Engine.SkeletalMeshComponent.HideBoneByName");
        }
        struct Params { std::uint32_t BoneName[2]; unsigned char PhysBodyOption; } params = { { packed, 0u }, 0u };
        return ProcessEventSEH(mesh, fn, &params);
    }
    static void* fn = nullptr;
    if (fn == nullptr)
    {
        fn = FindFunctionByOuterAndName("SkeletalMeshComponent", "UnHideBoneByName");
        if (fn != nullptr) LogLine("[FPHIDE] found Function Engine.SkeletalMeshComponent.UnHideBoneByName");
    }
    struct Params { std::uint32_t BoneName[2]; } params = { { packed, 0u } };
    return ProcessEventSEH(mesh, fn, &params);
}

struct HeadMeshGroup
{
    void* part[5];
    int count;
};

bool ReadBioPawnHeadMeshGroupSEH(void* pawn, HeadMeshGroup* group) noexcept
{
    if (group == nullptr) return false;
    *group = {};
    if (!PointerLooksCanonicalAligned(pawn)) return false;
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
            if (!PointerLooksCanonicalAligned(component)) continue;
            bool alreadyAdded = false;
            for (int j = 0; j < group->count; ++j)
                if (group->part[j] == component) { alreadyAdded = true; break; }
            if (!alreadyAdded && group->count < 5)
                group->part[group->count++] = component;
        }
        return group->count > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *group = {};
        return false;
    }
}

// Cheap identity signature of everything FpApplyHides would WRITE to: the pawn, its body mesh, the
// attached actors (weapon/gear), and the head-mesh group - plus the active + head-wanted intent.
// The hide pass is applied ONLY when this changes (2026-07-08 crash fix). Every-frame re-walking wrote
// to volatile combat actors ~60x/sec; one eventually hit a freed-then-reused slot and corrupted an
// unrelated object -> the game AV'd (0xC0000005 read 0x100000000). Edge-driven writes shrink that
// window from thousands of writes to a handful (only on weapon draw/holster/swap + the head-hide flip).
// All sub-reads are individually SEH-guarded; no writes here.
std::uint64_t ComputeFpHideSignature(bool active, bool headWanted) noexcept
{
    std::uint64_t h = 1469598103934665603ull;   // FNV-1a
    auto mix = [&h](std::uint64_t v) { h ^= v; h *= 1099511628211ull; };
    mix(active ? 0xA5ull : 0x5Aull);
    mix(headWanted ? 0x3Cull : 0xC3ull);
    void* pawn = (g_controller != nullptr) ? ReadPossessedPawnSEH(g_controller) : nullptr;
    mix(reinterpret_cast<std::uintptr_t>(pawn));
    if (pawn != nullptr)
    {
        mix(reinterpret_cast<std::uintptr_t>(ReadPawnMeshSEH(pawn)));
        const int ac = ReadPointerArrayCountSEH(pawn, MELEVR::LE1::kActorAttached);
        mix(static_cast<std::uint64_t>(ac));
        for (int i = 0; i < ac && i < 16; ++i)
        {
            void* a = nullptr;
            if (ReadPointerArrayElementSEH(pawn, MELEVR::LE1::kActorAttached, i, &a))
                mix(reinterpret_cast<std::uintptr_t>(a));
        }
        HeadMeshGroup g = {};
        if (ReadBioPawnHeadMeshGroupSEH(pawn, &g))
            for (int i = 0; i < g.count; ++i)
                mix(reinterpret_cast<std::uintptr_t>(g.part[i]));
    }
    return h;
}

// Apply (active=true) or clear (active=false) the FP anti-cull + head hide, re-walking everything
// fresh from the cached controller. Clear writes VANILLA values (idempotent). Returns true if a
// clear pass completed against a readable pawn (so the dirty flag can drop).
std::int32_t FindObjectSlotSEH(const void* obj) noexcept;   // fwd decl (defined in the FP-camera section below)

// Pawn last APPLIED hides to, slot-stamped - cutscenes UNPOSSESS the pawn (controller reads fail),
// which orphaned the clear and left the wireframe head painted through cinematics (2026-07-08). The
// clear path falls back to this cached pawn IF its object-table slot still holds it (alive); a freed
// pawn is abandoned (its components died with it - nothing on screen to clear).
void* g_fpHidePawn = nullptr;
std::int32_t g_fpHidePawnSlot = -1;

bool FpApplyHides(bool active, bool hideHead) noexcept
{
    void* pawn = (g_controller != nullptr) ? ReadPossessedPawnSEH(g_controller) : nullptr;
    if (pawn == nullptr && !active &&
        g_fpHidePawn != nullptr && ObjectTableSlotHoldsSEH(g_fpHidePawnSlot, g_fpHidePawn))
    {
        pawn = g_fpHidePawn;   // clear via the slot-validated cached pawn (cutscene unpossession)
    }
    if (pawn == nullptr) return false;
    if (active && pawn != g_fpHidePawn)
    {
        const std::int32_t slot = FindObjectSlotSEH(pawn);
        if (slot >= 0) { g_fpHidePawn = pawn; g_fpHidePawnSlot = slot; }
    }
    if (!active) { g_fpHidePawn = nullptr; g_fpHidePawnSlot = -1; }

    // Anti-cull: pawn + body mesh + attached actors (weapon). The engine hides these when the camera
    // sits inside the pawn; the IgnoreHiddenActorsMembership bit makes them render anyway.
    if (active) SetActorHiddenSEH(pawn, false);
    MarkActorComponentsIgnoreHidden(pawn, active);
    void* bodyMesh = ReadPawnMeshSEH(pawn);
    if (bodyMesh != nullptr) SetPrimitiveIgnoreHiddenBitSEH(bodyMesh, active);
    const int attachedCount = ReadPointerArrayCountSEH(pawn, MELEVR::LE1::kActorAttached);
    for (int i = 0; i < attachedCount; ++i)
    {
        void* attachedActor = nullptr;
        if (!ReadPointerArrayElementSEH(pawn, MELEVR::LE1::kActorAttached, i, &attachedActor)) continue;
        if (active) SetActorHiddenSEH(attachedActor, false);
        MarkActorComponentsIgnoreHidden(attachedActor, active);
    }

    // Head hide (OwnerNoSee + wireframe + eye/tongue bones - the proven combo). Cleared whenever FP
    // is not active, which is also the fix for the old stuck-wireframe-in-cutscene bug.
    const bool wantHidden = active && hideHead;
    HeadMeshGroup group = {};
    if (ReadBioPawnHeadMeshGroupSEH(pawn, &group))
    {
        for (int i = 0; i < group.count; ++i)
        {
            if (!ComponentLooksLiveSEH(group.part[i])) continue;   // skip stale/garbage mesh pointers
            SetPrimitiveOwnerNoSeeSEH(group.part[i], wantHidden);
            WriteForceWireframeSEH(group.part[i], wantHidden ? 1 : 0);   // restore to 0, never a captured value
        }
    }
    if (bodyMesh != nullptr)
    {
        SetBoneHiddenSEH(bodyMesh, "Eye_Left", wantHidden);
        SetBoneHiddenSEH(bodyMesh, "Eye_Right", wantHidden);
        SetBoneHiddenSEH(bodyMesh, "Tongue", wantHidden);
    }
    return true;
}

bool g_fpHidesDirty = false;   // hides were applied and not yet cleared (cleared via fresh idempotent walk)
int  g_fpHideGrace = 0;        // frames to hold hides through un-classified transition blips
int  g_fpActiveFrames = 0;     // consecutive frames FP has been active (drives the head-hide entry delay)
std::uint64_t g_fpHideSig = 0; // last-applied hide signature; re-apply only when the topology/intent changes

struct FpOwnedMode
{
    void* mode;            // the owned USFXCameraMode instance
    std::int32_t slot;     // its object-table slot at own time (liveness identity for restore)
    void* klass;           // UObject::Class (@0x50) captured at own time - identity for the deep-liveness check
    float origX, origY, origZ;   // vanilla offset captured before the first write
    bool origFp;           // vanilla bFirstPerson bit
};

// Deep liveness: ask the OBJECT about itself, not just the table about the object (reported crash 2026-07-10).
// UE3's GC does NOT null the object-table slot when it frees an object - the stale pointer lingers until the
// slot is reused, so `table[slot] == ptr` passes on a corpse and the write lands in freed-but-committed heap
// (allocator metadata corruption -> delayed __fastfail that bypasses SEH/VEH = the silent crash).
// The allocator stomps a freed block's FIRST bytes (free-list link), which is exactly where the vtable (0x00)
// and the object's self-recorded table index (ObjectInternalInteger @0x08) live. So:
//   1. index round-trip: obj->ObjectInternalInteger must equal the slot it's owned under, AND the table at
//      that slot must still hold this exact pointer (catches freed-and-stomped AND slot-reuse);
//   2. Class pointer equality (@0x50 vs. captured at own time) catches a different object reusing the slot.
// Raw reads only - never ProcessEvent (the SDK IsPendingKill executes script on the object; on a corpse
// that is a second crash). Offsets: LE1-SDK Core_classes.h UObject (vtable 0x00, index 0x08, Class 0x50).
bool FpModeDeepAliveSEH(const void* mode, std::int32_t slot, const void* expectedClass) noexcept
{
    __try
    {
        const auto addr = reinterpret_cast<std::uintptr_t>(mode);
        if (mode == nullptr || (addr & 0x7) != 0 || addr < 0x10000) return false;
        const std::int32_t selfIdx = *reinterpret_cast<const std::int32_t*>(addr + 0x08);   // ObjectInternalInteger
        if (selfIdx != slot) return false;                       // stomped header or relocated = dead
        if (!ObjectTableSlotHoldsSEH(slot, mode)) return false;  // table no longer agrees = dead/reused
        const void* cls = *reinterpret_cast<void* const*>(addr + MELEVR::LE1::kUObjectClass);
        return cls != nullptr && cls == expectedClass;           // different class in the slot = reused
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
constexpr int kFpOwnedCap = 16;
FpOwnedMode g_fpOwned[kFpOwnedCap] = {};
int g_fpOwnedCount = 0;

FpOwnedMode* FindFpOwned(void* mode) noexcept
{
    for (int i = 0; i < g_fpOwnedCount; ++i)
        if (g_fpOwned[i].mode == mode) return &g_fpOwned[i];
    return nullptr;
}

// Object-table slot of a live object (linear scan; runs ONCE per owned mode object per level).
std::int32_t FindObjectSlotSEH(const void* obj) noexcept
{
    __try
    {
        MELEVR::LE1::TArrayHeader* objects = MELEVR::LE1::Objects();
        if (objects == nullptr || objects->data == nullptr || objects->count <= 0 || objects->count > 8'000'000)
            return -1;
        void** data = objects->data;
        const std::int32_t count = objects->count;
        for (std::int32_t i = 0; i < count; ++i)
            if (data[i] == obj) return i;
        return -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

// Active camera-mode object + class name off the cached controller. Pure read, SEH, POD-only.
bool ReadCurrentCameraModeSEH(void** modeOut, char* cls, int cap) noexcept
{
    *modeOut = nullptr;
    cls[0] = '\0';
    if (g_controller == nullptr) return false;
    __try
    {
        void* camera = MELEVR::LE1::ReadPtr(g_controller, MELEVR::LE1::kPlayerCameraPtr);
        if (camera == nullptr) return false;
        void* mode = MELEVR::LE1::ReadPtr(camera, MELEVR::LE1::kCameraCurrentMode);
        if (mode == nullptr || !PointerLooksCanonicalAligned(mode)) return false;
        const char* name = MELEVR::LE1::ClassName(mode);
        if (name == nullptr || name[0] == '\0') return false;
        SafeCopyCString(cls, static_cast<size_t>(cap), name);
        *modeOut = mode;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *modeOut = nullptr;
        cls[0] = '\0';
        return false;
    }
}

bool ReadModeOffsetFpSEH(void* mode, float* x, float* y, float* z, bool* fp) noexcept
{
    __try
    {
        *x = MELEVR::LE1::ReadF32(mode, MELEVR::LE1::kSFXCameraModeOffset + 0);
        *y = MELEVR::LE1::ReadF32(mode, MELEVR::LE1::kSFXCameraModeOffset + 4);
        *z = MELEVR::LE1::ReadF32(mode, MELEVR::LE1::kSFXCameraModeOffset + 8);
        const std::uint32_t flags = static_cast<std::uint32_t>(MELEVR::LE1::ReadI32(mode, MELEVR::LE1::kSFXCameraModeFlags));
        *fp = (flags & MELEVR::LE1::kSFXCameraModeFirstPersonMask) != 0;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool WriteModeOffsetFpSEH(void* mode, float x, float y, float z, bool fp) noexcept
{
    __try
    {
        BYTE* base = reinterpret_cast<BYTE*>(mode);
        *reinterpret_cast<float*>(base + MELEVR::LE1::kSFXCameraModeOffset + 0) = x;
        *reinterpret_cast<float*>(base + MELEVR::LE1::kSFXCameraModeOffset + 4) = y;
        *reinterpret_cast<float*>(base + MELEVR::LE1::kSFXCameraModeOffset + 8) = z;
        std::uint32_t* flags = reinterpret_cast<std::uint32_t*>(base + MELEVR::LE1::kSFXCameraModeFlags);
        if (fp) *flags |= MELEVR::LE1::kSFXCameraModeFirstPersonMask;
        else    *flags &= ~MELEVR::LE1::kSFXCameraModeFirstPersonMask;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Which FP state config (if any) a camera-mode class maps to. Longer/storm names checked FIRST
// ("Combat" is a substring of "CombatStorm"). Aim modes, cover fire modes, Mako, cutscene,
// conversation, transitions: nullptr = stay third-person, never written.
const MELEVR::Config::VrConfig::FpStateCfg* FpStateForClass(const char* cls) noexcept
{
    if (cls == nullptr || cls[0] == '\0') return nullptr;
    const MELEVR::Config::VrConfig& c = MELEVR::Config::Get();
    if (std::strstr(cls, "Vehicle")  != nullptr) return nullptr;   // Mako - NEVER write (heap-corruption lesson)
    if (std::strstr(cls, "Sniper")   != nullptr) return nullptr;   // scoped: leave the game's scope view
    if (std::strstr(cls, "HipAim")   != nullptr) return nullptr;   // blind-fire from cover (cover gates this too)
    if (std::strstr(cls, "TightAim") != nullptr) return &c.fpCombatAim;   // ADS while standing (cover excluded upstream)
    if (std::strstr(cls, "CombatStorm")  != nullptr) return &c.fpCombatStorm;
    if (std::strstr(cls, "ExploreStorm") != nullptr) return &c.fpExploreStorm;
    if (std::strstr(cls, "Combat")  != nullptr) return &c.fpCombat;
    if (std::strstr(cls, "Explore") != nullptr) return &c.fpExplore;
    return nullptr;
}

// Own a mode (capture vanilla once, slot-stamped) and write this frame's FP offset (+ native flag only
// when fpNativeFlag is on - with the flag SET the game's own FP path ignores the offset, sliders dead).
void FpOwnAndWrite(void* mode, const MELEVR::Config::VrConfig::FpStateCfg* st) noexcept
{
    const bool nativeFlag = MELEVR::Config::Get().fpNativeFlag;
    // Pre-write readback: proves in the log whether last frame's write STUCK (before == the target) or the
    // game stomps the offset between frames (before == vanilla-ish every frame). Throttled.
    float bx = 0, by = 0, bz = 0; bool bfp = false;
    const bool readBefore = ReadModeOffsetFpSEH(mode, &bx, &by, &bz, &bfp);
    FpOwnedMode* owned = FindFpOwned(mode);
    if (owned == nullptr)
    {
        if (g_fpOwnedCount >= kFpOwnedCap) return;   // table full - skip (never overwrite entries)
        if (!readBefore) return;
        const std::int32_t slot = FindObjectSlotSEH(mode);
        if (slot < 0) return;   // not in the object table = not a live tracked object; do not touch
        // Capture the Class pointer now (identity for the deep-liveness check on every later write).
        void* klass = MELEVR::LE1::ReadPtr(mode, MELEVR::LE1::kUObjectClass);
        if (klass == nullptr) return;   // can't establish identity = don't own it
        FpOwnedMode& e = g_fpOwned[g_fpOwnedCount++];
        e.mode = mode; e.slot = slot; e.klass = klass;
        e.origX = bx; e.origY = by; e.origZ = bz; e.origFp = bfp;
        owned = &e;
        LogLine("[FPCAM] owned mode ptr slot=" + std::to_string(slot) +
                " vanilla=(" + std::to_string(bx) + "," + std::to_string(by) + "," + std::to_string(bz) +
                ") fp=" + std::to_string(bfp ? 1 : 0));
    }
    // DEEP LIVENESS GUARD before writing (2026-07-10 crash fix, supersedes the 2026-07-08
    // slot-compare-only guard): the table slot DANGLES after a free (GC never nulls it), so the old
    // `table[slot]==ptr` check passed on freed objects and the write corrupted the heap. The deep check
    // makes the object vouch for itself (self-index @0x08 + Class @0x50) - see FpModeDeepAliveSEH.
    if (!FpModeDeepAliveSEH(mode, owned->slot, owned->klass))
    {
        *owned = g_fpOwned[--g_fpOwnedCount];   // swap-remove stale entry
        LogLine("[FPCAM] owned mode failed deep-liveness -> dropped, no write");
        return;
    }
    // Write ONLY when the offset isn't already at target. The game does not stomp it frame-to-frame
    // (readback proved before==target), so in steady FP this writes ~never - another big cut to the
    // live-write surface. A slider change or a game reset makes before != target -> one corrective write.
    const bool needWrite = !readBefore ||
        std::fabs(bx - st->fwd) > 0.01f || std::fabs(by - st->right) > 0.01f ||
        std::fabs(bz - st->up) > 0.01f || (bfp != nativeFlag);
    if (needWrite) WriteModeOffsetFpSEH(mode, st->fwd, st->right, st->up, nativeFlag);
    static uint64_t s_fpWriteLog = 0;
    if ((++s_fpWriteLog % 120ull) == 1ull)
    {
        LogLine("[FPCAM] write before=(" + std::to_string(bx) + "," + std::to_string(by) + "," + std::to_string(bz) +
                ") fpBit=" + std::to_string(bfp ? 1 : 0) +
                " target=(" + std::to_string(st->fwd) + "," + std::to_string(st->right) + "," + std::to_string(st->up) +
                ") nativeFlag=" + std::to_string(nativeFlag ? 1 : 0));
    }
}

// Restore one owned entry IF its slot still holds the same pointer (alive); abandon otherwise.
// Returns true if the entry should be dropped from the table (restored OR dead) - always true.
bool FpRestoreEntry(const FpOwnedMode& e) noexcept
{
    // Same deep-liveness gate as the write path. Restore-writes through a dangling slot during load GC is
    // the exact Therum-exit CTD pattern (see the FP load-crash quarantine notes) - a corpse gets NO restore.
    if (FpModeDeepAliveSEH(e.mode, e.slot, e.klass))
    {
        WriteModeOffsetFpSEH(e.mode, e.origX, e.origY, e.origZ, e.origFp);
    }
    return true;
}

void FpRestoreAll() noexcept
{
    int restored = 0;
    for (int i = 0; i < g_fpOwnedCount; ++i)
        if (FpRestoreEntry(g_fpOwned[i])) ++restored;
    if (g_fpOwnedCount > 0)
        LogLine("[FPCAM] restore-all: " + std::to_string(g_fpOwnedCount) + " owned entries processed");
    g_fpOwnedCount = 0;
}

// Interpolate transitions blend toward the DESTINATION mode's offset - own the destination too (only
// when it classifies as an enabled FP state) so the blend glides straight to the eye instead of
// flashing third-person. Ported from the old OwnInterpolateDestinationSEH with its crash notes.
void* ReadInterpolateToSEH(void* interpolateMode, char* cls, int cap) noexcept
{
    cls[0] = '\0';
    __try
    {
        void* toMode = MELEVR::LE1::ReadPtr(interpolateMode, MELEVR::LE1::kInterpolateToMode);
        if (toMode == nullptr || !PointerLooksCanonicalAligned(toMode) ||
            !IsReadableAddress(toMode, MELEVR::LE1::kSFXCameraModeFlags + sizeof(std::uint32_t)))
            return nullptr;
        const char* name = MELEVR::LE1::ClassName(toMode);
        if (name == nullptr || name[0] == '\0') return nullptr;
        SafeCopyCString(cls, static_cast<size_t>(cap), name);
        return toMode;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        cls[0] = '\0';
        return nullptr;   // stale/garbage To this frame: skip (no flash fix), never crash
    }
}
}  // namespace

namespace MELEVR::HeadAim
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
            LogLine(std::string("[HEADAIM] found live PlayerController: class='") + info.className +
                    "' name='" + info.objectName + "' (camera reachable). Control rotation is now accessible.");
        }
        return true;
    }

    g_controller = nullptr;
    g_controllerIndex = -1;
    g_controllerWasLost = true;   // tripwire: quarantine the next controller found
    return false;
}

void ProbeWeaponGraph() noexcept
{
    if (!EnsureController() || g_controller == nullptr) return;

    static bool s_logged = false;
    static void* s_lastController = nullptr;
    static void* s_lastPawn = nullptr;
    static void* s_lastMesh = nullptr;
    static int s_lastAttachedCount = -1;
    static int s_lastComponentsCount = -1;
    static int s_lastAllComponentsCount = -1;
    static std::uint64_t s_lastAttachedSignature = 0;
    static std::uint64_t s_lastComponentsSignature = 0;
    static std::uint64_t s_lastAllComponentsSignature = 0;

    void* pawn = ReadPossessedPawnSEH(g_controller);
    void* mesh = ReadPawnMeshSEH(pawn);
    PointerArrayView attached = {};
    PointerArrayView components = {};
    PointerArrayView allComponents = {};
    const bool attachedOk = ReadPointerArrayViewSEH(pawn, MELEVR::LE1::kActorAttached, &attached);
    const bool componentsOk = ReadPointerArrayViewSEH(pawn, MELEVR::LE1::kActorComponents, &components);
    const bool allComponentsOk = ReadPointerArrayViewSEH(pawn, MELEVR::LE1::kActorAllComponents, &allComponents);
    const int attachedCount = attachedOk ? attached.count : -1;
    const int componentsCount = componentsOk ? components.count : -1;
    const int allComponentsCount = allComponentsOk ? allComponents.count : -1;
    const std::uint64_t attachedSignature = attachedOk ? PointerArraySignature(attached) : 0;
    const std::uint64_t componentsSignature = componentsOk ? PointerArraySignature(components) : 0;
    const std::uint64_t allComponentsSignature = allComponentsOk ? PointerArraySignature(allComponents) : 0;

    const bool changed = !s_logged || s_lastController != g_controller || s_lastPawn != pawn ||
                         s_lastMesh != mesh || s_lastAttachedCount != attachedCount ||
                         s_lastComponentsCount != componentsCount ||
                         s_lastAllComponentsCount != allComponentsCount ||
                         s_lastAttachedSignature != attachedSignature ||
                         s_lastComponentsSignature != componentsSignature ||
                         s_lastAllComponentsSignature != allComponentsSignature;
    if (!changed) return;

    s_logged = true;
    s_lastController = g_controller;
    s_lastPawn = pawn;
    s_lastMesh = mesh;
    s_lastAttachedCount = attachedCount;
    s_lastComponentsCount = componentsCount;
    s_lastAllComponentsCount = allComponentsCount;
    s_lastAttachedSignature = attachedSignature;
    s_lastComponentsSignature = componentsSignature;
    s_lastAllComponentsSignature = allComponentsSignature;

    LogLine(std::string("[WEAPONPROBE] graph changed controller=") + ObjectPointerText(g_controller) +
            " pawn=" + ObjectPointerText(pawn) + " mesh=" + ObjectPointerText(mesh));
    LogWeaponProbeObject("controller", 0, g_controller);
    LogWeaponProbeObject("pawn", 0, pawn);
    LogWeaponProbeObject("mesh", 0, mesh);
    LogWeaponProbeArray("pawn.attached", pawn, MELEVR::LE1::kActorAttached, 64);
    LogWeaponProbeArray("pawn.components", pawn, MELEVR::LE1::kActorComponents, 64);
    LogWeaponProbeArray("pawn.allComponents", pawn, MELEVR::LE1::kActorAllComponents, 64);
    LogWeaponProbeWeaponCandidates(allComponents);

    PointerArrayView meshAttachments = {};
    if (ReadPointerArrayViewSEH(mesh, MELEVR::LE1::kSkelMeshAttachments, &meshAttachments))
    {
        LogLine(std::string("[WEAPONPROBE] mesh.attachments count=") +
                std::to_string(meshAttachments.count) + " max=" + std::to_string(meshAttachments.max) +
                " (FAttachment layout intentionally not guessed)");
    }
    else
    {
        LogLine("[WEAPONPROBE] mesh.attachments unreadable");
    }
}

bool ControllerStable() noexcept
{
    if (g_controller == nullptr)
    {
        return false;   // nothing resolved; nothing is stable
    }

    const bool identityChanged =
        g_controller != g_stablePrevController || g_controllerIndex != g_stablePrevIndex;
    if (identityChanged || g_controllerWasLost)
    {
        // Load / possession swap / death-and-respawn: abandon-and-wait. The caller
        // must drop cached aim references and must NOT write for the quarantine.
        g_stablePrevController = g_controller;
        g_stablePrevIndex = g_controllerIndex;
        g_controllerWasLost = false;
        g_quarantineCallsLeft = 90;
        LogLine("[HEADAIM] controller transition detected -> 90-frame write quarantine (no ControlRotation writes).");
        return false;
    }

    if (g_quarantineCallsLeft > 0)
    {
        --g_quarantineCallsLeft;
        return false;
    }
    return true;
}

// Weapons-out classifier (restored 2026-07-07; was GameCamera::ReadWeaponMode, lost when game_camera left the
// build). Reads the cached controller's active camera-mode CLASS name; combat/aim modes = weapon out. This is
// the switch that gates head-AIM (weapon out -> HMD drives ControlRotation, camera orbits) vs head-LOOK
// (unarmed -> render-side 6DOF free look). Returns 1 = weapon out, 0 = explore/other, -1 = unreadable.
// PURE READ, SEH-guarded, POD-only inside __try.
int ReadWeaponModeSEH() noexcept
{
    // [ADSSNAP] Last DEFINITIVE weapon state (1/0), held through camera-mode BLENDS. The game transitions
    // between modes (hip<->ADS, draw/holster) via SFXCameraMode_Interpolate for ~100-500ms; classifying that
    // blend as "not out" made head-aim release (a real ControlRotation restore-write) + head-look apply +
    // re-latch on EVERY ADS press AND release = the crosshair visibly snapping to the head for the blend
    // (per the 1->2->3 drawing, 2026-07-13). Log-proven: each [HEADAIM] OFF lands on the exact frame
    // mode='SFXCameraMode_Interpolate' appears; the matching ON lands the frame TightAim/Combat arrives.
    // Holding the last definitive state through the blend is what bdc3c4c's aim driver called "TightAim
    // edge-hold / transition suppress" - dropped in the 2026-07-03 port (weapon reads were dead then),
    // never restored when the reader came back. A blend BETWEEN two definitive states resolves correctly
    // either way: aim->aim = no flap at all; aim->explore = one clean release when Explore actually lands.
    static int s_lastDefinitive = 0;
    if (!EnsureController() || g_controller == nullptr) return -1;
    __try
    {
        void* camera = MELEVR::LE1::ReadPtr(g_controller, MELEVR::LE1::kPlayerCameraPtr);
        if (camera == nullptr) return -1;
        void* mode = MELEVR::LE1::ReadPtr(camera, MELEVR::LE1::kCameraCurrentMode);
        if (mode == nullptr) return -1;
        const char* cls = MELEVR::LE1::ClassName(mode);
        if (cls == nullptr || cls[0] == '\0') return -1;
        if (std::strstr(cls, "Interpolate") != nullptr) return s_lastDefinitive;   // [ADSSNAP] blend = hold state
        // Combat/TightAim = hip/ADS; HipAimCover = blind-fire from cover; Sniper = scoped. All weapon-out.
        // Vehicle (Mako) = ALWAYS armed (turret): its aim follows ControlRotation exactly like on-foot aim, so
        // treat it as an aim context (2026-07-08 fix). Before this, Vehicle read 0 -> head-LOOK moved the view
        // but nothing wrote ControlRotation, so the turret didn't follow the crosshair. NOTE: this only engages
        // head-AIM (the ControlRotation write, hard-gated by EnsureController+ControllerStable); it does NOT
        // touch the vehicle CAMERA MODE object - that 0xB4 write is the one that heap-corrupted entering the
        // Mako and stays untouched (FpStateForClass still excludes Vehicle).
        const bool weaponOut = (std::strstr(cls, "Combat") != nullptr) ||
                               (std::strstr(cls, "TightAim") != nullptr) ||
                               (std::strstr(cls, "HipAimCover") != nullptr) ||
                               (std::strstr(cls, "Sniper") != nullptr) ||
                               (std::strstr(cls, "Vehicle") != nullptr);   // Mako turret aims via ControlRotation
        s_lastDefinitive = weaponOut ? 1 : 0;
        return s_lastDefinitive;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

// (GetPawnWorldAndYawSEH removed 2026-07-18 with CINEFP/CINEDOLLY - its only callers. The pattern
// lives in git if a pawn-position read is ever needed again.)

// Cinematic letterbox DETECTOR (2026-07-08, the "squished cutscenes", v2). The conversation/cutscene
// [NOLETTERBOX 2026-08-09] Clear ACamera.bConstrainAspectRatio for THIS frame, on the game thread,
// BEFORE FViewportClient::Draw consumes it. This is what makes LE1 cine render like LE2/LE3: their
// cine cameras never letterbox, LE1's constrains the render to a 16:9 band inside the target window, and
// every downstream attempt to accommodate that band has failed twice over (crop+stretch broke UI
// shape; 16:9 target made the whole frame anamorphic; FOV widening exceeded the runtime frustum =
// double vision). Kill the cause, not the symptom.
//
// HISTORY: clearing this bit was tried 2026-07-08 and lost the race - but that attempt wrote at
// PRESENT time, after the engine had already consumed the flag that frame (1800+ clears, bars
// stayed). This clear runs inside the FVC Draw detour on the game thread, before the original
// Draw builds its views, which is upstream of consumption. The engine re-sets the bit on its next
// tick; it's re-cleared next frame. Per-frame game-state write, same class as head-aim's
// ControlRotation writes (proven safe for weeks); single dword AND, SEH-guarded.
// Returns 1 = was set, cleared now; 0 = already clear; -1 = unreadable.
int ClearCameraAspectConstraintSEH() noexcept
{
    if (!EnsureController() || g_controller == nullptr) return -1;
    __try
    {
        void* camera = MELEVR::LE1::ReadPtr(g_controller, MELEVR::LE1::kPlayerCameraPtr);
        if (camera == nullptr || !PointerLooksCanonicalAligned(camera)) return -1;
        volatile std::uint32_t* flags = reinterpret_cast<volatile std::uint32_t*>(
            reinterpret_cast<BYTE*>(camera) + MELEVR::LE1::kCameraFlagsWord);
        const std::uint32_t v = *flags;
        if ((v & MELEVR::LE1::kCameraConstrainAspectMask) == 0) return 0;
        *flags = v & ~MELEVR::LE1::kCameraConstrainAspectMask;
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

// MAKO turret BONE-CONTROLLER probe (2026-07-08, READ-ONLY). Walks pawn.Mesh -> Animations(UAnimTree) ->
// SkelControlLists -> every control chain, matches class "TurretConstrained", and logs its ControlName +
// live BoneRotation(pitch,yaw,roll UU) + strength + applyFlags so the reach can be confirmed AND the units read
// while the stick aims the cannon. Zero writes. Bounded walk (16 lists x 32 chain depth).
// POD-only SEH core (no C++ objects -> no C2712). Fills a fixed struct; caller logs outside __try.
struct MakoBoneProbe {
    int   status;      // 0 ok, 1 no pawn, 2 no mesh, 3 no animtree, 4 bad list, 5 seh
    int   listCount;
    int   found;
    char  cls[64];     // class name of the first Turret control
    std::int32_t bp, by, br;   // BoneRotation pitch/yaw/roll (UU)
    std::int32_t dp, dy;       // DesiredBoneRotation pitch/yaw (UU)
    float strength;
    std::uint32_t applyFlags;
};
void ProbeMakoBoneSEH(MakoBoneProbe* o) noexcept
{
    o->status = 5; o->listCount = 0; o->found = 0; o->cls[0] = '\0';
    o->bp = o->by = o->br = o->dp = o->dy = 0; o->strength = 0.0f; o->applyFlags = 0;
    if (!EnsureController() || g_controller == nullptr) { o->status = 1; return; }
    __try
    {
        void* pawn = ReadPossessedPawnSEH(g_controller);
        if (pawn == nullptr || !PointerLooksCanonicalAligned(pawn)) { o->status = 1; return; }
        void* mesh = MELEVR::LE1::ReadPtr(pawn, MELEVR::LE1::kPawnMesh);
        if (mesh == nullptr || !PointerLooksCanonicalAligned(mesh) ||
            !IsReadableAddress(mesh, MELEVR::LE1::kSkelMeshAnimTree + sizeof(void*))) { o->status = 2; return; }
        void* tree = MELEVR::LE1::ReadPtr(mesh, MELEVR::LE1::kSkelMeshAnimTree);
        if (tree == nullptr || !PointerLooksCanonicalAligned(tree) ||
            !IsReadableAddress(tree, MELEVR::LE1::kAnimTreeSkelCtrlLists + 16)) { o->status = 3; return; }
        BYTE* lists = reinterpret_cast<BYTE*>(tree) + MELEVR::LE1::kAnimTreeSkelCtrlLists;
        void* listData = *reinterpret_cast<void**>(lists);
        const std::int32_t listCount = *reinterpret_cast<std::int32_t*>(lists + 8);
        o->listCount = listCount;
        if (listData == nullptr || !PointerLooksCanonicalAligned(listData) || listCount <= 0 || listCount > 256) { o->status = 4; return; }
        o->status = 0;
        for (std::int32_t i = 0; i < listCount && i < 32; ++i)
        {
            BYTE* headSlot = reinterpret_cast<BYTE*>(listData) + static_cast<size_t>(i) * MELEVR::LE1::kSkelCtrlListStride
                             + MELEVR::LE1::kSkelCtrlListHead;
            void* ctrl = *reinterpret_cast<void**>(headSlot);
            for (int depth = 0; depth < 32 && ctrl != nullptr && PointerLooksCanonicalAligned(ctrl); ++depth)
            {
                const char* cls = MELEVR::LE1::ClassName(ctrl);
                if (cls != nullptr && std::strstr(cls, "Turret") != nullptr)
                {
                    if (o->found == 0)   // capture the first one's live values
                    {
                        SafeCopyCString(o->cls, sizeof(o->cls), cls);
                        o->bp = MELEVR::LE1::ReadI32(ctrl, MELEVR::LE1::kSingleBoneRotation + 0);
                        o->by = MELEVR::LE1::ReadI32(ctrl, MELEVR::LE1::kSingleBoneRotation + 4);
                        o->br = MELEVR::LE1::ReadI32(ctrl, MELEVR::LE1::kSingleBoneRotation + 8);
                        o->strength   = MELEVR::LE1::ReadF32(ctrl, MELEVR::LE1::kSkelCtrlStrength);
                        o->applyFlags = static_cast<std::uint32_t>(MELEVR::LE1::ReadI32(ctrl, MELEVR::LE1::kSingleBoneApplyFlags));
                        o->dp = MELEVR::LE1::ReadI32(ctrl, MELEVR::LE1::kTurretDesiredBoneRot + 0);
                        o->dy = MELEVR::LE1::ReadI32(ctrl, MELEVR::LE1::kTurretDesiredBoneRot + 4);
                    }
                    ++o->found;
                }
                ctrl = MELEVR::LE1::ReadPtr(ctrl, MELEVR::LE1::kSkelCtrlNext);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        o->status = 5;
    }
}

// MAKO turret WRITE via the bone controllers (the real fix). For each BioSkelControl_TurretConstrained:
// route head yaw -> the yaw-constrained control's BoneRotation.yaw, head pitch -> the pitch-constrained
// control's BoneRotation.pitch (UU; 65536=360). Write DesiredBoneRotation too so the lag has nothing to
// pull away toward, and re-assert every frame so the game's stick-set Desired can't win. BoneRotation is
// the APPLIED pose -> the cannon follows. class-name gated (only real Turret controls) + SEH = a wrong
// offset writes nothing, never corrupts. Returns controls written (0 = reach failed). POD-only.
int WriteMakoBoneSEH(float headYawDeg, float headPitchDeg) noexcept
{
    if (!EnsureController() || g_controller == nullptr) return 0;
    __try
    {
        void* pawn = ReadPossessedPawnSEH(g_controller);
        if (pawn == nullptr || !PointerLooksCanonicalAligned(pawn)) return 0;
        void* mesh = MELEVR::LE1::ReadPtr(pawn, MELEVR::LE1::kPawnMesh);
        if (mesh == nullptr || !PointerLooksCanonicalAligned(mesh) ||
            !IsReadableAddress(mesh, MELEVR::LE1::kSkelMeshAnimTree + sizeof(void*))) return 0;
        void* tree = MELEVR::LE1::ReadPtr(mesh, MELEVR::LE1::kSkelMeshAnimTree);
        if (tree == nullptr || !PointerLooksCanonicalAligned(tree) ||
            !IsReadableAddress(tree, MELEVR::LE1::kAnimTreeSkelCtrlLists + 16)) return 0;
        BYTE* lists = reinterpret_cast<BYTE*>(tree) + MELEVR::LE1::kAnimTreeSkelCtrlLists;
        void* listData = *reinterpret_cast<void**>(lists);
        const std::int32_t listCount = *reinterpret_cast<std::int32_t*>(lists + 8);
        if (listData == nullptr || !PointerLooksCanonicalAligned(listData) || listCount <= 0 || listCount > 256) return 0;
        auto degToUU = [](float d) -> std::int32_t {
            float u = d * (65536.0f / 360.0f);
            while (u > 32768.0f) u -= 65536.0f; while (u < -32768.0f) u += 65536.0f;
            return static_cast<std::int32_t>(u);
        };
        const std::int32_t yawUU = degToUU(headYawDeg);
        float pc = headPitchDeg; if (pc > 60.0f) pc = 60.0f; if (pc < -60.0f) pc = -60.0f;
        const std::int32_t pitchUU = degToUU(pc);
        int wrote = 0;
        for (std::int32_t i = 0; i < listCount && i < 32; ++i)
        {
            BYTE* headSlot = reinterpret_cast<BYTE*>(listData) + static_cast<size_t>(i) * MELEVR::LE1::kSkelCtrlListStride
                             + MELEVR::LE1::kSkelCtrlListHead;
            void* ctrl = *reinterpret_cast<void**>(headSlot);
            for (int depth = 0; depth < 32 && ctrl != nullptr && PointerLooksCanonicalAligned(ctrl); ++depth)
            {
                const char* cls = MELEVR::LE1::ClassName(ctrl);
                if (cls != nullptr && std::strstr(cls, "Turret") != nullptr)
                {
                    const std::uint32_t cf = static_cast<std::uint32_t>(MELEVR::LE1::ReadI32(ctrl, MELEVR::LE1::kTurretConstrainFlags));
                    BYTE* bone = reinterpret_cast<BYTE*>(ctrl) + MELEVR::LE1::kSingleBoneRotation;      // FRotator pitch@0,yaw@4,roll@8
                    BYTE* des  = reinterpret_cast<BYTE*>(ctrl) + MELEVR::LE1::kTurretDesiredBoneRot;
                    // Route by which axis the control constrains (0x2=yaw, 0x1=pitch). Write both fields so
                    // the lag target == applied == head; re-asserted every frame.
                    if (cf & 0x2u) { *reinterpret_cast<std::int32_t*>(bone + 4) = yawUU;   *reinterpret_cast<std::int32_t*>(des + 4) = yawUU; }
                    if (cf & 0x1u) { *reinterpret_cast<std::int32_t*>(bone + 0) = pitchUU; *reinterpret_cast<std::int32_t*>(des + 0) = pitchUU; }
                    // If a control constrains NEITHER flag (some rigs), fall back to writing yaw (the dominant axis).
                    if ((cf & 0x3u) == 0) { *reinterpret_cast<std::int32_t*>(bone + 4) = yawUU; *reinterpret_cast<std::int32_t*>(des + 4) = yawUU; }
                    std::uint32_t* af = reinterpret_cast<std::uint32_t*>(reinterpret_cast<BYTE*>(ctrl) + MELEVR::LE1::kSingleBoneApplyFlags);
                    *af |= MELEVR::LE1::kSingleBoneApplyRotMask;
                    *reinterpret_cast<float*>(reinterpret_cast<BYTE*>(ctrl) + MELEVR::LE1::kSkelCtrlStrength) = 1.0f;
                    ++wrote;
                }
                ctrl = MELEVR::LE1::ReadPtr(ctrl, MELEVR::LE1::kSkelCtrlNext);
            }
        }
        return wrote;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

void ProbeMakoTurretBone() noexcept
{
    static uint64_t s_n = 0;
    if ((s_n++ % 30ull) != 0) return;   // ~2/s
    MakoBoneProbe p = {};
    ProbeMakoBoneSEH(&p);
    if (p.status != 0)
    {
        const char* why = p.status == 1 ? "no pawn" : p.status == 2 ? "no mesh"
                        : p.status == 3 ? "no animtree" : p.status == 4 ? "bad list" : "SEH fault";
        LogLine(std::string("[MAKOBONE] ") + why + " (listCount=" + std::to_string(p.listCount) + ")");
        return;
    }
    if (p.found == 0)
    {
        LogLine("[MAKOBONE] walked " + std::to_string(p.listCount) + " lists, NO Turret control found");
        return;
    }
    LogLine(std::string("[MAKOBONE] found=") + std::to_string(p.found) + " class=" + p.cls +
            " BoneRot(p,y,r)=(" + std::to_string(p.bp) + "," + std::to_string(p.by) + "," + std::to_string(p.br) + ")" +
            " Desired(p,y)=(" + std::to_string(p.dp) + "," + std::to_string(p.dy) + ")" +
            " strength=" + std::to_string(p.strength) + " applyFlags=0x" + std::to_string(p.applyFlags));
}

// MAKO turret probe (2026-07-08, READ-ONLY step 1). Walks possessedPawn -> behavior(0x820) ->
// appearance(0x1CC) -> turret TArray(0x7C) and reads element[0..n] yaw/pitch/flags so the offset chain
// resolution can be confirmed + the UNITS seen before ever writing (the whole crash history here is blind live writes).
// Pure reads, every hop null+canonical+readable guarded, SEH around the derefs. Returns element count read
// (0 = chain didn't resolve). Fills up to `cap` elements.
struct MakoTurretInfo { float yaw; float pitch; std::uint32_t flags; };
int ReadMakoTurretSEH(MakoTurretInfo* out, int cap, void** appearanceOut) noexcept
{
    if (appearanceOut) *appearanceOut = nullptr;
    if (!EnsureController() || g_controller == nullptr) return 0;
    __try
    {
        void* pawn = ReadPossessedPawnSEH(g_controller);
        if (pawn == nullptr || !PointerLooksCanonicalAligned(pawn)) return 0;
        void* behavior = MELEVR::LE1::ReadPtr(pawn, MELEVR::LE1::kBioVehicleBehavior);
        if (behavior == nullptr || !PointerLooksCanonicalAligned(behavior) ||
            !IsReadableAddress(behavior, MELEVR::LE1::kBioBehaviorAppearance + sizeof(void*))) return 0;
        void* appearance = MELEVR::LE1::ReadPtr(behavior, MELEVR::LE1::kBioBehaviorAppearance);
        if (appearance == nullptr || !PointerLooksCanonicalAligned(appearance) ||
            !IsReadableAddress(appearance, MELEVR::LE1::kAppearanceTurretArray + 16)) return 0;
        if (appearanceOut) *appearanceOut = appearance;
        // TArray at +0x7C: data ptr @ +0x00, count @ +0x08 (x64 layout).
        BYTE* arr = reinterpret_cast<BYTE*>(appearance) + MELEVR::LE1::kAppearanceTurretArray;
        void* data = *reinterpret_cast<void**>(arr);
        const std::int32_t count = *reinterpret_cast<std::int32_t*>(arr + 8);
        if (data == nullptr || !PointerLooksCanonicalAligned(data) || count <= 0 || count > 8) return 0;
        const size_t bytes = static_cast<size_t>(count) * MELEVR::LE1::kTurretInfoStride;
        if (!IsReadableAddress(data, bytes)) return 0;
        const int n = (count < cap) ? count : cap;
        for (int i = 0; i < n; ++i)
        {
            BYTE* e = reinterpret_cast<BYTE*>(data) + static_cast<size_t>(i) * MELEVR::LE1::kTurretInfoStride;
            out[i].yaw   = *reinterpret_cast<float*>(e + MELEVR::LE1::kTurretInfoYaw);
            out[i].pitch = *reinterpret_cast<float*>(e + MELEVR::LE1::kTurretInfoPitch);
            out[i].flags = *reinterpret_cast<std::uint32_t*>(e + MELEVR::LE1::kTurretInfoFlags);
        }
        return n;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// Engine game-mode context (2026-07-08). Reads USFXGameModeManager::CurrentMode off the cached
// BioPlayerController: one pointer hop + one byte. This is the authoritative UI-context enum
// (EGameModes) - see le1_game.h for the value table. Used to force full-screen menus (mode 7 = GUI)
// to flat mono in VR. Returns the mode byte 0..12, or -1 if unreadable (caller fails OPEN to VR so a
// bad read can never latch mono into live gameplay). Pure READ, SEH-guarded, POD-only inside __try.
// MAKO turret WRITE (step 2, experimental). Clears the auto-track bits (flags &= ~0x3) and writes the
// yaw/pitch into element[0] so the cannon follows a commanded angle instead of the game's aim path.
// Units UNVERIFIED (probe read 0) -> caller logs the readback. restoreAutoTrack=true just re-sets the bits
// (stand-down) and writes nothing else. All hops guarded, SEH around derefs.
bool WriteMakoTurretSEH(float yaw, float pitch, bool restoreAutoTrack) noexcept
{
    if (!EnsureController() || g_controller == nullptr) return false;
    __try
    {
        void* pawn = ReadPossessedPawnSEH(g_controller);
        if (pawn == nullptr || !PointerLooksCanonicalAligned(pawn)) return false;
        void* behavior = MELEVR::LE1::ReadPtr(pawn, MELEVR::LE1::kBioVehicleBehavior);
        if (behavior == nullptr || !PointerLooksCanonicalAligned(behavior) ||
            !IsReadableAddress(behavior, MELEVR::LE1::kBioBehaviorAppearance + sizeof(void*))) return false;
        void* appearance = MELEVR::LE1::ReadPtr(behavior, MELEVR::LE1::kBioBehaviorAppearance);
        if (appearance == nullptr || !PointerLooksCanonicalAligned(appearance) ||
            !IsReadableAddress(appearance, MELEVR::LE1::kAppearanceTurretArray + 16)) return false;
        BYTE* arrb = reinterpret_cast<BYTE*>(appearance) + MELEVR::LE1::kAppearanceTurretArray;
        void* data = *reinterpret_cast<void**>(arrb);
        const std::int32_t count = *reinterpret_cast<std::int32_t*>(arrb + 8);
        if (data == nullptr || !PointerLooksCanonicalAligned(data) || count <= 0 || count > 8) return false;
        if (!IsReadableAddress(data, MELEVR::LE1::kTurretInfoStride)) return false;   // element[0] = cannon
        BYTE* e = reinterpret_cast<BYTE*>(data);
        std::uint32_t* flags = reinterpret_cast<std::uint32_t*>(e + MELEVR::LE1::kTurretInfoFlags);
        if (restoreAutoTrack) { *flags |= 0x3u; return true; }   // stand down: hand the turret back to the game
        *flags &= ~0x3u;                                          // take manual control (stop auto-track overwrite)
        *reinterpret_cast<float*>(e + MELEVR::LE1::kTurretInfoYaw)   = yaw;
        *reinterpret_cast<float*>(e + MELEVR::LE1::kTurretInfoPitch) = pitch;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void ProbeMakoTurret() noexcept
{
    MakoTurretInfo info[4] = {};
    void* appearance = nullptr;
    const int n = ReadMakoTurretSEH(info, 4, &appearance);
    static uint64_t s_probeN = 0;
    if ((s_probeN++ % 30ull) != 0) return;   // ~2/s
    if (n <= 0)
    {
        LogLine("[MAKOTURRET] chain unresolved (n=0) appearance=" +
                std::to_string(reinterpret_cast<std::uintptr_t>(appearance)));
        return;
    }
    std::string s = "[MAKOTURRET] count=" + std::to_string(n);
    for (int i = 0; i < n; ++i)
        s += " [" + std::to_string(i) + "] yaw=" + std::to_string(info[i].yaw) +
             " pitch=" + std::to_string(info[i].pitch) +
             " flags=0x" + std::to_string(info[i].flags);
    LogLine(s);
}

// Write the LOGICAL turret angle (m_fCurrentTurretRelativeYaw/Pitch, degrees rel body) the FIRE TRACE
// reads - the visual bone follows head but the shot used the game's stick angle (2026-07-08). Auto-track
// LEFT ON (clearing it froze the turret system); it's re-asserted every frame so the trace uses the injected angle.
// Returns true if written.
bool WriteMakoTurretRelativeDegSEH(float yawDeg, float pitchDeg) noexcept
{
    if (!EnsureController() || g_controller == nullptr) return false;
    __try
    {
        void* pawn = ReadPossessedPawnSEH(g_controller);
        if (pawn == nullptr || !PointerLooksCanonicalAligned(pawn)) return false;
        void* behavior = MELEVR::LE1::ReadPtr(pawn, MELEVR::LE1::kBioVehicleBehavior);
        if (behavior == nullptr || !PointerLooksCanonicalAligned(behavior) ||
            !IsReadableAddress(behavior, MELEVR::LE1::kBioBehaviorAppearance + sizeof(void*))) return false;
        void* appearance = MELEVR::LE1::ReadPtr(behavior, MELEVR::LE1::kBioBehaviorAppearance);
        if (appearance == nullptr || !PointerLooksCanonicalAligned(appearance) ||
            !IsReadableAddress(appearance, MELEVR::LE1::kAppearanceTurretArray + 16)) return false;
        BYTE* arrb = reinterpret_cast<BYTE*>(appearance) + MELEVR::LE1::kAppearanceTurretArray;
        void* data = *reinterpret_cast<void**>(arrb);
        const std::int32_t count = *reinterpret_cast<std::int32_t*>(arrb + 8);
        if (data == nullptr || !PointerLooksCanonicalAligned(data) || count <= 0 || count > 8) return false;
        if (!IsReadableAddress(data, MELEVR::LE1::kTurretInfoStride)) return false;
        BYTE* e = reinterpret_cast<BYTE*>(data);   // turret 0 = cannon; leave flags (auto-track) untouched
        *reinterpret_cast<float*>(e + MELEVR::LE1::kTurretInfoYaw)   = yawDeg;
        *reinterpret_cast<float*>(e + MELEVR::LE1::kTurretInfoPitch) = pitchDeg;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Public Mako bone head-aim entry: write head yaw/pitch into the turret bone controllers (visual) AND the
// logical relative angle (fire trace), log ~2/s. Returns bone controls written (0 = reach failed).
int DriveMakoTurretBone(float headYawDeg, float headPitchDeg) noexcept
{
    const int n = WriteMakoBoneSEH(headYawDeg, headPitchDeg);
    const bool rel = WriteMakoTurretRelativeDegSEH(headYawDeg, headPitchDeg);
    static uint64_t s_n = 0;
    if ((s_n++ % 30ull) == 0)
        LogLine("[MAKOBONE] WRITE controls=" + std::to_string(n) + " rel=" + std::to_string(rel ? 1 : 0) +
                " head=(" + std::to_string(headYawDeg) + "," + std::to_string(headPitchDeg) + ")");
    return n;
}

// Mako head-aim STEP 3: drive the turret through the game's OWN aim function. The relative-field write
// (step 2) proved dead - it stuck but the bone stayed frozen. The cannon bone is driven by the game from
// a WORLD-SPACE aim point via UBioVehicleBehaviorBase::SetDesiredTurretAimPoint(idx, point). So: leave
// auto-track ON and every frame feed it a point built from the head ray (Mako world pos + head-relative
// direction, far out). The Present-time call lands after the game's stick-driven call -> it wins.
// POD-only inside __try; ProcessEventSEH has its own SEH. Fills outPt/outRet for logging OUTSIDE __try.
constexpr float kPiF = 3.14159265358979f;
bool CallSetTurretAimSEH(void* fn, float headYawDeg, float headPitchDeg,
                         float outPt[3], std::int32_t outRet[3], float* outBodyYawDeg) noexcept
{
    if (!EnsureController() || g_controller == nullptr) return false;
    __try
    {
        void* pawn = ReadPossessedPawnSEH(g_controller);
        if (pawn == nullptr || !PointerLooksCanonicalAligned(pawn)) return false;
        void* behavior = MELEVR::LE1::ReadPtr(pawn, MELEVR::LE1::kBioVehicleBehavior);
        if (behavior == nullptr || !PointerLooksCanonicalAligned(behavior)) return false;
        const float lx = MELEVR::LE1::ReadF32(pawn, MELEVR::LE1::kActorLocation + 0);
        const float ly = MELEVR::LE1::ReadF32(pawn, MELEVR::LE1::kActorLocation + 4);
        const float lz = MELEVR::LE1::ReadF32(pawn, MELEVR::LE1::kActorLocation + 8);
        const std::int32_t bodyYawUU = MELEVR::LE1::ReadI32(pawn, MELEVR::LE1::kActorRotation + MELEVR::LE1::kRotYaw);
        const float bodyYawDeg = static_cast<float>(bodyYawUU) * (360.0f / 65536.0f);
        float wp = headPitchDeg; if (wp > 80.0f) wp = 80.0f; if (wp < -80.0f) wp = -80.0f;
        const float yr = (bodyYawDeg + headYawDeg) * (kPiF / 180.0f);
        const float pr = wp * (kPiF / 180.0f);
        const float cp = std::cos(pr);
        const float D = 100000.0f;   // far point along the head ray (UE3 left-handed: X fwd, Y right, Z up)
        // FTPOV/params: SetDesiredTurretAimPoint(int nTurretIndex, FVector vTargetLocation) -> FRotator
        struct Params { std::int32_t idx; float x, y, z; std::int32_t retPitch, retYaw, retRoll; } p = {};
        p.idx = 0;   // turret 0 = cannon
        p.x = lx + cp * std::cos(yr) * D;
        p.y = ly + cp * std::sin(yr) * D;
        p.z = lz + std::sin(pr) * D;
        const bool ok = ProcessEventSEH(behavior, fn, &p);
        outPt[0] = p.x; outPt[1] = p.y; outPt[2] = p.z;
        outRet[0] = p.retPitch; outRet[1] = p.retYaw; outRet[2] = p.retRoll;
        if (outBodyYawDeg) *outBodyYawDeg = bodyYawDeg;
        return ok;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Public Mako head-aim entry (step 3). active=true: feed the game's turret-aim function a head-ray world
// point (auto-track left ON so the game drives the bone toward it). active=false: no-op (auto-track never
// touched). Logs [MAKOTURRET] aim ~2/s.
void DriveMakoTurret(bool active, float headYawDeg, float headPitchDeg) noexcept
{
    if (!active) return;
    static void* s_fn = nullptr; static bool s_tried = false;
    if (!s_tried)
    {
        s_fn = FindFunctionByOuterAndName("BioVehicleBehaviorBase", "SetDesiredTurretAimPoint");
        s_tried = true;
        LogLine(std::string("[MAKOTURRET] SetDesiredTurretAimPoint fn=") + (s_fn ? "FOUND" : "NOT FOUND"));
    }
    if (s_fn == nullptr) return;
    // Ensure auto-track is ON (a prior step-2 session may have cleared it) so the game applies the aim point.
    WriteMakoTurretSEH(0, 0, /*restoreAutoTrack*/true);
    float pt[3] = {}; std::int32_t ret[3] = {}; float bodyYaw = 0.0f;
    const bool ok = CallSetTurretAimSEH(s_fn, headYawDeg, headPitchDeg, pt, ret, &bodyYaw);
    static uint64_t s_n = 0;
    if ((s_n++ % 30ull) == 0)
        LogLine("[MAKOTURRET] aim ok=" + std::to_string(ok ? 1 : 0) +
                " head=(" + std::to_string(headYawDeg) + "," + std::to_string(headPitchDeg) + ")" +
                " bodyYaw=" + std::to_string(bodyYaw) +
                " pt=(" + std::to_string(pt[0]) + "," + std::to_string(pt[1]) + "," + std::to_string(pt[2]) + ")" +
                " ret=(" + std::to_string(ret[0]) + "," + std::to_string(ret[1]) + "," + std::to_string(ret[2]) + ")");
}

int ReadGameModeSEH() noexcept
{
    if (!EnsureController() || g_controller == nullptr) return -1;
    __try
    {
        void* gmm = MELEVR::LE1::ReadPtr(g_controller, MELEVR::LE1::kSFXGameModeManager);
        if (gmm == nullptr) return -1;
        const std::int32_t raw = MELEVR::LE1::ReadI32(gmm, MELEVR::LE1::kGameModeCurrentMode);
        return raw & 0xFF;   // CurrentMode is a single byte
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return -1;
    }
}

// [LIVEGUI2 2026-07-17] World-state telemetry for the mode-7 disambiguation (see head_aim.h). First
// attempt (pawn + Pauser + TimeSeconds advancing) was FALSIFIED in-headset the same day: ME1's in-game
// ESC menu keeps Pauser null, keeps the sim clock advancing, and keeps rendering the (visually frozen)
// world - it fingerprinted identically to the live bomb-disarm overlay. This wider read feeds the
// [LIVEGUI2] log lines so the real discriminator comes from measurement, not another guess.
// Pure reads, POD-only inside __try.
bool ReadWorldTelemetrySEH(WorldTelemetry* out) noexcept
{
    if (out == nullptr) return false;
    out->hasPawn = false; out->pauserSet = false;
    out->playersOnly = false; out->playersOnlyPending = false;
    out->timeDilation = 0.0f; out->timeSeconds = 0.0f;
    out->realTimeSeconds = 0.0f; out->deltaSeconds = 0.0f;
    if (!EnsureController() || g_controller == nullptr) return false;
    __try
    {
        void* pawn = MELEVR::LE1::ReadPtr(g_controller, MELEVR::LE1::kControllerPawn);
        if (pawn == nullptr)
            pawn = MELEVR::LE1::ReadPtr(g_controller, MELEVR::LE1::kPlayerAcknowledgedPawn);
        void* worldInfo = MELEVR::LE1::ReadPtr(g_controller, MELEVR::LE1::kActorWorldInfo);
        if (worldInfo == nullptr) return false;
        const void* pauser = MELEVR::LE1::ReadPtr(worldInfo, MELEVR::LE1::kWorldInfoPauser);
        const float dilation = MELEVR::LE1::ReadF32(worldInfo, MELEVR::LE1::kWorldInfoTimeDilation);
        const float timeSec = MELEVR::LE1::ReadF32(worldInfo, MELEVR::LE1::kWorldInfoTimeSeconds);
        const float realSec = MELEVR::LE1::ReadF32(worldInfo, MELEVR::LE1::kWorldInfoRealTimeSeconds);
        const float deltaSec = MELEVR::LE1::ReadF32(worldInfo, MELEVR::LE1::kWorldInfoDeltaSeconds);
        const std::uint32_t flags = static_cast<std::uint32_t>(
            MELEVR::LE1::ReadI32(worldInfo, MELEVR::LE1::kWorldInfoFlagsWord790));
        if (!std::isfinite(timeSec) || !std::isfinite(dilation)) return false;
        out->hasPawn = (pawn != nullptr);
        out->pauserSet = (pauser != nullptr);
        out->playersOnly = (flags & MELEVR::LE1::kWorldInfoPlayersOnlyMask) != 0;
        out->playersOnlyPending = (flags & MELEVR::LE1::kWorldInfoPlayersOnlyPendingMask) != 0;
        out->timeDilation = dilation;
        out->timeSeconds = timeSec;
        out->realTimeSeconds = std::isfinite(realSec) ? realSec : 0.0f;
        out->deltaSeconds = std::isfinite(deltaSec) ? deltaSec : 0.0f;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// Active camera-mode CLASS name (restored 2026-07-07; was GameCamera::ReadCameraSnapshot). Used to detect
// conversations (BioCameraBehaviorConversation) and cutscenes (SFXCameraMode_Cinematic) so the flat/mono +
// disable-head-tracking policy can re-activate. Pure READ, SEH-guarded, POD-only inside __try. Returns true +
// fills 'out' with the class name; false + empty 'out' if unreadable.
bool GetCameraModeNameSEH(char* out, int cap) noexcept
{
    if (out == nullptr || cap <= 0) return false;
    out[0] = '\0';
    if (!EnsureController() || g_controller == nullptr) return false;
    __try
    {
        void* camera = MELEVR::LE1::ReadPtr(g_controller, MELEVR::LE1::kPlayerCameraPtr);
        if (camera == nullptr) return false;
        void* mode = MELEVR::LE1::ReadPtr(camera, MELEVR::LE1::kCameraCurrentMode);
        if (mode == nullptr) return false;
        const char* cls = MELEVR::LE1::ClassName(mode);
        if (cls == nullptr || cls[0] == '\0') return false;
        int i = 0;
        for (; i + 1 < cap && cls[i] != '\0'; ++i) out[i] = cls[i];
        out[i] = '\0';
        return i > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out[0] = '\0';
        return false;
    }
}

bool FirstPersonOwnsAny() noexcept
{
    return g_fpOwnedCount > 0;
}

// Per-frame first-person camera tick (Present thread, after EnsureController/ControllerStable ran).
// fpEnabled off: restore-once (slot-liveness-gated writes) and stand down. On: own + write the active
// mode when it maps to an enabled FP state; restore-and-drop a mode that's owned the instant it stops
// classifying (state toggled off / feature narrowed). Quarantine (ctrlStable false): NO writes at all,
// table kept - entries self-validate via slot identity when next touched.
void FirstPersonTick(bool ctrlLive, bool ctrlStable, int gameMode) noexcept
{
    const MELEVR::Config::VrConfig& cfg = MELEVR::Config::Get();
    // SAVE-SAFETY (2026-07-09): while the pause/GUI menu is up (gameMode 7 - the screen the user saves
    // from), stand FP fully DOWN: restore the vanilla camera-mode offsets + clear the head/anti-cull
    // hides, so a save written from the menu can never capture FP camera state. cfg.fpEnabled is LEFT
    // UNTOUCHED, so FP resumes automatically when the menu closes. (ME1 doesn't serialize camera-mode
    // template floats into a save anyway; this makes the "never save in first person" guarantee airtight
    // at the exact moment a save can be triggered, and keeps the save/menu backdrop in third person.)
    const bool inSaveMenu = (gameMode == 7);
    if (!cfg.fpEnabled || inSaveMenu)
    {
        if (g_fpOwnedCount > 0) FpRestoreAll();
        if (g_fpHidesDirty && ctrlLive)
        {
            if (FpApplyHides(false, false)) { g_fpHidesDirty = false; LogLine(inSaveMenu ? "[FPHIDE] cleared (save menu - FP stood down)" : "[FPHIDE] cleared (fp disabled)"); }
        }
        g_fpHideGrace = 0;
        return;
    }
    if (!ctrlLive || !ctrlStable)
    {
        // Controller gate down (cutscene unpossession / load quarantine). Camera writes stop, but a
        // PENDING HIDE CLEAR must still run or Shepard plays the cinematic with a wireframe head
        // (2026-07-08): after the short grace (rides out 1-frame blips), the clear walks the
        // slot-validated cached pawn - never a stale pointer.
        if (g_fpHidesDirty)
        {
            if (g_fpHideGrace > 0) { --g_fpHideGrace; }
            else if (FpApplyHides(false, false))
            {
                g_fpHidesDirty = false;
                g_fpHideSig = 0;
                g_fpActiveFrames = 0;
                LogLine("[FPHIDE] cleared (controller gate down - cutscene/transition)");
            }
        }
        return;
    }

    void* mode = nullptr;
    char cls[64] = {};
    if (!ReadCurrentCameraModeSEH(&mode, cls, sizeof(cls)))
    {
        // Camera-mode read failed ('?' frames, common inside cutscenes). Same rule: pending hides
        // clear after grace so a cinematic never plays against a wireframe head.
        if (g_fpHidesDirty)
        {
            if (g_fpHideGrace > 0) { --g_fpHideGrace; }
            else if (FpApplyHides(false, false))
            {
                g_fpHidesDirty = false;
                g_fpHideSig = 0;
                g_fpActiveFrames = 0;
                LogLine("[FPHIDE] cleared (camera-mode unreadable)");
            }
        }
        return;
    }

    // Resolve whether first person is ACTIVE this frame (drives both the camera write and the hides).
    bool fpActiveNow = false;

    // COVER = third person (2026-07-08: hugging a wall shouldn't flip to FP, and popping out to
    // aim shouldn't flash the hidden head). Cover shares the Combat mode object, so the pawn flag is the
    // only tell. When in cover this does NOT own the mode (release if it already did) -> the game's own cover
    // camera + full 3rd-person body, and no head hide toggling on the pop-out.
    const bool inCover = ReadPawnInCoverSEH(g_controller);

    if (std::strstr(cls, "Interpolate") != nullptr)
    {
        // Blend in progress: own the DESTINATION if (and only if) it is an enabled FP state and not cover.
        char toCls[64] = {};
        void* toMode = ReadInterpolateToSEH(mode, toCls, sizeof(toCls));
        if (toMode != nullptr && !inCover)
        {
            const MELEVR::Config::VrConfig::FpStateCfg* toSt = FpStateForClass(toCls);
            if (toSt != nullptr && toSt->on)
            {
                FpOwnAndWrite(toMode, toSt);
                fpActiveNow = true;   // blending INTO an FP state: keep hides up through the blend
            }
        }
    }
    else
    {
        const MELEVR::Config::VrConfig::FpStateCfg* st = inCover ? nullptr : FpStateForClass(cls);
        if (st != nullptr && st->on)
        {
            FpOwnAndWrite(mode, st);
            fpActiveNow = true;
        }
        else if (FpOwnedMode* owned = FindFpOwned(mode))
        {
            // Active (so definitely alive) but no longer an FP state (or now in cover) -> restore + drop.
            WriteModeOffsetFpSEH(mode, owned->origX, owned->origY, owned->origZ, owned->origFp);
            *owned = g_fpOwned[--g_fpOwnedCount];   // swap-remove
            LogLine(std::string("[FPCAM] released mode '") + cls + "'" + (inCover ? " (in cover)" : " (state off)"));
        }
    }

    // ---- Hides / anti-cull (M2): stateless per-frame apply + clear -------------------------------
    // Grace hold: brief un-classified transition frames (ZoomSnap etc.) keep the hides up so Shepard
    // doesn't flash visible mid-animation. HARD exits (cutscene/convo/vehicle/galaxy) clear instantly
    // - this is what fixes the old stuck-wireframe-on-cinematic-entry bug.
    const bool hardExit = inCover ||   // cover is deliberately 3rd person: clear hides at once, no lingering
                          std::strstr(cls, "Cinematic") != nullptr ||
                          std::strstr(cls, "Conversation") != nullptr ||
                          std::strstr(cls, "Vehicle") != nullptr ||
                          std::strstr(cls, "Galaxy") != nullptr;
    // (The [CINEFP] keep-hides-up-in-cine branch lived here; removed 2026-07-18 with the FP-cine
    // feature. The hard-exit clear above now always wins in cine - no wireframe/hidden head there.)
    if (fpActiveNow)
    {
        if (g_fpActiveFrames < 100000) ++g_fpActiveFrames;
        // Head hide is DELAYED after ENTERING fp (leaving cover hid the head before the camera
        // finished sliding to the eye). Anti-cull (body/weapon visible) applies immediately - only the
        // head hide waits out fpHeadHideDelay frames so the blend lands first. Re-entry only: staying in
        // fp (Combat<->Explore) keeps the counter high, so drawing/holstering never re-shows the head.
        const int delay = (cfg.fpHeadHideDelay > 0.0f) ? static_cast<int>(cfg.fpHeadHideDelay) : 0;
        const bool headNow = cfg.fpHideHead && (g_fpActiveFrames > delay);
        // EDGE-DRIVEN (2026-07-08 crash fix): only touch game visibility when the topology/intent
        // actually changes (weapon draw/holster/swap, or the head-hide flip). Steady combat = no writes.
        const std::uint64_t sig = ComputeFpHideSignature(true, headNow);
        if (sig != g_fpHideSig)
        {
            if (FpApplyHides(true, headNow))
            {
                if (!g_fpHidesDirty) LogLine("[FPHIDE] applied (anti-cull on; head hide "
                                             + std::string(cfg.fpHideHead ? "after delay" : "off") + ")");
                g_fpHidesDirty = true;
                g_fpHideSig = sig;
            }
        }
        g_fpHideGrace = 45;   // ~0.75s at 60fps
    }
    else if (g_fpHidesDirty)
    {
        g_fpActiveFrames = 0;
        if (!hardExit && g_fpHideGrace > 0)
        {
            --g_fpHideGrace;   // transition blip: hold current hides, no writes either way
        }
        else if (FpApplyHides(false, false))
        {
            g_fpHidesDirty = false;
            g_fpHideGrace = 0;
            g_fpHideSig = 0;
            LogLine(std::string("[FPHIDE] cleared (mode '") + cls + "')");
        }
    }
    else
    {
        g_fpActiveFrames = 0;
    }
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
}  // namespace MELEVR::HeadAim
