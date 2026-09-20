#include "pchud.h"

#include "logger.h"

#include <Windows.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

// Ported from M8NativeSplitXR/process_event_hud_probe.cpp (proven). Self-contained: all object-table /
// function-resolution / SEH helpers live in this anonymous namespace (this deliberately does NOT reach into
// head_aim.cpp). Logging routed through MELEVR::Logger::LogLine. Settings come from SetTransform (not a
// file poll like the reference). CRITICAL: when disabled, ZERO ProcessEvent calls are made; the thread-local
// reentrancy guard makes the hook ignore its own SetVariableFloat calls.

using MELEVR::Logger::LogLine;
using MELEVR::Pchud::HudElemState;
using MELEVR::Pchud::kHudElemCount;
using MELEVR::Pchud::kConvoElemCount;

namespace
{
constexpr std::uintptr_t kNamePoolsOffset = 0x16A2090;
constexpr std::uintptr_t kGObjObjectsOffset = 0x1770670;
constexpr std::uintptr_t kProcessEventVtableOffset = 0x250;
constexpr std::uintptr_t kUObjectClass = 0x50;
constexpr std::uintptr_t kUObjectName = 0x48;
constexpr std::uintptr_t kFNameEntryString = 0x0C;
constexpr std::uintptr_t kPchudHandlerPanel = 0x80;
constexpr int32_t kProcessEventTargetScanLimit = 200000;

using ProcessEventFn = void(__fastcall*)(void*, void*, void*, void*);
using NativeSubtitleProbeFn = std::uintptr_t(__fastcall*)(void*, void*, void*, void*, void*, void*, void*, void*);

struct NativeSubtitleProbeSpec
{
    const char* tag = "";
    std::uintptr_t rva = 0;
};

struct ObjectArrayHeader
{
    void** data = nullptr;
    int32_t count = 0;
    int32_t capacity = 0;
};

struct PchudTransform
{
    bool  enabled = false;
    float scaleX = 0.82f;
    float scaleY = 0.82f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
};

struct FStringParam
{
    const wchar_t* data = nullptr;
    int32_t count = 0;
    int32_t capacity = 0;
};

struct SetVariableFloatParams
{
    FStringParam variable;
    float value = 0.0f;
};

// Engine.Canvas.DrawText params: FString Text @0x0, ulong CR @0x10, float XScale @0x14, float YScale @0x18,
// FFontRenderInfo RenderInfo @0x1C (zeroed; oversized for safety). Matches the SDK's execDrawText_Parms layout.
struct DrawTextParams
{
    FStringParam text;          // 0x00
    uint32_t     cr = 1;        // 0x10  (CR = new line after; 1 keeps single-line behavior consistent)
    float        xscale = 1.0f; // 0x14
    float        yscale = 1.0f; // 0x18
    uint8_t      renderInfo[0x30] = {};  // 0x1C
};

std::atomic_bool g_installed{false};
std::atomic_uintptr_t g_activePchudPanel{0};
// Perf: cache the discovered BioSFHandler_PCHUD object so the per-event hot path is a CHEAP pointer compare,
// not a class-name resolution on every ProcessEvent (thousands/frame = the gameplay hang). The expensive
// class check runs only during throttled discovery (and to re-find the handler if the game rebuilds it).
std::atomic_uintptr_t g_pchudHandler{0};
std::atomic<uint32_t> g_pchudDiscTick{0};
std::atomic_uintptr_t g_lastAppliedPchudPanel{0};
std::atomic_uint64_t g_applyLogs{0};

ProcessEventFn g_originalProcessEvent = nullptr;
void* g_processEventTarget = nullptr;
void* g_bioSfPanelSetVariableFloatFunction = nullptr;
thread_local bool g_applyingPchud = false;

// Desired transform + change detection. Written by SetTransform (menu thread), read in Tick/hook (present
// thread). Atomics keep the reads coherent; the values themselves are independent so no lock is needed.
std::atomic_bool  g_enabled{false};
std::atomic<float> g_scaleX{0.82f};
std::atomic<float> g_scaleY{0.82f};
std::atomic<float> g_offsetX{0.0f};
std::atomic<float> g_offsetY{0.0f};
// Monotonic revision bumped whenever the desired values change; the applied copy tracks the last applied one.
std::atomic_uint64_t g_settingsRev{0};
std::atomic_uint64_t g_lastAppliedRev{0};

// ---- Conversation wheel (BioSFHandler_Conversation): a SECOND movie target driven by the exact same
// mechanism. oPanel lives at 0x80 in the BASE UBioSFHandler (verified in Engine_classes.h), so every handler
// - PCHUD and Conversation alike - exposes its panel there. Its own enable + transform + caches; the
// shared g_applyingPchud reentrancy guard still covers its own SetVariableFloat calls for either movie.
std::atomic_uintptr_t g_convoHandler{0};
std::atomic<uint32_t> g_convoDiscTick{0};
std::atomic_uintptr_t g_lastAppliedConvoPanel{0};
std::atomic_uint64_t  g_convoApplyLogs{0};
// Conversation detection for the subtitle-overlay STANDDOWN (user chose: game's own subtitle in convos,
// the overlay only for gameplay barks). Stamped whenever the conversation handler is processing; a generous
// window keeps it marked across gaps between the handler's events so the overlay doesn't flicker back mid-convo.
std::atomic_uint64_t  g_convoActiveUntilFrame{0};
std::atomic<uint32_t> g_convoStandTick{0};
std::atomic_bool  g_convoEnabled{false};
std::atomic<float> g_convoScaleX{1.0f};
std::atomic<float> g_convoScaleY{1.0f};
std::atomic<float> g_convoOffsetX{0.0f};
std::atomic<float> g_convoOffsetY{0.0f};
std::atomic_uint64_t g_convoSettingsRev{0};
std::atomic_uint64_t g_convoLastAppliedRev{0};

// ---- Designer UI (BioSFHandler_DesignerUI): a THIRD movie target, same mechanism. Drives scripted/mission
// HUD overlays - charge counters, countdown timers, boss-fight UI. Discovered 2026-07-11 via the ProcessEvent
// class-name scan while fighting Saren: the panel's Kismet sequence is literally named
// "SarenMonster_UIElements" / "CountdownTimer" (BioSeqAct_DUISetElementColor/Visible/BarFillPercent/DUITimer,
// all outer=that sequence). NOT part of kHudElemNames - those came from an offline parse of the PERSISTENT
// HUD movie only; Designer UI is a separate, ad-hoc Scaleform target the game reuses across many missions.
// Same 0x80 panel offset (shared BioSFHandler base - proven by PCHUD and Conversation already using it).
// One set of values (no per-VR-mode Stereo/DIBR split like PCHUD/Convo) - this is an occasional scripted
// overlay, not a persistent per-mode HUD layout. Defaults OFF/identity: untouched installs see no change.
std::atomic_uintptr_t g_designerUiHandler{0};
std::atomic<uint32_t> g_designerUiDiscTick{0};
std::atomic_uintptr_t g_lastAppliedDesignerUiPanel{0};
std::atomic_uint64_t  g_designerUiApplyLogs{0};
std::atomic_bool  g_designerUiEnabled{false};
std::atomic<float> g_designerUiScaleX{1.0f};
std::atomic<float> g_designerUiScaleY{1.0f};
std::atomic<float> g_designerUiOffsetX{0.0f};
std::atomic<float> g_designerUiOffsetY{0.0f};
std::atomic_uint64_t g_designerUiSettingsRev{0};
std::atomic_uint64_t g_designerUiLastAppliedRev{0};

// [LASERUI 2026-07-18] The Therum mining-laser interface is the SAME BioSFHandler_DesignerUI panel in a
// different LAYOUT (Kismet flips it via BioSeqAct_DUISetLaserLayout - log-proven [PANELDISC] #380). The
// one master designer transform (tuned for the boss timer: offset -224/-345) therefore displaced the
// laser layout too = "mining UI doesn't show up" (2026-07-17). Fix: latch laser mode when the
// layout action fires, apply a SEPARATE transform set (default identity = the game's own placement),
// unlatch when the designer panel instance changes (level/movie change). Own HUD-tab controls.
std::atomic_bool g_laserUiLatched{false};
std::atomic_uintptr_t g_laserUiPanel{0};
std::atomic_uintptr_t g_laserSeqClsName{0};   // interned FName ptr -> pointer-compare fast path
std::atomic_bool  g_laserUiEnabled{true};
std::atomic<float> g_laserUiScaleX{1.0f};
std::atomic<float> g_laserUiScaleY{1.0f};
std::atomic<float> g_laserUiOffsetX{0.0f};
std::atomic<float> g_laserUiOffsetY{0.0f};

// ---- Combat/squad-bark subtitles (UBioSubtitles): NOT a Scaleform movie - the engine has no position
// field, only a render-mode enum (m_DefaultRenderMode @0x80, m_CurrentRenderMode @0x81). Values:
// NONE=0, DEFAULT=1, TOP=2, BOTTOM=3, AMBIENT=4. Combat barks default to TOP (out of view in VR). The mode
// is FORCED each frame on the live object (write where the engine reads, don't fight it after).
// Byte-write only, object re-found + class-validated each frame -> crash-safe (no method call, no stale cache).
constexpr std::uintptr_t kSubtitleFontColor         = 0x70;   // UBioSubtitles.m_FontColor (FColor)
constexpr std::uintptr_t kSubtitleFontSize          = 0x74;   // UBioSubtitles.m_FontSize (float)
constexpr std::uintptr_t kSubtitleDefaultRenderMode = 0x80;
constexpr std::uintptr_t kSubtitleCurrentRenderMode = 0x81;
std::atomic_bool g_subtitleForce{false};
std::atomic<int> g_subtitleMode{3};        // ESubtitlesRenderMode; 3 = BOTTOM
std::atomic<uint32_t> g_subtitleRescanTick{0};
std::atomic_uint64_t g_subtitleLogs{0};
std::atomic_uint64_t g_subtitleTextLogs{0};

// ---- Subtitle REDRAW (the real fix): the render-mode enum is a dead lever, so instead the game's subtitle
// is HIDDEN and redrawn on the HUD Canvas at a position + scale under direct control -> true sliders. Proven
// LE1 pattern (ME3Tweaks LE1AppearanceTest): hook BioHUD.PostRender, read Context->Canvas, call Canvas.DrawText.
// Offsets (verified in the SDK headers): AActor.WorldInfo @0x1AC, AHUD.Canvas @0x564, ABioWorldInfo.m_Subtitles
// @0xB3C, UBioSubtitles.m_sSubtitle FString @0x60 (data@0x60,count@0x68), UCanvas CurX@0xA0 CurY@0xA4
// DrawColor@0xAC SizeX@0xB0 SizeY@0xB4, UFunction.FunctionFlags @0xD8 (FUNC_NATIVE 0x400).
constexpr std::uintptr_t kActorWorldInfo   = 0x1AC;
constexpr std::uintptr_t kHudCanvas        = 0x564;
constexpr std::uintptr_t kWorldSubtitles   = 0xB3C;
constexpr std::uintptr_t kSubtitleFString  = 0x60;
constexpr std::uintptr_t kCanvasCurX       = 0xA0;
constexpr std::uintptr_t kCanvasCurY       = 0xA4;
constexpr std::uintptr_t kCanvasDrawColor  = 0xAC;
constexpr std::uintptr_t kCanvasSizeX      = 0xB0;
constexpr std::uintptr_t kCanvasSizeY      = 0xB4;
constexpr std::uintptr_t kFunctionFlags    = 0xD8;
constexpr uint32_t       kFuncNative       = 0x400;

void* g_canvasDrawTextFn = nullptr;   // Engine.Canvas.DrawText UFunction
void* g_postRenderFn = nullptr;       // SFXGame.BioHUD.PostRender UFunction (identify the event in the hook)
void* g_getSubtitleRegionFn = nullptr; // Engine.GameViewportClient.GetSubtitleRegion UFunction
void* g_bioSubtitlesDisplayFn = nullptr;
void* g_bioSubtitlesUpdateFn = nullptr;
std::atomic_bool  g_subtitleRedraw{true};
std::atomic_bool  g_subtitleHideOriginal{true};
std::atomic<float> g_subPosXFrac{0.43f};   // fraction of screen width for the text's left edge
std::atomic<float> g_subPosYFrac{0.65f};   // fraction of screen height for the text baseline
std::atomic<float> g_subScaleX{2.25f};
std::atomic<float> g_subScaleY{2.25f};
std::atomic_uint64_t g_subRedrawLogs{0};
std::atomic_uint64_t g_subRedrawSkipLogs{0};

// Native subtitle move: no duplicate redraw. This intercepts the game's own Canvas.DrawText call for the live
// BioSubtitles text and changes only that call's Canvas.CurX/Y + X/Y scale.
std::atomic_bool  g_nativeSubMove{false};
std::atomic<float> g_nativeSubPosXFrac{0.0f};   // region LEFT X fraction (was reserved; 0 = stock)
std::atomic<float> g_nativeSubPosYFrac{0.82f};  // region TOP Y fraction
std::atomic<float> g_nativeSubScaleX{1.0f};     // region RIGHT X fraction (was reserved; 1 = stock)
std::atomic<float> g_nativeSubScaleY{1.0f};     // region BOTTOM Y fraction
std::atomic<float> g_nativeSubFontSize{0.0f};   // experimental UBioSubtitles.m_FontSize write; 0 = leave game value
std::atomic_uint64_t g_nativeSubMoveLogs{0};
std::atomic_uint64_t g_nativeSubCandidateLogs{0};
std::atomic_uint64_t g_nativeSubSettingsRev{0};
std::atomic_uint64_t g_nativeSubLastAppliedRev{0};
std::atomic_uintptr_t g_lastAppliedNativeSubPanel{0};
std::atomic_bool g_subtitleHuntDumped{false};
std::atomic_uint64_t g_subtitleHuntEventLogs{0};
std::atomic_uint64_t g_subtitleProbeLogs{0};
std::atomic_uintptr_t g_activeSubtitleObj{0};
std::atomic_int g_activeSubtitleTextLen{0};
wchar_t g_activeSubtitleText[256] = {};
std::atomic_uint64_t g_subtitleTraceFrame{0};
std::atomic_uint64_t g_subtitleTraceUntilFrame{0};
std::atomic_uint64_t g_nativeSubtitleActiveUntilFrame{0};
std::atomic_uint64_t g_subtitleTraceArmId{0};
std::atomic_uint64_t g_subtitleTraceEventBudget{0};
std::atomic_uint64_t g_subtitleStackLogs{0};
std::atomic_uint64_t g_nativeSubtitleProbeLogs{0};
std::atomic_uint64_t g_nativeSubtitleWriteLogs{0};

constexpr NativeSubtitleProbeSpec kNativeSubtitleProbes[] = {
    {"SUBN1", 0x310F40},
    {"SUBN2", 0x3CA090},
    {"SUBN3", 0x9BC180},
    {"SUBN4", 0x9B8BD0},
    {"SUBN5", 0x9BDA70},
    {"SUBN6", 0xF0921C},
};
constexpr int kNativeSubtitleProbeCount =
    static_cast<int>(sizeof(kNativeSubtitleProbes) / sizeof(kNativeSubtitleProbes[0]));
NativeSubtitleProbeFn g_nativeSubtitleProbeOrig[kNativeSubtitleProbeCount] = {};
std::atomic_bool g_nativeSubtitleProbesInstalled{false};

// Engine.GameViewportClient.GetSubtitleRegion parms: TWO FVector2D out-params, i.e. FOUR floats.
// The old 2-float version of this struct silently wrote MinPos.X with the "Y" slider and never
// touched MaxPos at all - the native region override was never actually exercised correctly.
struct SubtitleRegionParams
{
    float minX = 0.0f;   // MinPos.X @0x0
    float minY = 0.0f;   // MinPos.Y @0x4
    float maxX = 1.0f;   // MaxPos.X @0x8
    float maxY = 1.0f;   // MaxPos.Y @0xC
};
static_assert(sizeof(SubtitleRegionParams) == 16, "GetSubtitleRegion parms are two FVector2Ds");

std::string PtrText(const void* ptr);
std::string SafeAnsi(const char* text) noexcept;
const char* ClassNameSEH(const void* obj) noexcept;
std::uintptr_t ModuleImageSize(HMODULE module) noexcept;
bool Readable(const void* ptr, size_t size) noexcept;
bool PointerLooksCanonicalAligned(const void* ptr) noexcept;

std::string DescribeGameAddress(const void* addr) noexcept
{
    if (addr == nullptr) return "0x0";
    HMODULE game = GetModuleHandleW(nullptr);
    const auto gameBase = reinterpret_cast<std::uintptr_t>(game);
    const auto gameSize = ModuleImageSize(game);
    const auto value = reinterpret_cast<std::uintptr_t>(addr);
    if (gameBase != 0 && gameSize != 0 && value >= gameBase && value < (gameBase + gameSize))
    {
        return std::string("game+rva=") + PtrText(reinterpret_cast<const void*>(value - gameBase));
    }
    return PtrText(addr);
}

std::string ProbeWordDump(const void* ptr) noexcept
{
    if (ptr == nullptr || !Readable(ptr, 0x20)) return "";
    uint64_t words[4] = {};
    std::memcpy(words, ptr, sizeof(words));

    std::string s;
    for (int i = 0; i < 4; ++i)
    {
        s += (i == 0 ? " [" : " ");
        s += "q";
        s += std::to_string(i);
        s += "=";
        s += PtrText(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(words[i])));
    }
    s += "]";
    return s;
}

std::string ProbeArgSummary(void* arg) noexcept
{
    if (arg == nullptr) return "0x0";
    std::string s = DescribeGameAddress(arg);
    if (PointerLooksCanonicalAligned(arg))
    {
        const char* cls = ClassNameSEH(arg);
        if (cls != nullptr && cls[0] != '\0')
        {
            s += " cls=" + SafeAnsi(cls);
        }
    }
    s += ProbeWordDump(arg);
    return s;
}

std::string ProbeFloatDump(const void* ptr, int count) noexcept
{
    if (ptr == nullptr || count <= 0) return "";
    const size_t bytes = static_cast<size_t>(count) * sizeof(float);
    if (!Readable(ptr, bytes)) return "";
    const float* vals = reinterpret_cast<const float*>(ptr);
    std::string s = " [f";
    for (int i = 0; i < count; ++i)
    {
        if (i > 0) s += ",";
        char buf[32] = {};
        sprintf_s(buf, "%.3f", vals[i]);
        s += buf;
    }
    s += "]";
    return s;
}

std::string ProbeUIntDump(const void* ptr, int count) noexcept
{
    if (ptr == nullptr || count <= 0) return "";
    const size_t bytes = static_cast<size_t>(count) * sizeof(uint32_t);
    if (!Readable(ptr, bytes)) return "";
    const uint32_t* vals = reinterpret_cast<const uint32_t*>(ptr);
    std::string s = " [u";
    for (int i = 0; i < count; ++i)
    {
        if (i > 0) s += ",";
        s += std::to_string(vals[i]);
    }
    s += "]";
    return s;
}

const void* ProbePtrAt(const void* ptr, size_t offset) noexcept
{
    if (ptr == nullptr) return nullptr;
    const BYTE* base = reinterpret_cast<const BYTE*>(ptr);
    const void* out = nullptr;
    __try
    {
        if (!Readable(base + offset, sizeof(void*))) return nullptr;
        out = *reinterpret_cast<const void* const*>(base + offset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
    return out;
}

bool Writable(const void* ptr, size_t size) noexcept
{
    if (ptr == nullptr || size == 0) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
    constexpr DWORD writeMask =
        PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const auto start = reinterpret_cast<std::uintptr_t>(ptr);
    const auto end = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return (mbi.Protect & writeMask) != 0 &&
           start + size >= start &&
           start + size <= end;
}

bool WriteFloatSlotSEH(void* ptr, int index, float value) noexcept
{
    if (ptr == nullptr || index < 0) return false;
    __try
    {
        reinterpret_cast<float*>(ptr)[index] = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void TryOverrideSubtitleNativeField(void* a4) noexcept
{
    const void* nested = ProbePtrAt(a4, 0x10);
    if (nested == nullptr || !Writable(nested, sizeof(float) * 7)) return;

    float target = g_nativeSubScaleY.load(std::memory_order_relaxed);
    target = (std::max)(0.0f, (std::min)(1.5f, target));
    if (!WriteFloatSlotSEH(const_cast<void*>(nested), 6, target)) return;

    const uint64_t n = g_nativeSubtitleWriteLogs.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (n <= 24)
    {
        LogLine("[SUBNWRITE] n=" + std::to_string(n) +
                " ptr=" + DescribeGameAddress(nested) +
                " slot=6 value=" + std::to_string(target));
    }
}

bool ShouldLogNativeSubtitleProbe() noexcept
{
    if (!g_nativeSubMove.load(std::memory_order_acquire)) return false;

    const uint64_t frame = g_subtitleTraceFrame.load(std::memory_order_acquire);
    const uint64_t activeUntil = g_nativeSubtitleActiveUntilFrame.load(std::memory_order_acquire);
    if (activeUntil != 0 && frame <= activeUntil) return true;

    return g_activeSubtitleTextLen.load(std::memory_order_acquire) > 1;
}

std::uintptr_t RunNativeSubtitleProbe(int index,
                                      void* a1, void* a2, void* a3, void* a4,
                                      void* a5, void* a6, void* a7, void* a8) noexcept
{
    if (index < 0 || index >= kNativeSubtitleProbeCount || g_nativeSubtitleProbeOrig[index] == nullptr)
    {
        return 0;
    }

    const bool armed = ShouldLogNativeSubtitleProbe();
    const uint64_t logId = armed ? (g_nativeSubtitleProbeLogs.fetch_add(1, std::memory_order_acq_rel) + 1) : 0;
    if (armed && index == 0)
    {
        TryOverrideSubtitleNativeField(a4);
    }
    if (armed && logId <= 24)
    {
        void* frames[8] = {};
        const USHORT captured = RtlCaptureStackBackTrace(1, 8, frames, nullptr);
        std::string stack;
        for (USHORT i = 0; i < captured && i < 4; ++i)
        {
            if (!stack.empty()) stack += " <- ";
            stack += DescribeGameAddress(frames[i]);
        }

        std::string extra;
        if (index == 0)
        {
            extra += " a3f=" + ProbeFloatDump(a3, 8);
            extra += " a4f=" + ProbeFloatDump(a4, 8);
            const void* a4p0 = ProbePtrAt(a4, 0x00);
            const void* a4p1 = ProbePtrAt(a4, 0x08);
            const void* a4p2 = ProbePtrAt(a4, 0x10);
            extra += " a4p0=" + DescribeGameAddress(a4p0);
            extra += " a4p1=" + DescribeGameAddress(a4p1);
            extra += " a4p2=" + DescribeGameAddress(a4p2);
            extra += " a4p2f=" + ProbeFloatDump(a4p2, 12);
            extra += " a4p2u=" + ProbeUIntDump(a4p2, 12);
        }
        else if (index == 1)
        {
            extra += " a2f=" + ProbeFloatDump(a2, 8);
            extra += " a3f=" + ProbeFloatDump(a3, 8);
        }

        LogLine(std::string("[SUBNATIVE] pre id=") + std::to_string(logId) +
                " tag=" + kNativeSubtitleProbes[index].tag +
                " fn=" + PtrText(reinterpret_cast<const void*>(kNativeSubtitleProbes[index].rva)) +
                " stack=" + stack +
                " a1=" + ProbeArgSummary(a1) +
                " a2=" + ProbeArgSummary(a2) +
                " a3=" + ProbeArgSummary(a3) +
                " a4=" + ProbeArgSummary(a4) +
                extra);
    }

    const std::uintptr_t ret = g_nativeSubtitleProbeOrig[index](a1, a2, a3, a4, a5, a6, a7, a8);

    if (armed && logId <= 24)
    {
        std::string extra;
        if (index == 0)
        {
            extra += " a3f=" + ProbeFloatDump(a3, 8);
            extra += " a4f=" + ProbeFloatDump(a4, 8);
            const void* a4p0 = ProbePtrAt(a4, 0x00);
            const void* a4p1 = ProbePtrAt(a4, 0x08);
            const void* a4p2 = ProbePtrAt(a4, 0x10);
            const void* a1p8 = ProbePtrAt(a1, 0x08);
            extra += " a4p0=" + DescribeGameAddress(a4p0);
            extra += " a4p1=" + DescribeGameAddress(a4p1);
            extra += " a4p2=" + DescribeGameAddress(a4p2);
            extra += " a4p2f=" + ProbeFloatDump(a4p2, 12);
            extra += " a4p2u=" + ProbeUIntDump(a4p2, 12);
            extra += " a1p8=" + DescribeGameAddress(a1p8);
            extra += " a1p8f=" + ProbeFloatDump(a1p8, 12);
            extra += " a1p8u=" + ProbeUIntDump(a1p8, 12);
        }
        else if (index == 1)
        {
            extra += " a2f=" + ProbeFloatDump(a2, 8);
            extra += " a3f=" + ProbeFloatDump(a3, 8);
        }

        LogLine(std::string("[SUBNATIVE] post id=") + std::to_string(logId) +
                " tag=" + kNativeSubtitleProbes[index].tag +
                " ret=" + PtrText(reinterpret_cast<const void*>(ret)) +
                " a1=" + ProbeArgSummary(a1) +
                " a2=" + ProbeArgSummary(a2) +
                extra);
    }

    return ret;
}

std::uintptr_t __fastcall HookNativeSubtitleProbe1(void* a1, void* a2, void* a3, void* a4,
                                                   void* a5, void* a6, void* a7, void* a8) noexcept
{
    return RunNativeSubtitleProbe(0, a1, a2, a3, a4, a5, a6, a7, a8);
}

std::uintptr_t __fastcall HookNativeSubtitleProbe2(void* a1, void* a2, void* a3, void* a4,
                                                   void* a5, void* a6, void* a7, void* a8) noexcept
{
    return RunNativeSubtitleProbe(1, a1, a2, a3, a4, a5, a6, a7, a8);
}

std::uintptr_t __fastcall HookNativeSubtitleProbe3(void* a1, void* a2, void* a3, void* a4,
                                                   void* a5, void* a6, void* a7, void* a8) noexcept
{
    return RunNativeSubtitleProbe(2, a1, a2, a3, a4, a5, a6, a7, a8);
}

std::uintptr_t __fastcall HookNativeSubtitleProbe4(void* a1, void* a2, void* a3, void* a4,
                                                   void* a5, void* a6, void* a7, void* a8) noexcept
{
    return RunNativeSubtitleProbe(3, a1, a2, a3, a4, a5, a6, a7, a8);
}

std::uintptr_t __fastcall HookNativeSubtitleProbe5(void* a1, void* a2, void* a3, void* a4,
                                                   void* a5, void* a6, void* a7, void* a8) noexcept
{
    return RunNativeSubtitleProbe(4, a1, a2, a3, a4, a5, a6, a7, a8);
}

std::uintptr_t __fastcall HookNativeSubtitleProbe6(void* a1, void* a2, void* a3, void* a4,
                                                   void* a5, void* a6, void* a7, void* a8) noexcept
{
    return RunNativeSubtitleProbe(5, a1, a2, a3, a4, a5, a6, a7, a8);
}

void InstallNativeSubtitleProbes() noexcept
{
    bool expected = false;
    if (!g_nativeSubtitleProbesInstalled.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return;
    }

    HMODULE game = GetModuleHandleW(nullptr);
    BYTE* base = reinterpret_cast<BYTE*>(game);
    if (base == nullptr)
    {
        return;
    }

    void* detours[kNativeSubtitleProbeCount] = {
        reinterpret_cast<void*>(&HookNativeSubtitleProbe1),
        reinterpret_cast<void*>(&HookNativeSubtitleProbe2),
        reinterpret_cast<void*>(&HookNativeSubtitleProbe3),
        reinterpret_cast<void*>(&HookNativeSubtitleProbe4),
        reinterpret_cast<void*>(&HookNativeSubtitleProbe5),
        reinterpret_cast<void*>(&HookNativeSubtitleProbe6),
    };

    for (int i = 0; i < kNativeSubtitleProbeCount; ++i)
    {
        void* target = base + kNativeSubtitleProbes[i].rva;
        const MH_STATUS create = MH_CreateHook(target, detours[i],
                                               reinterpret_cast<void**>(&g_nativeSubtitleProbeOrig[i]));
        if (create != MH_OK && create != MH_ERROR_ALREADY_CREATED)
        {
            LogLine(std::string("[SUBNATIVE] MH_CreateHook failed tag=") + kNativeSubtitleProbes[i].tag +
                    " rva=" + PtrText(reinterpret_cast<const void*>(kNativeSubtitleProbes[i].rva)) +
                    " status=" + std::to_string(static_cast<int>(create)));
            continue;
        }

        const MH_STATUS enable = MH_EnableHook(target);
        if (enable != MH_OK && enable != MH_ERROR_ENABLED)
        {
            LogLine(std::string("[SUBNATIVE] MH_EnableHook failed tag=") + kNativeSubtitleProbes[i].tag +
                    " rva=" + PtrText(reinterpret_cast<const void*>(kNativeSubtitleProbes[i].rva)) +
                    " status=" + std::to_string(static_cast<int>(enable)));
            continue;
        }

        LogLine(std::string("[SUBNATIVE] probe installed tag=") + kNativeSubtitleProbes[i].tag +
                " rva=" + PtrText(reinterpret_cast<const void*>(kNativeSubtitleProbes[i].rva)));
    }
}

struct PanelFloatPaths
{
    const wchar_t* xscale;
    const wchar_t* yscale;
    const wchar_t* x;
    const wchar_t* y;
};

std::string PtrText(const void* ptr)
{
    char buffer[32] = {};
    sprintf_s(buffer, "0x%p", ptr);
    return buffer;
}

std::string SafeAnsi(const char* text) noexcept
{
    if (text == nullptr) return "<null>";
    char buffer[96] = {};
    int i = 0;
    for (; i + 1 < static_cast<int>(sizeof(buffer)) && text[i] != '\0'; ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        buffer[i] = (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
    }
    buffer[i] = '\0';
    return buffer;
}

bool NameContains(const char* text, const char* needle) noexcept
{
    return text != nullptr && needle != nullptr && std::strstr(text, needle) != nullptr;
}

bool IsSubtitleHuntRelated(const char* cls, const char* name, const char* outer) noexcept
{
    return NameContains(cls, "Subtitle") || NameContains(name, "Subtitle") || NameContains(outer, "Subtitle") ||
           NameContains(cls, "Caption")  || NameContains(name, "Caption")  || NameContains(outer, "Caption")  ||
           NameContains(cls, "Dialog")   || NameContains(name, "Dialog")   || NameContains(outer, "Dialog")   ||
           NameContains(cls, "Conversation") || NameContains(name, "Conversation") || NameContains(outer, "Conversation") ||
           NameContains(cls, "Conv")     || NameContains(name, "Conv")     || NameContains(outer, "Conv");
}

const char* ObjectNameSEH(const void* obj) noexcept;
const char* ClassNameSEH(const void* obj) noexcept;
const char* OuterNameSEH(const void* obj) noexcept;

bool IsSubtitleTraceRenderRelated(const char* cls, const char* name, const char* outer) noexcept
{
    return IsSubtitleHuntRelated(cls, name, outer) ||
           NameContains(cls, "Canvas") || NameContains(name, "Canvas") || NameContains(outer, "Canvas") ||
           NameContains(cls, "HUD")    || NameContains(name, "HUD")    || NameContains(outer, "HUD") ||
           NameContains(cls, "Font")   || NameContains(name, "Font")   || NameContains(outer, "Font") ||
           NameContains(cls, "Draw")   || NameContains(name, "Draw")   || NameContains(outer, "Draw") ||
           NameContains(cls, "Render") || NameContains(name, "Render") || NameContains(outer, "Render") ||
           NameContains(cls, "Viewport") || NameContains(name, "Viewport") || NameContains(outer, "Viewport");
}

void ArmSubtitleRenderTrace(const char* text) noexcept
{
    const uint64_t frame = g_subtitleTraceFrame.load(std::memory_order_acquire);
    const uint64_t armId = g_subtitleTraceArmId.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_subtitleTraceEventBudget.store(120, std::memory_order_release);
    g_subtitleTraceUntilFrame.store(frame + 2, std::memory_order_release);
    char preview[96] = {};
    if (text != nullptr)
    {
        strncpy_s(preview, text, sizeof(preview) - 1);
        preview[sizeof(preview) - 1] = '\0';
    }
    LogLine("[SUBTRACE_ARM] arm=" + std::to_string(armId) +
            " frame=" + std::to_string(frame) +
            " until=" + std::to_string(frame + 2) +
            " text='" + std::string(preview) + "'");
}

void MaybeLogSubtitleRenderTrace(void* context, void* function) noexcept
{
    const uint64_t frame = g_subtitleTraceFrame.load(std::memory_order_acquire);
    const uint64_t until = g_subtitleTraceUntilFrame.load(std::memory_order_acquire);
    if (until == 0 || frame > until) return;

    const char* ctxCls = ClassNameSEH(context);
    const char* ctxName = ObjectNameSEH(context);
    const char* ctxOuter = OuterNameSEH(context);
    const char* fnCls = ClassNameSEH(function);
    const char* fnName = ObjectNameSEH(function);
    const char* fnOuter = OuterNameSEH(function);

    if (!IsSubtitleTraceRenderRelated(ctxCls, ctxName, ctxOuter) &&
        !IsSubtitleTraceRenderRelated(fnCls, fnName, fnOuter))
    {
        return;
    }

    uint64_t budget = g_subtitleTraceEventBudget.load(std::memory_order_acquire);
    while (budget > 0)
    {
        if (g_subtitleTraceEventBudget.compare_exchange_weak(budget, budget - 1, std::memory_order_acq_rel))
            break;
    }
    if (budget == 0) return;

    const uint64_t armId = g_subtitleTraceArmId.load(std::memory_order_acquire);
    const uint64_t traceCount = 121 - budget;
    LogLine("[SUBTRACE] arm=" + std::to_string(armId) +
            " frame=" + std::to_string(frame) +
            " n=" + std::to_string(traceCount) +
            " ctxCls=" + SafeAnsi(ctxCls) +
            " ctxName=" + SafeAnsi(ctxName) +
            " ctxOuter=" + SafeAnsi(ctxOuter) +
            " fnCls=" + SafeAnsi(fnCls) +
            " fnName=" + SafeAnsi(fnName) +
            " fnOuter=" + SafeAnsi(fnOuter) +
            " ctx=" + PtrText(context) +
            " fn=" + PtrText(function));
}

std::uintptr_t ModuleImageSize(HMODULE module) noexcept
{
    if (module == nullptr) return 0;
    __try
    {
        auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
        auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(reinterpret_cast<const BYTE*>(module) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
        return static_cast<std::uintptr_t>(nt->OptionalHeader.SizeOfImage);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

void MaybeLogSubtitleRegionStack() noexcept
{
    if (g_activeSubtitleTextLen.load(std::memory_order_acquire) <= 1) return;
    const uint64_t n = g_subtitleStackLogs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > 6) return;

    void* frames[16] = {};
    const USHORT captured = RtlCaptureStackBackTrace(0, 16, frames, nullptr);
    HMODULE game = GetModuleHandleW(nullptr);
    const auto gameBase = reinterpret_cast<std::uintptr_t>(game);
    const auto gameSize = ModuleImageSize(game);

    LogLine("[SUBSTACK] n=" + std::to_string(n) + " frames=" + std::to_string(captured));
    for (USHORT i = 0; i < captured; ++i)
    {
        const auto addr = reinterpret_cast<std::uintptr_t>(frames[i]);
        std::string where = "external";
        if (gameBase != 0 && gameSize != 0 && addr >= gameBase && addr < (gameBase + gameSize))
        {
            const auto rva = addr - gameBase;
            where = "game+rva=" + PtrText(reinterpret_cast<const void*>(rva));
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(static_cast<DWORD64>(addr), &imageBase, nullptr);
            if (fn != nullptr)
            {
                where += " func=[" + PtrText(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(fn->BeginAddress))) +
                         "," + PtrText(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(fn->EndAddress))) + ")";
            }
        }
        LogLine("[SUBSTACK]  #" + std::to_string(i) + " addr=" + PtrText(frames[i]) + " " + where);
    }
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

bool IsExecutableAddress(const void* ptr) noexcept
{
    if (ptr == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) return false;
    constexpr DWORD executeMask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & executeMask) != 0;
}

bool PointerLooksCanonicalAligned(const void* ptr) noexcept
{
    const std::uintptr_t bits = reinterpret_cast<std::uintptr_t>(ptr);
    return ptr != nullptr && (bits & 0xF) == 0 && bits < 0x0000800000000000ull;
}

bool ReadPtrAtSEH(const void* base, std::uintptr_t offset, void** out) noexcept
{
    if (out == nullptr) return false;
    __try
    {
        *out = *reinterpret_cast<void* const*>(reinterpret_cast<const BYTE*>(base) + offset);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *out = nullptr;
        return false;
    }
}

bool ReadU32AtSEH(const void* base, std::uintptr_t offset, uint32_t* out) noexcept
{
    if (out == nullptr) return false;
    __try
    {
        *out = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const BYTE*>(base) + offset);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *out = 0;
        return false;
    }
}

bool WriteU8AtSEH(void* base, std::uintptr_t offset, uint8_t value) noexcept
{
    __try
    {
        *reinterpret_cast<uint8_t*>(reinterpret_cast<BYTE*>(base) + offset) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

uint8_t ReadU8AtSEH(const void* base, std::uintptr_t offset) noexcept
{
    __try
    {
        return *reinterpret_cast<const uint8_t*>(reinterpret_cast<const BYTE*>(base) + offset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0xFF;
    }
}

bool WriteFloatAtSEH(void* base, std::uintptr_t offset, float value) noexcept
{
    __try { *reinterpret_cast<float*>(reinterpret_cast<BYTE*>(base) + offset) = value; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool ReadFloatAtSEH(const void* base, std::uintptr_t offset, float* out) noexcept
{
    if (out == nullptr) return false;
    __try { *out = *reinterpret_cast<const float*>(reinterpret_cast<const BYTE*>(base) + offset); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = 0.0f; return false; }
}

bool WritePtrAtSEH(void* base, std::uintptr_t offset, void* value) noexcept
{
    if (base == nullptr) return false;
    __try
    {
        *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(base) + offset) = value;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool WriteU32AtSEH(void* base, std::uintptr_t offset, uint32_t value) noexcept
{
    __try { *reinterpret_cast<uint32_t*>(reinterpret_cast<BYTE*>(base) + offset) = value; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int32_t ReadI32AtSEH(const void* base, std::uintptr_t offset) noexcept
{
    __try { return *reinterpret_cast<const int32_t*>(reinterpret_cast<const BYTE*>(base) + offset); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

bool ReadObjectSlotSEH(void* const* data, int32_t index, void** out) noexcept
{
    if (out == nullptr) return false;
    __try
    {
        *out = data[index];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *out = nullptr;
        return false;
    }
}

ObjectArrayHeader* ObjectsSEH() noexcept
{
    __try
    {
        auto* moduleBase = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
        if (moduleBase == nullptr) return nullptr;

        auto* objects = reinterpret_cast<ObjectArrayHeader*>(moduleBase + kGObjObjectsOffset);
        if (objects == nullptr ||
            objects->data == nullptr ||
            objects->count <= 0 ||
            objects->count > 8000000 ||
            objects->capacity < objects->count)
        {
            return nullptr;
        }

        return objects;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

const char* DecodeFNameSEH(uint32_t packed) noexcept
{
    __try
    {
        auto* moduleBase = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
        if (moduleBase == nullptr) return nullptr;

        auto** namePools = reinterpret_cast<BYTE**>(moduleBase + kNamePoolsOffset);
        const uint32_t offset = packed & 0x1FFFFFFFu;
        const uint32_t chunk = (packed >> 29) & 0x7u;
        BYTE* pool = namePools[chunk];
        if (pool == nullptr) return nullptr;

        const char* text = reinterpret_cast<const char*>(pool + offset + kFNameEntryString);
        return text != nullptr && text[0] != '\0' ? text : nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

const char* ObjectNameSEH(const void* obj) noexcept
{
    uint32_t packed = 0;
    return ReadU32AtSEH(obj, kUObjectName, &packed) ? DecodeFNameSEH(packed) : nullptr;
}

const char* ClassNameSEH(const void* obj) noexcept
{
    void* cls = nullptr;
    return ReadPtrAtSEH(obj, kUObjectClass, &cls) ? ObjectNameSEH(cls) : nullptr;
}

const char* OuterNameSEH(const void* obj) noexcept
{
    void* outer = nullptr;
    return ReadPtrAtSEH(obj, 0x40, &outer) ? ObjectNameSEH(outer) : nullptr;
}

bool IsFunctionNamedOuter(void* obj, const char* functionName, const char* outerName) noexcept
{
    if (obj == nullptr || functionName == nullptr || outerName == nullptr || !Readable(obj, 0x60)) return false;

    const char* cls = ClassNameSEH(obj);
    const char* name = ObjectNameSEH(obj);
    const char* outer = OuterNameSEH(obj);
    return cls != nullptr &&
           name != nullptr &&
           outer != nullptr &&
           std::strcmp(cls, "Function") == 0 &&
           std::strcmp(name, functionName) == 0 &&
           std::strcmp(outer, outerName) == 0;
}

void* FindFunctionByOuterName(const char* outerName, const char* functionName) noexcept
{
    ObjectArrayHeader* objects = ObjectsSEH();
    if (objects == nullptr || !Readable(objects->data, sizeof(void*) * static_cast<size_t>(objects->count)))
    {
        return nullptr;
    }

    for (int32_t index = 0; index < objects->count; ++index)
    {
        void* obj = nullptr;
        if (!ReadObjectSlotSEH(objects->data, index, &obj)) return nullptr;
        if (!PointerLooksCanonicalAligned(obj)) continue;
        if (IsFunctionNamedOuter(obj, functionName, outerName)) return obj;
    }

    return nullptr;
}

// Returns the panel pointer IFF this context is an ACTIVE BioSFHandler_PCHUD - read fresh from the handler
// the game is CURRENTLY processing, so it is alive at this instant. Returns nullptr otherwise. This NEVER caches
// this across ProcessEvent calls: panels are freed/rebuilt on HUD transitions, and applying to a cached
// (stale) panel is what crashed the game (freed-object read, 0xFFFF... AV).
void* PchudHandlerLivePanel(void* context) noexcept
{
    const char* cls = ClassNameSEH(context);
    const char* name = ObjectNameSEH(context);
    if ((cls == nullptr || std::strstr(cls, "BioSFHandler_PCHUD") == nullptr) &&
        (name == nullptr || std::strstr(name, "BioSFHandler_PCHUD") == nullptr))
    {
        return nullptr;
    }

    void* panel = nullptr;
    if (ReadPtrAtSEH(context, kPchudHandlerPanel, &panel) &&
        PointerLooksCanonicalAligned(panel) &&
        Readable(panel, 0x100))
    {
        return panel;
    }
    return nullptr;
}

bool ResolveSetVariableFloat() noexcept
{
    if (g_bioSfPanelSetVariableFloatFunction != nullptr) return true;

    g_bioSfPanelSetVariableFloatFunction = FindFunctionByOuterName("BioSFPanel", "SetVariableFloat");
    if (g_bioSfPanelSetVariableFloatFunction != nullptr)
    {
        LogLine("[PCHUD] resolved BioSFPanel.SetVariableFloat=" +
                PtrText(g_bioSfPanelSetVariableFloatFunction));
        return true;
    }

    return false;
}

// ---- Per-element movie controls: wide-path tables + state. Names came from the live SWF dumps
// (swf_GUI_SF_HUD_PC_ME_HUD / swf_GUI_SF_Conversation_Conversation, parsed PlaceObject2 records).
// The narrow (config/UI) name tables live in the public namespace at the bottom of this file.
const wchar_t* const kHudElemNamesW[kHudElemCount] = {
    L"BottomUI", L"radarMC", L"targetMC", L"topLeft", L"topRight", L"leftUI", L"rightUI",
    L"squadMC", L"weaponAbilityTab", L"EventHolder", L"TeamBG", L"EquipBG", L"vehiclePause",
};
const wchar_t* const kConvoElemNamesW[kConvoElemCount] = {
    L"SubtitleConversation", L"SubtitleTop", L"SubtitleBottom", L"ConversationWheel",
};

// AUTHORED placements, extracted OFFLINE from the dumped SWFs (PlaceObject2 matrices, twips/20 = px;
// scale in percent). These are the baselines offsets/scales apply against - baked so the runtime NEVER
// has to read a value back from the movie. (The GetVariableFloat baseline read was the prime suspect in
// the 2026-07-06 heap-fast-fail: an FString parm in a ProcessEvent call that RETURNS a value risks the
// engine's parms teardown freeing the stack-owned string. Write-only calls have run safely for weeks.)
struct ElemBaseline { float x; float y; float sxPct; float syPct; };
constexpr ElemBaseline kHudElemBase[kHudElemCount] = {
    { 434.80f,  601.95f, 100.0f,  100.0f },   // BottomUI
    { 1089.00f, 556.60f,  78.3f,   78.3f },   // radarMC (authored at 78.3%)
    { 400.55f,   36.05f, 100.0f,  100.0f },   // targetMC
    { -264.90f,   0.30f, 100.0f,  100.0f },   // topLeft
    { 929.80f,  144.80f, 100.0f,  100.0f },   // topRight
    { -169.05f, 100.00f, 100.0f,  100.0f },   // leftUI
    { 1565.85f, 100.00f, -100.0f, 100.0f },   // rightUI (authored mirrored: xscale -100)
    { 635.50f,  355.55f, 100.0f,  100.0f },   // squadMC
    { 923.45f,  101.50f, 100.0f,  100.0f },   // weaponAbilityTab
    { 1175.00f, 527.00f, 100.0f,  100.0f },   // EventHolder
    { 477.25f,  526.30f, 100.0f,  100.0f },   // TeamBG
    { 64.85f,   685.40f,  99.6f,  100.0f },   // EquipBG
    { 640.00f,  183.30f, 100.0f,  100.0f },   // vehiclePause
};
constexpr ElemBaseline kConvoElemBase[kConvoElemCount] = {
    { 639.05f,  605.60f, 100.0f,  100.0f },   // SubtitleConversation
    { 640.25f,  159.15f, 100.0f,  100.0f },   // SubtitleTop
    { 639.00f,  703.10f, 100.0f,  100.0f },   // SubtitleBottom
    { 639.70f,  643.25f, 100.0f,  100.0f },   // ConversationWheel
};

// Double-buffered desired state (menu thread writes the inactive slot then swaps; game thread reads active).
HudElemState g_hudElemBuf[2][kHudElemCount] = {};
std::atomic<int> g_hudElemActive{0};
std::atomic_uint64_t g_hudElemRev{0};
HudElemState g_convoElemBuf[2][kConvoElemCount] = {};
std::atomic<int> g_convoElemActive{0};
std::atomic_uint64_t g_convoElemRev{0};
std::atomic_uint64_t g_elemLogs{0};

bool CallBioSfPanelSetVariableFloat(void* panel, const wchar_t* variable, float value) noexcept
{
    ProcessEventFn original = g_originalProcessEvent;
    if (original == nullptr ||
        panel == nullptr ||
        variable == nullptr ||
        !Readable(panel, 0x100) ||
        g_applyingPchud ||
        !ResolveSetVariableFloat())
    {
        return false;
    }

    SetVariableFloatParams params = {};
    params.variable.data = variable;
    params.variable.count = static_cast<int32_t>(std::wcslen(variable) + 1);
    params.variable.capacity = params.variable.count;
    params.value = value;

    bool ok = false;
    g_applyingPchud = true;
    __try
    {
        original(panel, g_bioSfPanelSetVariableFloatFunction, &params, nullptr);
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        ok = false;
    }
    g_applyingPchud = false;
    return ok;
}

// Per-element apply: moves/scales NAMED children of a movie relative to their authored layout.
// WRITE-ONLY by design: baselines are the baked kHudElemBase/kConvoElemBase tables (from the SWF dumps),
// so this never reads a value back from the movie. Restore-on-disable writes the authored values back.
// Game-thread only (called from the ProcessEvent hook), so plain statics are safe.
//
// VISIBILITY RULE (2026-07-06, learned the hard way): NEVER write _visible unless the user explicitly
// asked to hide. The unconditional `_visible = hide ? 0 : 1` force-showed assets the game keeps hidden
// (equipment backdrop = the "big metal door", event popups strobing between a forced 1 and the game's 0).
// The game owns visibility; this only overrides it downward, and only restores what it hid.
struct ElemRuntime
{
    bool applied = false;   // something non-neutral was written (needs restore when turned off)
    bool hidden = false;    // this wrote _visible=0 (restore to 1 only in that case)
};

void ApplyElementsToPanel(void* panel, const wchar_t* const* namesW, const ElemBaseline* base, int count,
                          const HudElemState* desired, std::uintptr_t* cachedPanel,
                          ElemRuntime* runtime, const char* tag) noexcept
{
    const std::uintptr_t panelBits = reinterpret_cast<std::uintptr_t>(panel);
    if (*cachedPanel != panelBits)
    {
        // fresh movie instance = authored layout; nothing has been applied to it yet
        for (int i = 0; i < count; ++i) runtime[i] = ElemRuntime{};
        *cachedPanel = panelBits;
    }

    wchar_t path[128] = {};
    auto elemPath = [&](const wchar_t* elem, const wchar_t* prop) noexcept -> const wchar_t*
    {
        swprintf_s(path, L"_root.%s.%s", elem, prop);
        return path;
    };

    for (int i = 0; i < count; ++i)
    {
        const HudElemState& e = desired[i];
        ElemRuntime& rt = runtime[i];
        const bool neutral = !e.hide && e.offX == 0.0f && e.offY == 0.0f &&
                             e.scaleX == 1.0f && e.scaleY == 1.0f;

        if (!e.on || neutral)
        {
            if (rt.applied)
            {
                // restore the authored layout once, then go quiet
                CallBioSfPanelSetVariableFloat(panel, elemPath(namesW[i], L"_x"), base[i].x);
                CallBioSfPanelSetVariableFloat(panel, elemPath(namesW[i], L"_y"), base[i].y);
                CallBioSfPanelSetVariableFloat(panel, elemPath(namesW[i], L"_xscale"), base[i].sxPct);
                CallBioSfPanelSetVariableFloat(panel, elemPath(namesW[i], L"_yscale"), base[i].syPct);
                if (rt.hidden)
                {
                    CallBioSfPanelSetVariableFloat(panel, elemPath(namesW[i], L"_visible"), 1.0f);
                }
                rt = ElemRuntime{};
            }
            continue;
        }

        auto clampf = [](float x, float lo, float hi) noexcept { if (!(x == x)) return 0.0f; return x < lo ? lo : (x > hi ? hi : x); };
        const float sx = base[i].sxPct * clampf(e.scaleX, 0.10f, 4.0f);   // multiply the authored scale
        const float sy = base[i].syPct * clampf(e.scaleY, 0.10f, 4.0f);   // (keeps radar's 78.3%, rightUI's mirror)
        const float x  = base[i].x + clampf(e.offX, -2000.0f, 2000.0f);
        const float y  = base[i].y + clampf(e.offY, -2000.0f, 2000.0f);

        bool ok =
            CallBioSfPanelSetVariableFloat(panel, elemPath(namesW[i], L"_x"), x) &&
            CallBioSfPanelSetVariableFloat(panel, elemPath(namesW[i], L"_y"), y) &&
            CallBioSfPanelSetVariableFloat(panel, elemPath(namesW[i], L"_xscale"), sx) &&
            CallBioSfPanelSetVariableFloat(panel, elemPath(namesW[i], L"_yscale"), sy);
        if (e.hide)
        {
            ok = CallBioSfPanelSetVariableFloat(panel, elemPath(namesW[i], L"_visible"), 0.0f) && ok;
            rt.hidden = true;
        }
        else if (rt.hidden)
        {
            // user just un-ticked Hide while overrides stay on: give visibility back to the game once
            ok = CallBioSfPanelSetVariableFloat(panel, elemPath(namesW[i], L"_visible"), 1.0f) && ok;
            rt.hidden = false;
        }

        if (!rt.applied)
        {
            const uint64_t n = g_elemLogs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 80)
            {
                LogLine(std::string("[") + tag + "] apply " + MELEVR::Logger::WideToUtf8(namesW[i]) +
                        " ok=" + std::to_string(ok ? 1 : 0) +
                        " x=" + std::to_string(x) + " y=" + std::to_string(y) +
                        " sx=" + std::to_string(sx) + " sy=" + std::to_string(sy) +
                        " hide=" + std::to_string(e.hide ? 1 : 0) +
                        " panel=" + PtrText(panel));
            }
        }
        rt.applied = true;
    }
}

// Low-level: push scale/offset to a live panel's _root via the game's BioSFPanel.SetVariableFloat. Shared by
// every movie target. Caller has already validated the panel and holds its enable gate + change-detect. The
// clamp keeps extreme values from wedging the movie and taking the game down. Returns true iff all 4 writes ok.
bool ApplyScaleOffsetToPanel(void* panel, float scaleX, float scaleY, float offsetX, float offsetY) noexcept
{
    auto clampf = [](float x, float lo, float hi) noexcept { if (!(x == x)) return 1.0f; return x < lo ? lo : (x > hi ? hi : x); };
    const float sx = clampf(scaleX, 0.20f, 2.00f);
    const float sy = clampf(scaleY, 0.20f, 2.00f);
    const float ox = clampf(offsetX, -1000.0f, 1000.0f);
    const float oy = clampf(offsetY, -1000.0f, 1000.0f);

    bool ok = true;
    ok = CallBioSfPanelSetVariableFloat(panel, L"_root._xscale", sx * 100.0f) && ok;
    ok = CallBioSfPanelSetVariableFloat(panel, L"_root._yscale", sy * 100.0f) && ok;
    ok = CallBioSfPanelSetVariableFloat(panel, L"_root._x", ox) && ok;
    ok = CallBioSfPanelSetVariableFloat(panel, L"_root._y", oy) && ok;
    return ok;
}

bool ApplyScaleOffsetToPanelPaths(void* panel, const PanelFloatPaths& paths,
                                  float scaleX, float scaleY, float offsetX, float offsetY) noexcept
{
    auto clampf = [](float x, float lo, float hi) noexcept { if (!(x == x)) return 1.0f; return x < lo ? lo : (x > hi ? hi : x); };
    const float sx = clampf(scaleX, 0.20f, 3.00f);
    const float sy = clampf(scaleY, 0.20f, 3.00f);
    const float ox = clampf(offsetX, -2000.0f, 2000.0f);
    const float oy = clampf(offsetY, -2000.0f, 2000.0f);

    bool ok = true;
    ok = CallBioSfPanelSetVariableFloat(panel, paths.xscale, sx * 100.0f) && ok;
    ok = CallBioSfPanelSetVariableFloat(panel, paths.yscale, sy * 100.0f) && ok;
    ok = CallBioSfPanelSetVariableFloat(panel, paths.x, ox) && ok;
    ok = CallBioSfPanelSetVariableFloat(panel, paths.y, oy) && ok;
    return ok;
}

// Apply the transform to a panel read LIVE from an active BioSFHandler_PCHUD ProcessEvent THIS instant.
// Never call with a cached/stale pointer - that is the freed-panel crash.
void ApplyToLivePanel(void* panel) noexcept
{
    if (g_applyingPchud) return;
    if (!g_enabled.load(std::memory_order_acquire)) return;
    if (!PointerLooksCanonicalAligned(panel) || !Readable(panel, 0x100)) return;

    // Per-element overrides (BottomUI/radar/target-name/...): re-stamped every handler event so the movie's
    // own timeline can't quietly undo a hide/move. Neutral elements cost nothing.
    {
        static std::uintptr_t s_elemPanel = 0;
        static ElemRuntime s_elemRuntime[kHudElemCount] = {};
        const HudElemState* desired = g_hudElemBuf[g_hudElemActive.load(std::memory_order_acquire)];
        ApplyElementsToPanel(panel, kHudElemNamesW, kHudElemBase, kHudElemCount, desired,
                             &s_elemPanel, s_elemRuntime, "HUDELEM");
    }

    const std::uintptr_t panelBits = reinterpret_cast<std::uintptr_t>(panel);
    const uint64_t rev = g_settingsRev.load(std::memory_order_acquire);
    if (panelBits == g_lastAppliedPchudPanel.load(std::memory_order_acquire) &&
        rev == g_lastAppliedRev.load(std::memory_order_acquire))
    {
        return;   // nothing changed since the last successful apply to this panel
    }

    const float scaleX = g_scaleX.load(std::memory_order_relaxed);
    const float scaleY = g_scaleY.load(std::memory_order_relaxed);
    const float offsetX = g_offsetX.load(std::memory_order_relaxed);
    const float offsetY = g_offsetY.load(std::memory_order_relaxed);

    // CENTER-PIVOT master scale (2026-07-06): _root scales about the movie ORIGIN (0,0), so raw scaling
    // marches the whole HUD toward the corner and out of view. Compensate the position so the STAGE CENTER
    // (~640,360 on the 1280x720 movie) stays fixed as scale changes: at the reference scale the offset is
    // unchanged (no jump on existing saves), away from it the position pivots. Result: sizing up/down grows
    // the HUD in place instead of flinging it off-screen.
    constexpr float kStageCenterX = 640.0f, kStageCenterY = 360.0f;
    constexpr float kRefScale = 0.35f;   // the default master scale; offset is "true" at this scale
    const float pivotedX = offsetX - kStageCenterX * (scaleX - kRefScale);
    const float pivotedY = offsetY - kStageCenterY * (scaleY - kRefScale);
    const bool ok = ApplyScaleOffsetToPanel(panel, scaleX, scaleY, pivotedX, pivotedY);

    if (ok)
    {
        g_lastAppliedPchudPanel.store(panelBits, std::memory_order_release);
        g_lastAppliedRev.store(rev, std::memory_order_release);
    }

    const uint64_t n = g_applyLogs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || !ok)
    {
        LogLine("[PCHUD] apply n=" + std::to_string(n) +
                " ok=" + std::to_string(ok ? 1 : 0) +
                " panel=" + PtrText(panel));
    }
}

// Conversation-wheel twin of ApplyToLivePanel: same freed-panel discipline, its own enable gate + caches.
void ApplyConvoToLivePanel(void* panel) noexcept
{
    if (g_applyingPchud) return;
    if (!g_convoEnabled.load(std::memory_order_acquire)) return;
    if (!PointerLooksCanonicalAligned(panel) || !Readable(panel, 0x100)) return;

    // Per-element overrides for the conversation movie (SubtitleTop/SubtitleConversation/wheel).
    {
        static std::uintptr_t s_elemPanel = 0;
        static ElemRuntime s_elemRuntime[kConvoElemCount] = {};
        const HudElemState* desired = g_convoElemBuf[g_convoElemActive.load(std::memory_order_acquire)];
        ApplyElementsToPanel(panel, kConvoElemNamesW, kConvoElemBase, kConvoElemCount, desired,
                             &s_elemPanel, s_elemRuntime, "CONVOELEM");
    }

    // ALWAYS RE-APPLY (2026-07-06): the old change-detect early-return (same panel + same rev = skip) is
    // exactly why the conversation UI stayed large until a nudge. The conversation movie's intro animation
    // resets _root scale to 100% AFTER the first apply; with the early-return it was never corrected (panel
    // and rev unchanged) until a menu nudge bumped the rev. Fix: re-stamp the scale on EVERY convo event
    // (the handler ticks ~once/frame during the conversation), so any reset is overwritten next frame.
    // Cheap: 4 SetVariableFloat, and only while a conversation is on screen.
    const std::uintptr_t panelBits = reinterpret_cast<std::uintptr_t>(panel);
    const float scaleX = g_convoScaleX.load(std::memory_order_relaxed);
    const float scaleY = g_convoScaleY.load(std::memory_order_relaxed);
    const float offsetX = g_convoOffsetX.load(std::memory_order_relaxed);
    const float offsetY = g_convoOffsetY.load(std::memory_order_relaxed);
    const bool ok = ApplyScaleOffsetToPanel(panel, scaleX, scaleY, offsetX, offsetY);

    if (ok)
    {
        g_lastAppliedConvoPanel.store(panelBits, std::memory_order_release);
        g_convoLastAppliedRev.store(g_convoSettingsRev.load(std::memory_order_acquire),
                                    std::memory_order_release);
    }

    // Log the first ~20 applies, then 1 in 120 (once every ~2s), so the log shows it's re-applying
    // continuously without spamming.
    const uint64_t n = g_convoApplyLogs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || !ok || (n % 120) == 0)
    {
        LogLine("[CONVO] apply n=" + std::to_string(n) +
                " ok=" + std::to_string(ok ? 1 : 0) +
                " scale=" + std::to_string(scaleX) +
                " panel=" + PtrText(panel));
    }
}

void ApplyNativeSubtitleToConvoPanel(void* panel) noexcept
{
    if (g_applyingPchud) return;
    if (!g_nativeSubMove.load(std::memory_order_acquire)) return;
    if (!PointerLooksCanonicalAligned(panel) || !Readable(panel, 0x100)) return;

    const std::uintptr_t panelBits = reinterpret_cast<std::uintptr_t>(panel);
    const uint64_t rev = g_nativeSubSettingsRev.load(std::memory_order_acquire);
    if (panelBits == g_lastAppliedNativeSubPanel.load(std::memory_order_acquire) &&
        rev == g_nativeSubLastAppliedRev.load(std::memory_order_acquire))
    {
        return;
    }

    const float scaleX = g_nativeSubScaleX.load(std::memory_order_relaxed);
    const float scaleY = g_nativeSubScaleY.load(std::memory_order_relaxed);
    const float offsetX = g_nativeSubPosXFrac.load(std::memory_order_relaxed);
    const float offsetY = g_nativeSubPosYFrac.load(std::memory_order_relaxed);

    static const PanelFloatPaths kSubtitleCandidates[] =
    {
        { L"_root.subtitle._xscale",      L"_root.subtitle._yscale",      L"_root.subtitle._x",      L"_root.subtitle._y" },
        { L"_root.subtitles._xscale",     L"_root.subtitles._yscale",     L"_root.subtitles._x",     L"_root.subtitles._y" },
        { L"_root.Subtitle._xscale",      L"_root.Subtitle._yscale",      L"_root.Subtitle._x",      L"_root.Subtitle._y" },
        { L"_root.Subtitles._xscale",     L"_root.Subtitles._yscale",     L"_root.Subtitles._x",     L"_root.Subtitles._y" },
        { L"_root.dialogSubtitle._xscale",L"_root.dialogSubtitle._yscale",L"_root.dialogSubtitle._x",L"_root.dialogSubtitle._y" },
        { L"_root.DialogSubtitle._xscale",L"_root.DialogSubtitle._yscale",L"_root.DialogSubtitle._x",L"_root.DialogSubtitle._y" },
        { L"_root.caption._xscale",       L"_root.caption._yscale",       L"_root.caption._x",       L"_root.caption._y" },
        { L"_root.Caption._xscale",       L"_root.Caption._yscale",       L"_root.Caption._x",       L"_root.Caption._y" },
        { L"_root.mcSubtitle._xscale",    L"_root.mcSubtitle._yscale",    L"_root.mcSubtitle._x",    L"_root.mcSubtitle._y" },
        { L"_root.txtSubtitle._xscale",   L"_root.txtSubtitle._yscale",   L"_root.txtSubtitle._x",   L"_root.txtSubtitle._y" },
        { L"_root.oSubtitle._xscale",     L"_root.oSubtitle._yscale",     L"_root.oSubtitle._x",     L"_root.oSubtitle._y" },
        { L"_root.oSubtitles._xscale",    L"_root.oSubtitles._yscale",    L"_root.oSubtitles._x",    L"_root.oSubtitles._y" }
    };

    bool ok = false;
    for (const auto& paths : kSubtitleCandidates)
    {
        ok = ApplyScaleOffsetToPanelPaths(panel, paths, scaleX, scaleY, offsetX, offsetY) || ok;
    }

    if (ok)
    {
        g_lastAppliedNativeSubPanel.store(panelBits, std::memory_order_release);
        g_nativeSubLastAppliedRev.store(rev, std::memory_order_release);
    }

    const uint64_t n = g_nativeSubMoveLogs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || !ok)
    {
        LogLine("[SUBMOVIE] n=" + std::to_string(n) +
                " ok=" + std::to_string(ok ? 1 : 0) +
                " panel=" + PtrText(panel) +
                " offset=[" + std::to_string(offsetX) + "," + std::to_string(offsetY) + "]" +
                " scale=[" + std::to_string(scaleX) + "," + std::to_string(scaleY) + "]");
    }
}

// Returns the panel IFF this context is an ACTIVE BioSFHandler_Conversation, read fresh (alive this instant).
void* ConvoHandlerLivePanel(void* context) noexcept
{
    const char* cls = ClassNameSEH(context);
    const char* name = ObjectNameSEH(context);
    if ((cls == nullptr || std::strstr(cls, "BioSFHandler_Conversation") == nullptr) &&
        (name == nullptr || std::strstr(name, "BioSFHandler_Conversation") == nullptr))
    {
        return nullptr;
    }

    void* panel = nullptr;
    if (ReadPtrAtSEH(context, kPchudHandlerPanel, &panel) &&
        PointerLooksCanonicalAligned(panel) &&
        Readable(panel, 0x100))
    {
        return panel;
    }
    return nullptr;
}

// Returns the panel IFF this context is an ACTIVE BioSFHandler_DesignerUI, read fresh (alive this instant).
// Third twin of PchudHandlerLivePanel/ConvoHandlerLivePanel - identical shape, different class string.
void* DesignerUiHandlerLivePanel(void* context) noexcept
{
    const char* cls = ClassNameSEH(context);
    const char* name = ObjectNameSEH(context);
    if ((cls == nullptr || std::strstr(cls, "BioSFHandler_DesignerUI") == nullptr) &&
        (name == nullptr || std::strstr(name, "BioSFHandler_DesignerUI") == nullptr))
    {
        return nullptr;
    }

    void* panel = nullptr;
    if (ReadPtrAtSEH(context, kPchudHandlerPanel, &panel) &&
        PointerLooksCanonicalAligned(panel) &&
        Readable(panel, 0x100))
    {
        return panel;
    }
    return nullptr;
}

// Designer UI twin of ApplyConvoToLivePanel: same always-reapply discipline (a scripted overlay's intro
// animation can reset _root scale the same way the conversation movie's does), no per-element sub-table -
// this is a master scale/offset only, matching the actual ask ("let me move that thing").
void ApplyDesignerUiToLivePanel(void* panel) noexcept
{
    if (g_applyingPchud) return;
    if (!g_designerUiEnabled.load(std::memory_order_acquire)) return;
    if (!PointerLooksCanonicalAligned(panel) || !Readable(panel, 0x100)) return;

    const std::uintptr_t panelBits = reinterpret_cast<std::uintptr_t>(panel);

    // [LASERUI] while the laser layout is latched, this panel gets the LASER transform set (default
    // identity = vanilla placement) instead of the timer-tuned master values. Bind the latch to the
    // panel instance on first apply; a different panel instance later = the movie was torn down
    // (left the area / level change) -> unlatch back to the master set.
    bool laser = g_laserUiLatched.load(std::memory_order_acquire);
    if (laser)
    {
        const std::uintptr_t rec = g_laserUiPanel.load(std::memory_order_relaxed);
        if (rec == 0)
        {
            g_laserUiPanel.store(panelBits, std::memory_order_relaxed);
        }
        else if (rec != panelBits)
        {
            g_laserUiLatched.store(false, std::memory_order_release);
            g_laserUiPanel.store(0, std::memory_order_relaxed);
            LogLine("[LASERUI] laser layout unlatched (designer panel instance changed)");
            laser = false;
        }
    }
    // Laser controls disabled while latched = apply IDENTITY (actively restores the game's own
    // placement; a plain skip would leave whatever transform was last applied stuck on the panel).
    const bool laserCustom = laser && g_laserUiEnabled.load(std::memory_order_acquire);
    const float scaleX = laser ? (laserCustom ? g_laserUiScaleX.load(std::memory_order_relaxed) : 1.0f)
                               : g_designerUiScaleX.load(std::memory_order_relaxed);
    const float scaleY = laser ? (laserCustom ? g_laserUiScaleY.load(std::memory_order_relaxed) : 1.0f)
                               : g_designerUiScaleY.load(std::memory_order_relaxed);
    const float offsetX = laser ? (laserCustom ? g_laserUiOffsetX.load(std::memory_order_relaxed) : 0.0f)
                                : g_designerUiOffsetX.load(std::memory_order_relaxed);
    const float offsetY = laser ? (laserCustom ? g_laserUiOffsetY.load(std::memory_order_relaxed) : 0.0f)
                                : g_designerUiOffsetY.load(std::memory_order_relaxed);
    const bool ok = ApplyScaleOffsetToPanel(panel, scaleX, scaleY, offsetX, offsetY);

    if (ok)
    {
        g_lastAppliedDesignerUiPanel.store(panelBits, std::memory_order_release);
        g_designerUiLastAppliedRev.store(g_designerUiSettingsRev.load(std::memory_order_acquire),
                                         std::memory_order_release);
    }

    const uint64_t n = g_designerUiApplyLogs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || !ok || (n % 120) == 0)
    {
        LogLine("[DESIGNERUI] apply n=" + std::to_string(n) +
                " ok=" + std::to_string(ok ? 1 : 0) +
                " laser=" + std::to_string(laser ? 1 : 0) +
                " scale=" + std::to_string(scaleX) +
                " offset=[" + std::to_string(offsetX) + "," + std::to_string(offsetY) + "]" +
                " panel=" + PtrText(panel));
    }
}

void DumpSubtitleOwnershipCandidates() noexcept
{
    if (g_subtitleHuntDumped.exchange(true, std::memory_order_acq_rel)) return;

    ObjectArrayHeader* objects = ObjectsSEH();
    if (objects == nullptr || !Readable(objects->data, sizeof(void*) * static_cast<size_t>(objects->count)))
    {
        LogLine("[SUBHUNT] object table unavailable.");
        return;
    }

    LogLine("[SUBHUNT] begin object/function scan count=" + std::to_string(objects->count));
    int logged = 0;
    for (int32_t index = 0; index < objects->count && logged < 160; ++index)
    {
        void* obj = nullptr;
        if (!ReadObjectSlotSEH(objects->data, index, &obj)) break;
        if (!PointerLooksCanonicalAligned(obj) || !Readable(obj, 0x60)) continue;

        const char* cls = ClassNameSEH(obj);
        const char* name = ObjectNameSEH(obj);
        const char* outer = OuterNameSEH(obj);
        if (!IsSubtitleHuntRelated(cls, name, outer)) continue;

        LogLine("[SUBHUNT_OBJ] cls=" + SafeAnsi(cls) +
                " name=" + SafeAnsi(name) +
                " outer=" + SafeAnsi(outer) +
                " ptr=" + PtrText(obj));
        ++logged;
    }
    LogLine("[SUBHUNT] scan complete logged=" + std::to_string(logged));
}

void DumpSubtitleFunctionCandidates() noexcept
{
    static bool s_dumped = false;
    if (s_dumped) return;
    s_dumped = true;

    ObjectArrayHeader* objects = ObjectsSEH();
    if (objects == nullptr || !Readable(objects->data, sizeof(void*) * static_cast<size_t>(objects->count)))
    {
        LogLine("[SUBFUN] object table unavailable.");
        return;
    }

    int logged = 0;
    for (int32_t index = 0; index < objects->count && logged < 120; ++index)
    {
        void* obj = nullptr;
        if (!ReadObjectSlotSEH(objects->data, index, &obj)) break;
        if (!PointerLooksCanonicalAligned(obj) || !Readable(obj, 0x60)) continue;

        const char* cls = ClassNameSEH(obj);
        if (cls == nullptr || std::strcmp(cls, "Function") != 0) continue;
        const char* name = ObjectNameSEH(obj);
        const char* outer = OuterNameSEH(obj);
        if (!IsSubtitleHuntRelated(cls, name, outer)) continue;

        LogLine("[SUBFUN] outer=" + SafeAnsi(outer) +
                " name=" + SafeAnsi(name) +
                " ptr=" + PtrText(obj));
        ++logged;
    }
    LogLine("[SUBFUN] scan complete logged=" + std::to_string(logged));
}

void MaybeLogSubtitleOwnershipEvent(void* context, void* function) noexcept
{
    if (g_subtitleHuntEventLogs.load(std::memory_order_relaxed) >= 80) return;

    const char* ctxCls = ClassNameSEH(context);
    const char* ctxName = ObjectNameSEH(context);
    const char* ctxOuter = OuterNameSEH(context);
    const char* fnCls = ClassNameSEH(function);
    const char* fnName = ObjectNameSEH(function);
    const char* fnOuter = OuterNameSEH(function);
    if (!IsSubtitleHuntRelated(ctxCls, ctxName, ctxOuter) &&
        !IsSubtitleHuntRelated(fnCls, fnName, fnOuter))
    {
        return;
    }

    g_subtitleHuntEventLogs.fetch_add(1, std::memory_order_relaxed);
    LogLine("[SUBHUNT_EVT] ctxCls=" + SafeAnsi(ctxCls) +
            " ctxName=" + SafeAnsi(ctxName) +
            " ctxOuter=" + SafeAnsi(ctxOuter) +
            " fnCls=" + SafeAnsi(fnCls) +
            " fnName=" + SafeAnsi(fnName) +
            " fnOuter=" + SafeAnsi(fnOuter) +
            " ctx=" + PtrText(context) +
            " fn=" + PtrText(function));
}

void RunPostRenderWithRedraw(void* context, void* function, void* params, void* result, ProcessEventFn original) noexcept;
bool RunNativeSubtitleRegionOverride(void* context, void* function, void* params, void* result, ProcessEventFn original) noexcept;
bool RunNativeSubtitleDrawMove(void* context, void* function, void* params, void* result, ProcessEventFn original) noexcept;
bool IsBioSubtitles(void* obj) noexcept;
bool IsDefaultArchetype(void* obj) noexcept;
void ForceBioSubtitlesModeNow(void* obj, int mode, const char* reason) noexcept;

// [PANELDISC] One-off discovery pass (2026-07-11): the "N Charges Remaining" / countdown timer HUD panel
// being repositioned is NOT one of the 13 kHudElemNames - those came from an OFFLINE parse of
// ONE specific SWF (swf_GUI_SF_HUD_PC_ME_HUD), so anything from a DIFFERENT movie was never discovered,
// which is exactly why the existing "move any element" picker can't reach it - it was never wired up, not
// broken. This logs every DISTINCT context class name seen via ProcessEvent (dedup by interned FName
// pointer - UE3 FNames are pointer-stable, so the same class returns the same pointer every time, making
// this a cheap set membership check, not a string compare per call). Self-caps at 400 distinct entries so
// it can't run away. Trigger the charge-timer moment once with this build running and grep the log for
// "PANELDISC" - the class name that shows up right when it appears on screen is the target.
void MaybeDiscoverPanelClass(void* context) noexcept
{
    constexpr int kPanelDiscCap = 400;
    static const void* s_seen[kPanelDiscCap] = {};
    static std::atomic<int> s_seenCount{0};

    if (context == nullptr) return;
    const int count = s_seenCount.load(std::memory_order_relaxed);
    if (count >= kPanelDiscCap) return;

    const char* cls = ClassNameSEH(context);
    if (cls == nullptr || cls[0] == '\0') return;

    // FNames are interned (one canonical UObject per unique name), so ClassNameSEH's returned pointer is
    // the SAME const char* every time this class appears - a linear pointer scan is enough to dedup.
    for (int i = 0; i < count; ++i)
    {
        if (s_seen[i] == static_cast<const void*>(cls)) return;   // already logged this class
    }

    const int slot = s_seenCount.fetch_add(1, std::memory_order_relaxed);
    if (slot >= kPanelDiscCap) return;
    s_seen[slot] = static_cast<const void*>(cls);

    const char* objName = ObjectNameSEH(context);
    const char* outerName = OuterNameSEH(context);
    LogLine(std::string("[PANELDISC] #") + std::to_string(slot) + " class=" + cls +
            " obj=" + (objName ? objName : "?") + " outer=" + (outerName ? outerName : "?"));
}

// [LASERUI] Latch laser-layout mode whenever the Kismet layout action executes through ProcessEvent.
// First sighting pays one strcmp per event; after that it's an interned-FName pointer compare (the
// same fast-path trick REFLRATE uses). Latching is idempotent; unlatch lives in the designer apply
// (panel-instance change = the movie was torn down / level changed).
void MaybeLatchLaserLayout(void* context) noexcept
{
    if (context == nullptr) return;
    const char* cls = ClassNameSEH(context);
    if (cls == nullptr) return;
    const std::uintptr_t cached = g_laserSeqClsName.load(std::memory_order_relaxed);
    if (cached != 0)
    {
        if (reinterpret_cast<std::uintptr_t>(cls) != cached) return;
    }
    else
    {
        if (std::strcmp(cls, "BioSeqAct_DUISetLaserLayout") != 0) return;
        g_laserSeqClsName.store(reinterpret_cast<std::uintptr_t>(cls), std::memory_order_relaxed);
    }
    if (!g_laserUiLatched.exchange(true, std::memory_order_acq_rel))
    {
        g_laserUiPanel.store(0, std::memory_order_relaxed);
        LogLine("[LASERUI] laser layout latched (DUISetLaserLayout fired) - laser transform set active");
    }
}

// ============================ [REFLRATE] ============================
// THE reflection head-tracking-shake fix (2026-07-15). The REFLLOG evidence run proved the
// reflection capture camera record is a same-frame, exactly mirror-conjugated copy of the
// head-rotated main view (rotation deltas match to 5 decimals, axis (x,y,z)->(-x,-y,z)) - the
// record was NEVER wrong, which is why rotating it (reflfix v5/v6) doubled the error and freezing
// it (v7) made it permanently stale. The real defect is CADENCE: capture CB writes land on only
// ~61% of presents (gap histogram: 192x every-2nd-present, 113x consecutive) while the main
// camera writes EVERY present - at the measured 60fps that is UE3's stock USceneCaptureComponent
// FrameRate=30 throttle. Every skipped present shows a reflection rendered from LAST frame's head
// pose against THIS frame's view: a one-frame misregistration oscillating at ~30Hz = "jerks like
// it wants to move", identical in Stereo 2 and Mono (mode-independent, exactly as reported).
// Fix: raise FrameRate on every SceneCaptureReflectComponent so the capture re-renders every
// frame. FrameRate is CPF_Const with a dedicated native SetFrameRate() (LE1 SDK: offset 0xC0) -
// the native exists because the render-side probe caches the interval, so it gets called via the
// ORIGINAL ProcessEvent (game thread) and verify by re-reading the property; a direct float write
// is the logged fallback. Objects are found and patched INSIDE one hook invocation - UE3 GC runs
// on this same thread between ticks, so nothing can be freed underneath (the FPQUAR lesson).
// Scan is incremental (20k objects per 30ms slice, ~2s pause between sweeps) so new instances
// from level transitions get patched automatically. Kill switch: MELEVR_DISABLE_REFLRATE.txt.
bool CallOriginalProcessEventSEH(ProcessEventFn original, void* context, void* function, void* params, void* result) noexcept;

constexpr float kReflRateTarget = 1000.0f;                 // captures/sec cap -> every frame
constexpr std::uintptr_t kSceneCaptureFrameRate = 0xC0;    // USceneCaptureComponent::FrameRate

bool ReflRateMarkerDisabled() noexcept
{
    // cached 2s poll; same exe-dir resolution as render_hook's MarkerEnabled
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
                if (swprintf_s(marker, L"%sMELEVR_DISABLE_REFLRATE.txt", exePath) > 0)
                {
                    disabled = GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES;
                }
            }
        }
        s_disabled = disabled;
    }
    return s_disabled;
}

void MaybeBoostReflectionCaptureRate() noexcept
{
    // Game-thread only (called from the ProcessEvent hook), so plain statics are safe.
    static bool s_inBoost = false;              // guard against re-entry via its own ProcessEvent call
    static int32_t s_cursor = 0;                // incremental GObjects scan position
    static ULONGLONG s_nextSliceMs = 0;         // slice pacing (ProcessEvent fires thousands/frame)
    static ULONGLONG s_sweepNotBefore = 0;      // pause between full sweeps
    static void* s_setFrameRateFn = nullptr;
    static bool s_fnSearched = false;
    static const void* s_reflClsName = nullptr; // interned FName ptr -> pointer-compare fast path
    static unsigned s_patched = 0;
    static unsigned s_discLogs = 0;

    if (s_inBoost) return;
    const ULONGLONG now = GetTickCount64();
    if (now < s_nextSliceMs) return;
    s_nextSliceMs = now + 30;
    if (s_cursor == 0 && now < s_sweepNotBefore) return;
    if (ReflRateMarkerDisabled()) return;

    ObjectArrayHeader* objects = ObjectsSEH();
    if (objects == nullptr) return;

    if (!s_fnSearched)
    {
        s_fnSearched = true;
        s_setFrameRateFn = FindFunctionByOuterName("SceneCaptureComponent", "SetFrameRate");
        LogLine(std::string("[REFLRATE] SetFrameRate function ") +
                (s_setFrameRateFn != nullptr ? PtrText(s_setFrameRateFn)
                                             : std::string("NOT FOUND (direct-write fallback only)")));
    }

    constexpr int32_t kSlice = 20000;
    int32_t end = s_cursor + kSlice;
    if (end > objects->count) end = objects->count;
    for (int32_t i = s_cursor; i < end; ++i)
    {
        void* obj = nullptr;
        if (!ReadObjectSlotSEH(objects->data, i, &obj)) break;
        if (!PointerLooksCanonicalAligned(obj)) continue;
        const char* cls = ClassNameSEH(obj);
        if (cls == nullptr) continue;
        if (s_reflClsName != nullptr)
        {
            if (static_cast<const void*>(cls) != s_reflClsName) continue;
        }
        else
        {
            if (std::strstr(cls, "SceneCapture") == nullptr) continue;
            // visibility: log the SceneCapture* classes the scan meets so a differently-named
            // reflect class can't hide (first 12 distinct sightings only)
            if (s_discLogs < 12)
            {
                ++s_discLogs;
                const char* objName = ObjectNameSEH(obj);
                LogLine(std::string("[REFLRATE] scan met class=") + cls +
                        " obj=" + (objName != nullptr ? objName : "?"));
            }
            if (std::strcmp(cls, "SceneCaptureReflectComponent") != 0) continue;
            s_reflClsName = static_cast<const void*>(cls);   // FNames interned: ptr-compare from here
        }

        float oldRate = 0.0f;
        if (!ReadFloatAtSEH(obj, kSceneCaptureFrameRate, &oldRate)) continue;
        if (!(oldRate >= 0.0f && oldRate < kReflRateTarget)) continue;   // boosted already, or NaN

        bool viaNative = false;
        if (s_setFrameRateFn != nullptr && g_originalProcessEvent != nullptr)
        {
            struct { float NewFrameRate; } parms = { kReflRateTarget };
            s_inBoost = true;
            CallOriginalProcessEventSEH(g_originalProcessEvent, obj, s_setFrameRateFn, &parms, nullptr);
            s_inBoost = false;
            float check = 0.0f;
            viaNative = ReadFloatAtSEH(obj, kSceneCaptureFrameRate, &check) && check >= kReflRateTarget;
        }
        if (!viaNative)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &kReflRateTarget, sizeof(bits));
            WriteU32AtSEH(obj, kSceneCaptureFrameRate, bits);
        }
        ++s_patched;
        char line[192] = {};
        std::snprintf(line, sizeof(line),
                      "[REFLRATE] boosted #%u obj=%p rate %.1f -> %.1f via %s",
                      s_patched, obj, oldRate, kReflRateTarget,
                      viaNative ? "SetFrameRate" : "direct write");
        LogLine(line);
    }
    s_cursor = (end >= objects->count) ? 0 : end;
    if (s_cursor == 0) s_sweepNotBefore = now + 2000;
}

void __fastcall ProcessEventHook(void* context, void* function, void* params, void* result) noexcept
{
    ProcessEventFn original = g_originalProcessEvent;
    MaybeDiscoverPanelClass(context);
    MaybeLatchLaserLayout(context);      // [LASERUI] mining-laser designer-UI layout detection
    MaybeBoostReflectionCaptureRate();   // [REFLRATE] reflection-capture cadence fix

    if ((g_nativeSubMove.load(std::memory_order_acquire) ||
         g_subtitleRedraw.load(std::memory_order_acquire)) &&
        !g_applyingPchud)
    {
        MaybeLogSubtitleOwnershipEvent(context, function);
    }

    if (!g_applyingPchud)
    {
        MaybeLogSubtitleRenderTrace(context, function);
    }

    // Native spoken-subtitle region: override the engine's subtitle region for the live draw path itself. This
    // happens before the legacy blank+redraw path and returns after calling original().
    // Runs even with the override disabled: passively logs the game's REAL region values per lane
    // ([SUBREGION_REAL]); only writes them back when the native-move toggle is on.
    if (!g_applyingPchud && original != nullptr &&
        RunNativeSubtitleRegionOverride(context, function, params, result, original))
    {
        return;
    }

    // BioHUD.PostRender: special-cased so the game's subtitle can be BLANKED before it draws, then redrawn on
    // top at the mod's position/scale. Runs original() itself (with the blank in place), so return after.
    if (false &&   // SUBTITLE OVERLAY DISABLED FOR TEST 2026-07-07 (see Tick block note) - remove "false &&" to restore
        !g_applyingPchud && function != nullptr && function == g_postRenderFn &&
        g_subtitleRedraw.load(std::memory_order_acquire) && original != nullptr)
    {
        RunPostRenderWithRedraw(context, function, params, result, original);
        return;
    }

    if (!g_applyingPchud && original != nullptr && g_subtitleForce.load(std::memory_order_acquire) &&
        context != nullptr && IsBioSubtitles(context) &&
        (function == g_bioSubtitlesDisplayFn || function == g_bioSubtitlesUpdateFn))
    {
        original(context, function, params, result);
        ForceBioSubtitlesModeNow(context, g_subtitleMode.load(std::memory_order_relaxed),
                                 function == g_bioSubtitlesDisplayFn ? "DisplaySubtitle" : "UpdateSubtitles");
        return;
    }

    if (original != nullptr)
    {
        original(context, function, params, result);
    }

    // Ignore events that this own SetVariableFloat call triggered (reentrancy guard).
    if (g_applyingPchud) return;

    // RELIABLE conversation detection for the overlay standdown - runs on EVERY event while the overlay is
    // on, independent of the convo-movie feature, so the overlay reliably stays off for the whole convo (the
    // game shows its own subtitle there). Cheap pointer-compare fast path; throttled class check to (re)find
    // the handler. Generous 120-frame (~2s) window bridges gaps between the handler's events.
    if (g_subtitleRedraw.load(std::memory_order_acquire))
    {
        const uint64_t frame = g_subtitleTraceFrame.load(std::memory_order_acquire);
        void* ch = reinterpret_cast<void*>(g_convoHandler.load(std::memory_order_relaxed));
        if (ch != nullptr && context == ch)
        {
            g_convoActiveUntilFrame.store(frame + 120, std::memory_order_release);
        }
        else if ((g_convoStandTick.fetch_add(1, std::memory_order_relaxed) & 0x3F) == 0)
        {
            void* panel = ConvoHandlerLivePanel(context);
            if (panel != nullptr)
            {
                g_convoHandler.store(reinterpret_cast<std::uintptr_t>(context), std::memory_order_relaxed);
                g_convoActiveUntilFrame.store(frame + 120, std::memory_order_release);
            }
        }
    }

    const bool pchudOn = g_enabled.load(std::memory_order_acquire);
    const bool convoOn = g_convoEnabled.load(std::memory_order_acquire);
    const bool designerUiOn = g_designerUiEnabled.load(std::memory_order_acquire);
    // Fully inert while ALL THREE movies are disabled: no panel tracking, no apply, no ProcessEvent calls.
    if (!pchudOn && !convoOn && !designerUiOn) return;

    // HOT PATH (cheap): known handler -> read its live panel + apply. Just a pointer compare per event, no
    // class-name resolution. Each handler fires ~once/frame, so its apply lands then.
    if (pchudOn)
    {
        void* handler = reinterpret_cast<void*>(g_pchudHandler.load(std::memory_order_relaxed));
        if (handler != nullptr && context == handler)
        {
            void* panel = nullptr;
            if (ReadPtrAtSEH(context, kPchudHandlerPanel, &panel) &&
                PointerLooksCanonicalAligned(panel) && Readable(panel, 0x100))
            {
                ApplyToLivePanel(panel);
            }
            return;
        }
    }
    if (convoOn)
    {
        void* handler = reinterpret_cast<void*>(g_convoHandler.load(std::memory_order_relaxed));
        if (handler != nullptr && context == handler)
        {
            void* panel = nullptr;
            if (ReadPtrAtSEH(context, kPchudHandlerPanel, &panel) &&
                PointerLooksCanonicalAligned(panel) && Readable(panel, 0x100))
            {
                ApplyConvoToLivePanel(panel);
                ApplyNativeSubtitleToConvoPanel(panel);
            }
            return;
        }
    }
    if (designerUiOn)
    {
        void* handler = reinterpret_cast<void*>(g_designerUiHandler.load(std::memory_order_relaxed));
        if (handler != nullptr && context == handler)
        {
            void* panel = nullptr;
            if (ReadPtrAtSEH(context, kPchudHandlerPanel, &panel) &&
                PointerLooksCanonicalAligned(panel) && Readable(panel, 0x100))
            {
                ApplyDesignerUiToLivePanel(panel);
            }
            return;
        }
    }

    if (g_nativeSubMove.load(std::memory_order_acquire))
    {
        void* panel = ConvoHandlerLivePanel(context);
        if (panel != nullptr)
        {
            g_convoHandler.store(reinterpret_cast<std::uintptr_t>(context), std::memory_order_relaxed);
            ApplyNativeSubtitleToConvoPanel(panel);
            return;
        }
    }

    // DISCOVERY (throttled ~1/32 events each): the expensive class-name check finds a handler, or re-finds it
    // if the game rebuilt it (new pointer). Bounds the per-event cost so it can't hang gameplay.
    if (pchudOn && (g_pchudDiscTick.fetch_add(1, std::memory_order_relaxed) & 0x1F) == 0)
    {
        void* panel = PchudHandlerLivePanel(context);
        if (panel != nullptr)
        {
            g_pchudHandler.store(reinterpret_cast<std::uintptr_t>(context), std::memory_order_relaxed);
            ApplyToLivePanel(panel);
            return;
        }
    }
    if (convoOn && (g_convoDiscTick.fetch_add(1, std::memory_order_relaxed) & 0x1F) == 0)
    {
        void* panel = ConvoHandlerLivePanel(context);
        if (panel != nullptr)
        {
            g_convoHandler.store(reinterpret_cast<std::uintptr_t>(context), std::memory_order_relaxed);
            ApplyConvoToLivePanel(panel);
            ApplyNativeSubtitleToConvoPanel(panel);
        }
    }
    if (designerUiOn && (g_designerUiDiscTick.fetch_add(1, std::memory_order_relaxed) & 0x1F) == 0)
    {
        void* panel = DesignerUiHandlerLivePanel(context);
        if (panel != nullptr)
        {
            g_designerUiHandler.store(reinterpret_cast<std::uintptr_t>(context), std::memory_order_relaxed);
            ApplyDesignerUiToLivePanel(panel);
        }
    }
}

// True iff obj's CLASS name is exactly "BioSubtitles" (the UBioSubtitles instance targeted here).
bool IsBioSubtitles(void* obj) noexcept
{
    if (!PointerLooksCanonicalAligned(obj) || !Readable(obj, 0x82)) return false;
    const char* cls = ClassNameSEH(obj);
    return cls != nullptr && std::strcmp(cls, "BioSubtitles") == 0;
}

void ForceBioSubtitlesModeNow(void* obj, int mode, const char* reason) noexcept
{
    if (!IsBioSubtitles(obj) || IsDefaultArchetype(obj)) return;
    if (mode < 1 || mode > 4) mode = 3;
    const uint8_t m = static_cast<uint8_t>(mode);
    const uint8_t beforeDefault = ReadU8AtSEH(obj, kSubtitleDefaultRenderMode);
    const uint8_t beforeCurrent = ReadU8AtSEH(obj, kSubtitleCurrentRenderMode);
    const bool ok = WriteU8AtSEH(obj, kSubtitleDefaultRenderMode, m) &&
                    WriteU8AtSEH(obj, kSubtitleCurrentRenderMode, m);
    static uint64_t s_logs = 0;
    if (s_logs++ < 40 || !ok)
    {
        LogLine("[SUBFORCE_EVT] reason=" + std::string(reason ? reason : "?") +
                " obj=" + PtrText(obj) +
                " before=[" + std::to_string(static_cast<int>(beforeDefault)) + "," + std::to_string(static_cast<int>(beforeCurrent)) + "]" +
                " after=" + std::to_string(mode) +
                " ok=" + std::to_string(ok ? 1 : 0));
    }
}

// The class-default/archetype object (name starts with "Default__") is a template, not the live instance that
// renders subtitles - writing to it does nothing visible. Skip it.
bool IsDefaultArchetype(void* obj) noexcept
{
    const char* name = ObjectNameSEH(obj);
    return name != nullptr && std::strncmp(name, "Default__", 9) == 0;
}

// Read the subtitle text (FString m_sSubtitle @0x60: {wchar_t* data; int32 count; int32 max}) into an ANSI
// buffer for logging, so it's possible to PROVE which object holds a given on-screen bark. Returns the wchar count.
int32_t ReadSubtitleTextSEH(void* obj, char* out, int outSize) noexcept
{
    if (out == nullptr || outSize <= 0) return 0;
    out[0] = '\0';
    void* data = nullptr;
    int32_t count = 0;
    if (!ReadPtrAtSEH(obj, 0x60, &data)) return 0;
    ReadU32AtSEH(obj, 0x68, reinterpret_cast<uint32_t*>(&count));
    if (data == nullptr || count <= 1 || !Readable(data, 2)) return count;
    __try
    {
        const wchar_t* w = reinterpret_cast<const wchar_t*>(data);
        int i = 0;
        for (; i + 1 < outSize && w[i] != L'\0'; ++i)
        {
            const wchar_t c = w[i];
            out[i] = (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
        }
        out[i] = '\0';
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = '\0'; }
    return count;
}

// Full object-table scan for ALL live UBioSubtitles instances (NOT the archetype). There are several; the
// squad-bark text can live in any of them, so they all get collected. Only called on a cache miss (throttled).
int FindAllBioSubtitles(void** out, int maxN) noexcept
{
    ObjectArrayHeader* objects = ObjectsSEH();
    if (objects == nullptr || !Readable(objects->data, sizeof(void*) * static_cast<size_t>(objects->count)))
    {
        return 0;
    }
    int n = 0;
    for (int32_t index = 0; index < objects->count && n < maxN; ++index)
    {
        void* obj = nullptr;
        if (!ReadObjectSlotSEH(objects->data, index, &obj)) break;
        if (!IsBioSubtitles(obj) || IsDefaultArchetype(obj)) continue;
        out[n++] = obj;
        LogLine(std::string("[SUBTITLE] live instance #") + std::to_string(n) +
                " name=" + SafeAnsi(ObjectNameSEH(obj)) + " ptr=" + PtrText(obj));
    }
    return n;
}

bool WideTextEqualsSEH(const wchar_t* a, int32_t aCount, const wchar_t* b, int32_t bCount) noexcept
{
    if (a == nullptr || b == nullptr || aCount <= 1 || bCount <= 1) return false;
    if (!Readable(a, sizeof(wchar_t)) || !Readable(b, sizeof(wchar_t))) return false;
    const int32_t maxA = (std::min)(aCount, 512);
    const int32_t maxB = (std::min)(bCount, 512);
    __try
    {
        int32_t i = 0;
        for (; i < maxA && i < maxB; ++i)
        {
            const wchar_t ca = a[i];
            const wchar_t cb = b[i];
            if (ca != cb) return false;
            if (ca == L'\0') return i > 0;
        }
        return i > 0 &&
               (i == maxA || a[i] == L'\0') &&
               (i == maxB || b[i] == L'\0');
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool WideTextContainsSEH(const wchar_t* haystack, int32_t haystackCount, const wchar_t* needle, int32_t needleCount) noexcept
{
    if (haystack == nullptr || needle == nullptr || haystackCount <= 1 || needleCount <= 1) return false;
    if (!Readable(haystack, sizeof(wchar_t)) || !Readable(needle, sizeof(wchar_t))) return false;
    const int32_t hn = (std::min)(haystackCount - 1, 512);
    const int32_t nn = (std::min)(needleCount - 1, 512);
    if (hn <= 0 || nn <= 0 || nn > hn) return false;

    __try
    {
        for (int32_t i = 0; i <= hn - nn; ++i)
        {
            int32_t j = 0;
            for (; j < nn; ++j)
                if (haystack[i + j] != needle[j]) break;
            if (j == nn) return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return false;
}

bool WideTextPrefixMatchSEH(const wchar_t* a, int32_t aCount, const wchar_t* b, int32_t bCount, int32_t minChars) noexcept
{
    if (a == nullptr || b == nullptr || aCount <= 1 || bCount <= 1) return false;
    if (!Readable(a, sizeof(wchar_t)) || !Readable(b, sizeof(wchar_t))) return false;
    const int32_t limit = (std::min)((std::min)(aCount - 1, bCount - 1), 96);
    if (limit < minChars) return false;
    __try
    {
        int32_t matched = 0;
        while (matched < limit)
        {
            const wchar_t ca = a[matched];
            const wchar_t cb = b[matched];
            if (ca == L'\0' || cb == L'\0') break;
            if (ca != cb) return false;
            ++matched;
        }
        return matched >= minChars;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool WideTextHasDialogueColonSEH(const wchar_t* text, int32_t count) noexcept
{
    if (text == nullptr || count <= 3 || !Readable(text, sizeof(wchar_t))) return false;
    const int32_t n = (std::min)(count - 1, 64);
    __try
    {
        for (int32_t i = 1; i + 1 < n; ++i)
        {
            if (text[i] == L':' && text[i + 1] == L' ') return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return false;
}

bool WideTextHasSpaceSEH(const wchar_t* text, int32_t count) noexcept
{
    if (text == nullptr || count <= 2 || !Readable(text, sizeof(wchar_t))) return false;
    const int32_t n = (std::min)(count - 1, 128);
    __try
    {
        for (int32_t i = 0; i < n; ++i)
            if (text[i] == L' ') return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return false;
}

bool WidePreviewSEH(const wchar_t* text, int32_t count, char* out, size_t outSize) noexcept
{
    if (out == nullptr || outSize == 0) return false;
    out[0] = '\0';
    if (text == nullptr || count <= 1 || !Readable(text, sizeof(wchar_t))) return false;
    __try
    {
        const int32_t n = (std::min)(count - 1, static_cast<int32_t>(outSize - 1));
        for (int32_t i = 0; i < n; ++i)
        {
            const wchar_t c = text[i];
            out[i] = (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
        }
        out[n] = '\0';
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool CopyWideTextSEH(const wchar_t* src, int32_t count, wchar_t* dst, int32_t dstCount) noexcept
{
    if (src == nullptr || dst == nullptr || count <= 1 || dstCount <= 1) return false;
    if (!Readable(src, sizeof(wchar_t))) return false;
    __try
    {
        const int32_t n = (std::min)(count - 1, dstCount - 1);
        for (int32_t i = 0; i < n; ++i)
        {
            dst[i] = src[i];
            if (src[i] == L'\0')
            {
                dst[i] = L'\0';
                return i > 0;
            }
        }
        dst[n] = L'\0';
        return n > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        dst[0] = L'\0';
        return false;
    }
}

bool DrawTextMatchesLiveSubtitle(const DrawTextParams* params, float curX, float curY, int32_t sizeX, int32_t sizeY) noexcept
{
    if (params == nullptr || params->text.data == nullptr || params->text.count <= 1) return false;
    if (params->text.count > 512 || !Readable(params->text.data, sizeof(wchar_t))) return false;

    const int32_t activeLen = g_activeSubtitleTextLen.load(std::memory_order_acquire);
    if (activeLen > 1 && g_activeSubtitleText[0] != L'\0')
    {
        if (WideTextEqualsSEH(params->text.data, params->text.count, g_activeSubtitleText, activeLen) ||
            WideTextContainsSEH(params->text.data, params->text.count, g_activeSubtitleText, activeLen) ||
            WideTextContainsSEH(g_activeSubtitleText, activeLen, params->text.data, params->text.count) ||
            WideTextPrefixMatchSEH(params->text.data, params->text.count, g_activeSubtitleText, activeLen, 10) ||
            WideTextPrefixMatchSEH(g_activeSubtitleText, activeLen, params->text.data, params->text.count, 10))
        {
            return true;
        }
    }

    constexpr int kMaxSubs = 8;
    static void* s_subs[kMaxSubs] = {};
    static int   s_n = 0;
    static uint32_t s_scan = 0;

    bool valid = s_n > 0;
    for (int i = 0; i < s_n && valid; ++i)
        if (!IsBioSubtitles(s_subs[i]) || IsDefaultArchetype(s_subs[i])) valid = false;
    if (!valid && (s_scan++ % 30) == 0) s_n = FindAllBioSubtitles(s_subs, kMaxSubs);

    for (int i = 0; i < s_n; ++i)
    {
        void* textData = nullptr;
        if (!ReadPtrAtSEH(s_subs[i], kSubtitleFString, &textData)) continue;
        const int32_t count = ReadI32AtSEH(s_subs[i], kSubtitleFString + 8);
        if (count <= 1) continue;
        const auto* liveText = reinterpret_cast<const wchar_t*>(textData);
        if (WideTextEqualsSEH(params->text.data, params->text.count, liveText, count) ||
            WideTextContainsSEH(params->text.data, params->text.count, liveText, count) ||
            WideTextContainsSEH(liveText, count, params->text.data, params->text.count) ||
            WideTextPrefixMatchSEH(params->text.data, params->text.count, liveText, count, 12) ||
            WideTextPrefixMatchSEH(liveText, count, params->text.data, params->text.count, 12))
        {
            return true;
        }
    }

    const bool topScreen = sizeY > 0 && curY >= -20.0f && curY <= (static_cast<float>(sizeY) * 0.35f);
    const bool reasonableX = sizeX <= 0 || (curX >= -100.0f && curX <= static_cast<float>(sizeX) + 100.0f);
    const bool centeredX = sizeX <= 0 || (curX >= (static_cast<float>(sizeX) * 0.15f) &&
                                           curX <= (static_cast<float>(sizeX) * 0.85f));
    const bool dialogueLine = params->text.count >= 12 && WideTextHasDialogueColonSEH(params->text.data, params->text.count);
    if (topScreen && reasonableX && dialogueLine)
    {
        const uint64_t n = g_nativeSubCandidateLogs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 20)
        {
            char preview[96] = {};
            WidePreviewSEH(params->text.data, params->text.count, preview, sizeof(preview));
            LogLine("[SUBMOVE_CAND] colon top-screen text='" + std::string(preview) +
                    "' cur=[" + std::to_string(curX) + "," + std::to_string(curY) + "]" +
                    " size=" + std::to_string(sizeX) + "x" + std::to_string(sizeY) +
                    " scale=[" + std::to_string(params->xscale) + "," + std::to_string(params->yscale) + "]");
        }
        return true;
    }

    const bool sentenceLike = params->text.count >= 18 && WideTextHasSpaceSEH(params->text.data, params->text.count);
    if (topScreen && centeredX && sentenceLike)
    {
        const uint64_t n = g_nativeSubCandidateLogs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 30)
        {
            char preview[96] = {};
            WidePreviewSEH(params->text.data, params->text.count, preview, sizeof(preview));
            LogLine("[SUBMOVE_CAND] sentence top-screen text='" + std::string(preview) +
                    "' cur=[" + std::to_string(curX) + "," + std::to_string(curY) + "]" +
                    " size=" + std::to_string(sizeX) + "x" + std::to_string(sizeY) +
                    " scale=[" + std::to_string(params->xscale) + "," + std::to_string(params->yscale) + "]");
        }
        return true;
    }

    return false;
}

bool CallOriginalProcessEventSEH(ProcessEventFn original, void* context, void* function, void* params, void* result) noexcept
{
    if (original == nullptr) return false;
    __try
    {
        original(context, function, params, result);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool RunNativeSubtitleDrawMove(void* context, void* function, void* params, void* result, ProcessEventFn original) noexcept
{
    if (!g_nativeSubMove.load(std::memory_order_acquire) ||
        original == nullptr ||
        function == nullptr ||
        function != g_canvasDrawTextFn ||
        context == nullptr ||
        params == nullptr ||
        !Readable(context, 0xB8) ||
        !Readable(params, sizeof(DrawTextParams)))
    {
        return false;
    }

    auto* draw = reinterpret_cast<DrawTextParams*>(params);
    int32_t sizeX = ReadI32AtSEH(context, kCanvasSizeX);
    int32_t sizeY = ReadI32AtSEH(context, kCanvasSizeY);
    if (sizeX <= 0 || sizeX > 16384) sizeX = 1920;
    if (sizeY <= 0 || sizeY > 16384) sizeY = 1080;

    float oldX = 0.0f;
    float oldY = 0.0f;
    ReadFloatAtSEH(context, kCanvasCurX, &oldX);
    ReadFloatAtSEH(context, kCanvasCurY, &oldY);

    if (!DrawTextMatchesLiveSubtitle(draw, oldX, oldY, sizeX, sizeY))
    {
        return false;
    }

    const float oldScaleX = draw->xscale;
    const float oldScaleY = draw->yscale;

    const float newX = g_nativeSubPosXFrac.load(std::memory_order_relaxed) * static_cast<float>(sizeX);
    const float newY = g_nativeSubPosYFrac.load(std::memory_order_relaxed) * static_cast<float>(sizeY);
    const float newScaleX = g_nativeSubScaleX.load(std::memory_order_relaxed);
    const float newScaleY = g_nativeSubScaleY.load(std::memory_order_relaxed);

    WriteFloatAtSEH(context, kCanvasCurX, newX);
    WriteFloatAtSEH(context, kCanvasCurY, newY);
    draw->xscale = newScaleX;
    draw->yscale = newScaleY;

    g_applyingPchud = true;
    const bool ok = CallOriginalProcessEventSEH(original, context, function, params, result);
    g_applyingPchud = false;

    WriteFloatAtSEH(context, kCanvasCurX, oldX);
    WriteFloatAtSEH(context, kCanvasCurY, oldY);
    draw->xscale = oldScaleX;
    draw->yscale = oldScaleY;

    const uint64_t n = g_nativeSubMoveLogs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 20 || !ok)
    {
        LogLine("[SUBMOVE] n=" + std::to_string(n) + " ok=" + std::to_string(ok ? 1 : 0) +
                " canvas=" + PtrText(context) +
                " size=" + std::to_string(sizeX) + "x" + std::to_string(sizeY) +
                " pos=[" + std::to_string(newX) + "," + std::to_string(newY) + "]" +
                " scale=[" + std::to_string(newScaleX) + "," + std::to_string(newScaleY) + "]");
    }

    return true;
}

bool RunNativeSubtitleRegionOverride(void* context, void* function, void* params, void* result, ProcessEventFn original) noexcept
{
    if (original == nullptr ||
        function == nullptr ||
        function != g_getSubtitleRegionFn ||
        params == nullptr ||
        !Readable(params, sizeof(SubtitleRegionParams)))
    {
        return false;
    }

    auto* region = reinterpret_cast<SubtitleRegionParams*>(params);
    g_applyingPchud = true;
    const bool ok = CallOriginalProcessEventSEH(original, context, function, params, result);
    g_applyingPchud = false;
    MaybeLogSubtitleRegionStack();

    // ALWAYS log the game's REAL region (before any override) whenever it changes. Every prior build only
    // logged the values that were written, so the true per-lane bands have never been observed. Game thread only,
    // so plain statics are fine.
    {
        static float s_last[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
        static uint64_t s_realLogs = 0;
        const bool changed = std::fabs(region->minX - s_last[0]) > 0.0005f ||
                             std::fabs(region->minY - s_last[1]) > 0.0005f ||
                             std::fabs(region->maxX - s_last[2]) > 0.0005f ||
                             std::fabs(region->maxY - s_last[3]) > 0.0005f;
        if ((changed && s_realLogs < 200) || !ok)
        {
            ++s_realLogs;
            LogLine("[SUBREGION_REAL] n=" + std::to_string(s_realLogs) +
                    " ok=" + std::to_string(ok ? 1 : 0) +
                    " ctxCls=" + SafeAnsi(ClassNameSEH(context)) +
                    " min=(" + std::to_string(region->minX) + "," + std::to_string(region->minY) + ")" +
                    " max=(" + std::to_string(region->maxX) + "," + std::to_string(region->maxY) + ")");
            s_last[0] = region->minX;
            s_last[1] = region->minY;
            s_last[2] = region->maxX;
            s_last[3] = region->maxY;
        }
    }

    if (!g_nativeSubMove.load(std::memory_order_acquire))
    {
        return true;   // passive: original ran, real region logged, nothing overridden
    }

    float minY = g_nativeSubPosYFrac.load(std::memory_order_relaxed);
    float maxY = g_nativeSubScaleY.load(std::memory_order_relaxed);
    float minX = g_nativeSubPosXFrac.load(std::memory_order_relaxed);
    float maxX = g_nativeSubScaleX.load(std::memory_order_relaxed);
    minY = (std::max)(0.0f, (std::min)(0.98f, minY));
    maxY = (std::max)(minY + 0.02f, (std::min)(1.0f, maxY));
    minX = (std::max)(0.0f, (std::min)(0.95f, minX));
    maxX = (std::max)(minX + 0.05f, (std::min)(1.0f, maxX));

    region->minX = minX;
    region->minY = minY;
    region->maxX = maxX;
    region->maxY = maxY;

    const uint64_t n = g_nativeSubMoveLogs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 24 || !ok)
    {
        LogLine("[SUBREGION] n=" + std::to_string(n) +
                " ok=" + std::to_string(ok ? 1 : 0) +
                " ctxCls=" + SafeAnsi(ClassNameSEH(context)) +
                " wrote min=(" + std::to_string(minX) + "," + std::to_string(minY) + ")" +
                " max=(" + std::to_string(maxX) + "," + std::to_string(maxY) + ")");
    }

    return true;
}

// Force the combat-bark subtitle render mode on the live UBioSubtitles. Validates the cached pointer cheaply
// each frame; re-scans (throttled) only on a miss. Writes both render-mode bytes so a fresh bark that set
// CurrentRenderMode=TOP gets flipped back to the forced mode before the next UpdateSubtitles reads it.
void TickSubtitles() noexcept
{
    if (!g_subtitleForce.load(std::memory_order_acquire)) return;

    // Present-thread-only cache of ALL live UBioSubtitles instances. Re-scan on a miss (throttled 1/60).
    constexpr int kMaxSubs = 8;
    static void* s_objs[kMaxSubs] = {};
    static int   s_count = 0;

    // Validate the cached set cheaply each frame; any invalid entry (freed on transition) triggers a rescan.
    bool valid = s_count > 0;
    for (int i = 0; i < s_count && valid; ++i)
        if (!IsBioSubtitles(s_objs[i]) || IsDefaultArchetype(s_objs[i])) valid = false;

    if (!valid)
    {
        if ((g_subtitleRescanTick.fetch_add(1, std::memory_order_relaxed) % 60) == 0)
            s_count = FindAllBioSubtitles(s_objs, kMaxSubs);
        if (s_count == 0) return;
    }

    int mode = g_subtitleMode.load(std::memory_order_relaxed);
    if (mode < 1 || mode > 4) mode = 3;   // clamp to a real, non-NONE position (never hide subtitles)
    const uint8_t m = static_cast<uint8_t>(mode);

    for (int i = 0; i < s_count; ++i)
    {
        void* obj = s_objs[i];
        // Diagnostics BEFORE the write: the game's mode + the actual text, so it's possible to PROVE which instance holds
        // an on-screen bark and whether writing the mode moves it.
        const uint8_t gameMode = ReadU8AtSEH(obj, kSubtitleCurrentRenderMode);
        char text[64] = {};
        const int32_t textLen = ReadSubtitleTextSEH(obj, text, sizeof(text));
        const bool hasText = textLen > 1;

        const bool ok = WriteU8AtSEH(obj, kSubtitleDefaultRenderMode, m) &&
                        WriteU8AtSEH(obj, kSubtitleCurrentRenderMode, m);

        // ALWAYS log the moment a subtitle is present (capped), to catch the exact bark + its object + mode.
        if (hasText)
        {
            const uint64_t t = g_subtitleTextLogs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (t <= 60)
                LogLine("[SUBTITLE] TEXT inst#" + std::to_string(i) + "/" + std::to_string(s_count) +
                        " gameMode=" + std::to_string(static_cast<int>(gameMode)) +
                        " forced=" + std::to_string(mode) + " ok=" + std::to_string(ok ? 1 : 0) +
                        " obj=" + PtrText(obj) + " text='" + text + "'");
        }
    }

    const uint64_t n = g_subtitleLogs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3)
        LogLine("[SUBTITLE] steering " + std::to_string(s_count) + " live instance(s), forced mode=" + std::to_string(mode));
}

void ProbeLiveBioSubtitles() noexcept
{
    static void* s_objs[8] = {};
    static int   s_count = 0;
    static uint32_t s_probeTick = 0;
    static char s_lastText[8][160] = {};
    static uint8_t s_lastMode[8] = {};

    if ((s_probeTick++ % 30) != 0) return;

    bool valid = s_count > 0;
    for (int i = 0; i < s_count && valid; ++i)
        if (!IsBioSubtitles(s_objs[i]) || IsDefaultArchetype(s_objs[i])) valid = false;
    if (!valid) s_count = FindAllBioSubtitles(s_objs, 8);

    for (int i = 0; i < s_count; ++i)
    {
        void* obj = s_objs[i];
        if (!IsBioSubtitles(obj) || IsDefaultArchetype(obj)) continue;

        char text[160] = {};
        const int32_t textLen = ReadSubtitleTextSEH(obj, text, sizeof(text));
        const uint8_t defaultMode = ReadU8AtSEH(obj, kSubtitleDefaultRenderMode);
        const uint8_t currentMode = ReadU8AtSEH(obj, kSubtitleCurrentRenderMode);
        float fontSize = 0.0f;
        uint32_t fontColor = 0;
        ReadFloatAtSEH(obj, kSubtitleFontSize, &fontSize);
        ReadU32AtSEH(obj, kSubtitleFontColor, &fontColor);
        const bool changed = std::strcmp(text, s_lastText[i]) != 0 ||
                             currentMode != s_lastMode[i];

        if (textLen > 1 && changed)
        {
            void* textData = nullptr;
            if (ReadPtrAtSEH(obj, kSubtitleFString, &textData) &&
                textData != nullptr &&
                CopyWideTextSEH(reinterpret_cast<const wchar_t*>(textData), textLen, g_activeSubtitleText,
                                static_cast<int32_t>(sizeof(g_activeSubtitleText) / sizeof(g_activeSubtitleText[0]))))
            {
                g_activeSubtitleObj.store(reinterpret_cast<std::uintptr_t>(obj), std::memory_order_release);
                g_activeSubtitleTextLen.store((std::min)(textLen, static_cast<int32_t>(sizeof(g_activeSubtitleText) / sizeof(g_activeSubtitleText[0]))), std::memory_order_release);
            }
            const uint64_t frame = g_subtitleTraceFrame.load(std::memory_order_acquire);
            g_nativeSubtitleActiveUntilFrame.store(frame + 360, std::memory_order_release);
            const uint64_t n = g_subtitleProbeLogs.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 400)
            {
                char colorHex[16] = {};
                snprintf(colorHex, sizeof(colorHex), "%08X", fontColor);
                LogLine("[SUBPROBE] inst#" + std::to_string(i + 1) + "/" + std::to_string(s_count) +
                        " obj=" + PtrText(obj) +
                        " defaultMode=" + std::to_string(static_cast<int>(defaultMode)) +
                        " currentMode=" + std::to_string(static_cast<int>(currentMode)) +
                        " fontSize=" + std::to_string(fontSize) +
                        " fontColor=0x" + colorHex +
                        " textLen=" + std::to_string(textLen) +
                        " activeUntil=" + std::to_string(frame + 360) +
                        " text='" + text + "'");
            }
            ArmSubtitleRenderTrace(text);
        }

        strcpy_s(s_lastText[i], text);
        s_lastMode[i] = currentMode;
    }
}

// One-shot dump of the LIVE (Denuvo-decrypted) subtitle draw code to disk for offline disassembly. The on-disk
// exe .text is encrypted, so this is the only way to statically analyze the native draw chain (where the
// top-lane Y position is computed). Read-only memcpy of in-process code pages, SEH-guarded, runs once.
bool CopyBytesSEH(void* dst, const void* src, size_t n) noexcept
{
    __try
    {
        std::memcpy(dst, src, n);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void DumpNativeSubtitleCodeOnce() noexcept
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;

    struct Range { std::uintptr_t begin; std::uintptr_t end; };
    // Covers the captured call chain + all SUBN probe RVAs.
    constexpr Range kRanges[] = {
        { 0x30F000, 0x314000 },   // SUBN1 0x310F40 / 0x3113A9
        { 0x3C6000, 0x3CC000 },   // SUBN2 0x3CA090 / 0x3C7E6D / 0x3CAA05
        { 0x4C8000, 0x4CA000 },   // chain caller 0x4C8931
        { 0x9B8000, 0x9BF000 },   // draw cluster 0x9B8BD0/0x9B8DA0/0x9BC180/0x9BC327/0x9BDA70/0x9BDBBE
        { 0xF08000, 0xF0B000 },   // SUBN6 0xF0921C
    };
    static uint8_t s_buffer[0x7000];   // >= the largest range above

    HMODULE game = GetModuleHandleW(nullptr);
    if (game == nullptr || !MELEVR::Logger::EnsureDataFolderExists()) return;
    const auto base = reinterpret_cast<const uint8_t*>(game);
    const std::wstring folder = MELEVR::Logger::GetDataFolderPath();

    for (const Range& r : kRanges)
    {
        const size_t size = r.end - r.begin;
        if (size > sizeof(s_buffer)) continue;

        char name[64] = {};
        snprintf(name, sizeof(name), "subcode_%06zX_%06zX.bin",
                 static_cast<size_t>(r.begin), static_cast<size_t>(r.end));

        if (!CopyBytesSEH(s_buffer, base + r.begin, size))
        {
            LogLine(std::string("[SUBCODE] dump FAILED (unreadable) ") + name);
            continue;
        }

        const std::wstring path = folder + L"\\" + std::wstring(name, name + std::strlen(name));
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            LogLine(std::string("[SUBCODE] dump FAILED (file open) ") + name);
            continue;
        }
        DWORD written = 0;
        WriteFile(file, s_buffer, static_cast<DWORD>(size), &written, nullptr);
        CloseHandle(file);
        LogLine(std::string("[SUBCODE] dumped ") + name + " bytes=" + std::to_string(written));
    }
}

// One-shot dump of every loaded GFxMovieInfo's RawData (the in-memory decompressed-package SWF bytes) so the
// PCHUD / conversation movie CHILD ELEMENT NAMES can be extracted offline (strings in the swf). This is the
// data source for per-element HUD controls (health/map/weapon vs top name bar). Fires once, only after the
// PCHUD handler has been seen (guarantees the HUD movie is loaded). Read-only SEH-guarded scan + copy.
void DumpLoadedSwfMoviesOnce() noexcept
{
    static bool s_done = false;
    if (s_done) return;
    if (g_pchudHandler.load(std::memory_order_acquire) == 0) return;   // wait until the HUD movie exists
    s_done = true;

    ObjectArrayHeader* objects = ObjectsSEH();
    if (objects == nullptr ||
        !Readable(objects->data, sizeof(void*) * static_cast<size_t>(objects->count)) ||
        !MELEVR::Logger::EnsureDataFolderExists())
    {
        s_done = false;   // table not ready; retry next frame
        return;
    }

    const std::wstring folder = MELEVR::Logger::GetDataFolderPath();
    constexpr std::uintptr_t kRawDataPtr   = 0x60;
    constexpr std::uintptr_t kRawDataCount = 0x68;
    constexpr int32_t kMaxSwfBytes = 32 * 1024 * 1024;
    static uint8_t s_chunk[0x10000];
    int dumped = 0;

    for (int32_t index = 0; index < objects->count; ++index)
    {
        void* obj = nullptr;
        if (!ReadObjectSlotSEH(objects->data, index, &obj)) break;
        if (obj == nullptr) continue;
        const char* cls = ClassNameSEH(obj);
        if (cls == nullptr || std::strcmp(cls, "GFxMovieInfo") != 0) continue;

        void* data = nullptr;
        int32_t count = 0;
        if (!ReadPtrAtSEH(obj, kRawDataPtr, &data)) continue;
        ReadU32AtSEH(obj, kRawDataCount, reinterpret_cast<uint32_t*>(&count));
        if (data == nullptr || count <= 8 || count > kMaxSwfBytes) continue;

        const char* name = ObjectNameSEH(obj);
        const char* outer = OuterNameSEH(obj);
        char fileName[160] = {};
        snprintf(fileName, sizeof(fileName), "swf_%s_%s_%d.bin",
                 (outer != nullptr && outer[0] != '\0') ? outer : "x",
                 (name != nullptr && name[0] != '\0') ? name : "x",
                 index);
        for (char* p = fileName; *p != '\0'; ++p)
            if (*p == ':' || *p == '/' || *p == '\\' || *p == ' ') *p = '-';

        // CREATE_NEW: a file dumped by an earlier session is kept, not rewritten (the movies are static
        // assets; re-dumping 143 files every boot was pure disk churn).
        const std::wstring path = folder + L"\\" + std::wstring(fileName, fileName + std::strlen(fileName));
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) continue;

        int32_t written = 0;
        bool ok = true;
        while (written < count && ok)
        {
            const int32_t n = (std::min)(count - written, static_cast<int32_t>(sizeof(s_chunk)));
            ok = CopyBytesSEH(s_chunk, static_cast<const uint8_t*>(data) + written, static_cast<size_t>(n));
            if (ok)
            {
                DWORD wrote = 0;
                WriteFile(file, s_chunk, static_cast<DWORD>(n), &wrote, nullptr);
                ok = wrote == static_cast<DWORD>(n);
            }
            written += n;
        }
        CloseHandle(file);
        ++dumped;
        LogLine(std::string("[SWFDUMP] ") + fileName + " bytes=" + std::to_string(count) +
                " ok=" + std::to_string(ok ? 1 : 0));
    }
    LogLine("[SWFDUMP] complete, movies dumped=" + std::to_string(dumped));
}

// Experimental native SIZE control: write UBioSubtitles.m_FontSize on the live instances each frame while the
// slider is non-zero. Pure field write on a freshly validated object (same crash-safety pattern as
// TickSubtitles: SEH reads/writes, class re-validated per frame, rescan-on-miss throttled, no method calls).
void TickNativeSubtitleFontSize() noexcept
{
    const float target = g_nativeSubFontSize.load(std::memory_order_acquire);
    if (target <= 0.0f) return;

    constexpr int kMaxSubs = 8;
    static void* s_objs[kMaxSubs] = {};
    static int   s_count = 0;
    static uint32_t s_rescanTick = 0;
    static uint64_t s_writeLogs = 0;

    bool valid = s_count > 0;
    for (int i = 0; i < s_count && valid; ++i)
        if (!IsBioSubtitles(s_objs[i]) || IsDefaultArchetype(s_objs[i])) valid = false;

    if (!valid)
    {
        s_count = 0;   // never write through a stale pointer set (FPQUAR lesson)
        if ((s_rescanTick++ % 60) == 0) s_count = FindAllBioSubtitles(s_objs, kMaxSubs);
        if (s_count == 0) return;
    }

    for (int i = 0; i < s_count; ++i)
    {
        void* obj = s_objs[i];
        if (!IsBioSubtitles(obj) || IsDefaultArchetype(obj)) continue;

        float before = 0.0f;
        if (!ReadFloatAtSEH(obj, kSubtitleFontSize, &before)) continue;
        if (std::fabs(before - target) < 0.01f) continue;   // already at target; skip the write + log

        const bool ok = WriteFloatAtSEH(obj, kSubtitleFontSize, target);
        if (s_writeLogs < 48)
        {
            ++s_writeLogs;
            char text[64] = {};
            const int32_t textLen = ReadSubtitleTextSEH(obj, text, sizeof(text));
            LogLine("[SUBFONT] inst#" + std::to_string(i + 1) + "/" + std::to_string(s_count) +
                    " obj=" + PtrText(obj) +
                    " before=" + std::to_string(before) +
                    " wrote=" + std::to_string(target) +
                    " ok=" + std::to_string(ok ? 1 : 0) +
                    " textLen=" + std::to_string(textLen) +
                    " text='" + text + "'");
        }
    }
}

// Resolve the two UFunctions the redraw needs: Canvas.DrawText (to draw) and BioHUD.PostRender (to identify the
// event in the hook). Full table scan, so only called from Tick() until both resolve. Cheap once cached.
bool ResolveCanvasFns() noexcept
{
    if (g_canvasDrawTextFn != nullptr && g_postRenderFn != nullptr && g_getSubtitleRegionFn != nullptr &&
        g_bioSubtitlesDisplayFn != nullptr && g_bioSubtitlesUpdateFn != nullptr) return true;
    if (g_canvasDrawTextFn == nullptr) g_canvasDrawTextFn = FindFunctionByOuterName("Canvas", "DrawText");
    if (g_postRenderFn == nullptr)     g_postRenderFn     = FindFunctionByOuterName("BioHUD", "PostRender");
    if (g_getSubtitleRegionFn == nullptr) g_getSubtitleRegionFn = FindFunctionByOuterName("GameViewportClient", "GetSubtitleRegion");
    if (g_bioSubtitlesDisplayFn == nullptr) g_bioSubtitlesDisplayFn = FindFunctionByOuterName("BioSubtitles", "DisplaySubtitle");
    if (g_bioSubtitlesUpdateFn == nullptr)  g_bioSubtitlesUpdateFn  = FindFunctionByOuterName("BioSubtitles", "UpdateSubtitles");
    if (g_canvasDrawTextFn != nullptr && g_postRenderFn != nullptr && g_getSubtitleRegionFn != nullptr &&
        g_bioSubtitlesDisplayFn != nullptr && g_bioSubtitlesUpdateFn != nullptr)
    {
        LogLine("[SUBDRAW] resolved Canvas.DrawText=" + PtrText(g_canvasDrawTextFn) +
                " BioHUD.PostRender=" + PtrText(g_postRenderFn) +
                " GameViewportClient.GetSubtitleRegion=" + PtrText(g_getSubtitleRegionFn) +
                " BioSubtitles.DisplaySubtitle=" + PtrText(g_bioSubtitlesDisplayFn) +
                " BioSubtitles.UpdateSubtitles=" + PtrText(g_bioSubtitlesUpdateFn));
        return true;
    }
    return false;
}

// Draw text on the HUD Canvas via the engine's Canvas.DrawText, invoked through the original ProcessEvent (the
// same call path PCHUD uses for SetVariableFloat). The reentrancy guard makes the hook ignore this call.
bool CallCanvasDrawText(void* canvas, const wchar_t* text, int32_t count, float sx, float sy) noexcept
{
    ProcessEventFn original = g_originalProcessEvent;
    if (original == nullptr || canvas == nullptr || text == nullptr || count <= 1 ||
        g_applyingPchud || g_canvasDrawTextFn == nullptr)
    {
        return false;
    }

    DrawTextParams params = {};
    params.text.data = text;
    params.text.count = count;
    params.text.capacity = count;
    params.cr = 1;
    params.xscale = sx;
    params.yscale = sy;

    bool ok = false;
    g_applyingPchud = true;
    __try
    {
        original(canvas, g_canvasDrawTextFn, &params, nullptr);
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    g_applyingPchud = false;
    return ok;
}

// Redraw the game's live subtitle on the HUD Canvas at the configured position + scale. Called from the BioHUD.PostRender
// hook (Canvas valid there). hud = the ABioHUD context. Reads the subtitle straight off the world so there is
// no cross-thread cache. Pure read + one DrawText; SEH-guarded throughout.
// Font PICKER (2026-07-06): the Coalesced says SubtitleFontName=EngineFonts.SmallFont, but the live test
// proved the native subtitle does NOT look like SmallFont - the real font is some other loaded Font asset.
// Rather than guess, enumerate every live Font/MultiFont object and let the user cycle until it matches.
// The chosen font's name goes to the menu + log so the winner can be hardcoded afterwards.
constexpr int kMaxFonts = 96;
void* g_fontObjs[kMaxFonts] = {};                  // game-thread written, menu reads names only
char  g_fontNames[kMaxFonts][80] = {};
std::atomic_int g_fontCount{0};
std::atomic_int g_subtitleFontIndex{-2};           // -2 = matched MediumFont; -1 = canvas default; 0..N-1 = picker
std::atomic_uint32_t g_fontScanTick{0};

// Engine.Canvas.TextSize: measures a string with the canvas's CURRENT font at scale 1 (out XL/YL).
// Parms: { FString String; float XL; float YL } - the same DrawText-family parm shape that has run safely
// for weeks. Used for true centering + word wrap.
void* g_canvasTextSizeFn = nullptr;

struct TextSizeParams
{
    FStringParam text;   // 0x00
    float xl = 0.0f;     // 0x10 (out)
    float yl = 0.0f;     // 0x14 (out)
};

bool ResolveCanvasTextSize() noexcept
{
    if (g_canvasTextSizeFn != nullptr) return true;
    g_canvasTextSizeFn = FindFunctionByOuterName("Canvas", "TextSize");
    if (g_canvasTextSizeFn != nullptr)
    {
        LogLine("[SUBDRAW] resolved Canvas.TextSize=" + PtrText(g_canvasTextSizeFn));
    }
    return g_canvasTextSizeFn != nullptr;
}

bool CallCanvasTextSize(void* canvas, const wchar_t* text, int32_t count, float* xl, float* yl) noexcept
{
    ProcessEventFn original = g_originalProcessEvent;
    if (original == nullptr || canvas == nullptr || text == nullptr || count <= 1 ||
        xl == nullptr || yl == nullptr || g_applyingPchud || !ResolveCanvasTextSize())
    {
        return false;
    }

    TextSizeParams params = {};
    params.text.data = text;
    params.text.count = count;
    params.text.capacity = count;

    bool ok = false;
    g_applyingPchud = true;
    __try
    {
        original(canvas, g_canvasTextSizeFn, &params, nullptr);
        ok = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    g_applyingPchud = false;
    if (ok)
    {
        *xl = params.xl;
        *yl = params.yl;
    }
    return ok;
}

void RescanFontObjects() noexcept
{
    ObjectArrayHeader* objects = ObjectsSEH();
    if (objects == nullptr || !Readable(objects->data, sizeof(void*) * static_cast<size_t>(objects->count)))
    {
        return;
    }
    int n = 0;
    for (int32_t index = 0; index < objects->count && n < kMaxFonts; ++index)
    {
        void* obj = nullptr;
        if (!ReadObjectSlotSEH(objects->data, index, &obj)) break;
        if (!PointerLooksCanonicalAligned(obj)) continue;
        const char* cls = ClassNameSEH(obj);
        if (cls == nullptr || (std::strcmp(cls, "Font") != 0 && std::strcmp(cls, "MultiFont") != 0)) continue;
        const char* name = ObjectNameSEH(obj);
        const char* outer = OuterNameSEH(obj);
        if (name == nullptr || name[0] == '\0') continue;
        if (std::strstr(name, "Default__") != nullptr) continue;   // skip archetypes
        g_fontObjs[n] = obj;
        snprintf(g_fontNames[n], sizeof(g_fontNames[n]), "%s.%s (%s)",
                 (outer != nullptr && outer[0] != '\0') ? outer : "?", name, cls);
        ++n;
    }
    const int prev = g_fontCount.exchange(n, std::memory_order_acq_rel);
    if (n != prev)
    {
        LogLine("[SUBFONTPICK] enumerated " + std::to_string(n) + " font objects:");
        for (int i = 0; i < n; ++i)
            LogLine("  [SUBFONTPICK] #" + std::to_string(i) + " " + g_fontNames[i] +
                    " ptr=" + PtrText(g_fontObjs[i]));
    }
}

void* CachedSubtitleFont() noexcept
{
    if ((g_fontScanTick.fetch_add(1, std::memory_order_relaxed) % 300) == 0) RescanFontObjects();
    const int idx = g_subtitleFontIndex.load(std::memory_order_acquire);
    const int count = g_fontCount.load(std::memory_order_acquire);

    // -2 (the default) = the MATCHED native subtitle font: EngineFonts.MediumFont, identified by
    // Eye-match 2026-07-06 (picker slot #2). Resolved by NAME so table order can't break it.
    if (idx == -2)
    {
        for (int i = 0; i < count; ++i)
        {
            if (std::strstr(g_fontNames[i], "EngineFonts.MediumFont") != nullptr)
            {
                void* font = g_fontObjs[i];
                const char* cls = ClassNameSEH(font);
                if (cls != nullptr && std::strstr(cls, "Font") != nullptr) return font;
            }
        }
        return nullptr;
    }

    if (idx < 0 || idx >= count) return nullptr;
    void* font = g_fontObjs[idx];
    const char* cls = ClassNameSEH(font);
    if (cls == nullptr || std::strstr(cls, "Font") == nullptr)
    {
        RescanFontObjects();   // table shifted (level transition); refresh before next use
        return nullptr;
    }
    return font;
}

void RedrawSubtitleOnCanvas(void* hud) noexcept
{
    if (!g_subtitleRedraw.load(std::memory_order_acquire) || g_canvasDrawTextFn == nullptr) return;
    if (!PointerLooksCanonicalAligned(hud) || !Readable(hud, 0x570)) return;

    // STAND DOWN during conversations: the conversation UI draws its own subtitle. The overlay handles only
    // gameplay barks. (Reliable convo detection lives in the ProcessEvent hook above.)
    const uint64_t frame = g_subtitleTraceFrame.load(std::memory_order_acquire);
    if (frame < g_convoActiveUntilFrame.load(std::memory_order_acquire))
    {
        const uint64_t s = g_subRedrawSkipLogs.fetch_add(1, std::memory_order_relaxed) + 1;
        if (s <= 8) LogLine("[SUBDRAW] skip (convo active) frame=" + std::to_string(frame) +
                            " until=" + std::to_string(g_convoActiveUntilFrame.load(std::memory_order_acquire)));
        return;
    }

    void* canvas = nullptr;
    void* world = nullptr;
    ReadPtrAtSEH(hud, kHudCanvas, &canvas);
    ReadPtrAtSEH(hud, kActorWorldInfo, &world);
    if (!PointerLooksCanonicalAligned(canvas) || !Readable(canvas, 0xB8)) return;
    if (!PointerLooksCanonicalAligned(world)  || !Readable(world, kWorldSubtitles + 8)) return;

    void* subs = nullptr;
    ReadPtrAtSEH(world, kWorldSubtitles, &subs);
    if (!PointerLooksCanonicalAligned(subs) || !Readable(subs, 0x70)) return;

    void* textData = nullptr;
    ReadPtrAtSEH(subs, kSubtitleFString, &textData);
    const int32_t count = ReadI32AtSEH(subs, kSubtitleFString + 8);
    if (textData == nullptr || count <= 1 || !Readable(textData, 2)) return;

    int32_t sizeX = ReadI32AtSEH(canvas, kCanvasSizeX);
    int32_t sizeY = ReadI32AtSEH(canvas, kCanvasSizeY);
    if (sizeX <= 0 || sizeX > 16384) sizeX = 1920;
    if (sizeY <= 0 || sizeY > 16384) sizeY = 1080;

    const float centerX = g_subPosXFrac.load(std::memory_order_relaxed) * static_cast<float>(sizeX);
    const float yBase = g_subPosYFrac.load(std::memory_order_relaxed) * static_cast<float>(sizeY);
    const float sx = g_subScaleX.load(std::memory_order_relaxed);
    const float sy = g_subScaleY.load(std::memory_order_relaxed);

    // Local copy (the live FString could be swapped mid-frame by the game; lines also need splitting).
    wchar_t buf[512] = {};
    const int32_t chars = (std::min)(count - 1, static_cast<int32_t>((sizeof(buf) / sizeof(buf[0])) - 1));
    if (!CopyBytesSEH(buf, textData, static_cast<size_t>(chars) * sizeof(wchar_t))) return;
    buf[chars] = L'\0';

    // NATIVE LOOK: swap in the matched subtitle font (EngineFonts.MediumFont) for measure + draw, restore
    // after. Shadow pass + the game's own subtitle color (0xFFCCFFFF, from the live m_FontColor probes).
    constexpr std::uintptr_t kCanvasFont = 0x80;
    void* font = CachedSubtitleFont();
    void* oldFont = nullptr;
    bool fontSwapped = false;
    if (font != nullptr && ReadPtrAtSEH(canvas, kCanvasFont, &oldFont))
    {
        fontSwapped = WritePtrAtSEH(canvas, kCanvasFont, font);
    }

    // CENTERED + WRAPPED: measure once with the swapped font (scale 1), wrap into up to 4 proportional
    // space-boundary lines when wider than 85% of the screen, center each line on centerX.
    float xlFull = 0.0f, ylFull = 0.0f;
    const bool measured = CallCanvasTextSize(canvas, buf, chars + 1, &xlFull, &ylFull);
    const float fullWidth = measured ? xlFull * sx : 0.0f;
    const float lineH = (measured && ylFull > 1.0f ? ylFull : 16.0f) * sy * 1.08f;
    const float maxW = 0.85f * static_cast<float>(sizeX);

    int numLines = 1;
    if (measured && fullWidth > maxW)
        numLines = (std::min)(4, static_cast<int>(fullWidth / maxW) + 1);

    // Split at space boundaries into ~equal chunks.
    struct Line { int start; int len; };
    Line lines[4] = {};
    int lineCount = 0;
    {
        int pos = 0;
        for (int li = 0; li < numLines && pos < chars; ++li)
        {
            int end = (li == numLines - 1) ? chars : pos + (chars - pos) / (numLines - li);
            if (end < chars)
            {
                int probe = end;
                while (probe > pos && buf[probe] != L' ') --probe;   // pull back to a space
                if (probe > pos) end = probe;
                else { while (end < chars && buf[end] != L' ') ++end; }   // none behind: push forward
            }
            lines[lineCount].start = pos;
            lines[lineCount].len = end - pos;
            ++lineCount;
            pos = end;
            while (pos < chars && buf[pos] == L' ') ++pos;   // eat the separator
        }
    }

    const float shadow = (std::max)(1.0f, 1.5f * sx);
    bool ok = true;
    for (int li = 0; li < lineCount; ++li)
    {
        wchar_t lineBuf[512] = {};
        const int len = (std::min)(lines[li].len, 511);
        if (len <= 0) continue;
        std::memcpy(lineBuf, buf + lines[li].start, static_cast<size_t>(len) * sizeof(wchar_t));
        lineBuf[len] = L'\0';

        // proportional width estimate keeps this at ONE TextSize call per frame
        const float lineWidth = measured ? fullWidth * (static_cast<float>(len) / static_cast<float>(chars))
                                         : 0.0f;
        const float x = measured ? centerX - lineWidth * 0.5f : centerX;
        const float y = yBase + static_cast<float>(li) * lineH;

        WriteFloatAtSEH(canvas, kCanvasCurX, x + shadow);
        WriteFloatAtSEH(canvas, kCanvasCurY, y + shadow);
        WriteU32AtSEH(canvas, kCanvasDrawColor, 0xFF000000u);
        CallCanvasDrawText(canvas, lineBuf, len + 1, sx, sy);

        WriteFloatAtSEH(canvas, kCanvasCurX, x);
        WriteFloatAtSEH(canvas, kCanvasCurY, y);
        WriteU32AtSEH(canvas, kCanvasDrawColor, 0xFFCCFFFFu);
        ok = CallCanvasDrawText(canvas, lineBuf, len + 1, sx, sy) && ok;
    }

    if (fontSwapped) WritePtrAtSEH(canvas, kCanvasFont, oldFont);

    const uint64_t n = g_subRedrawLogs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 10 || !ok)
        LogLine("[SUBDRAW] n=" + std::to_string(n) + " ok=" + std::to_string(ok ? 1 : 0) +
                " measured=" + std::to_string(measured ? 1 : 0) +
                " fullWidth=" + std::to_string(fullWidth) + " lines=" + std::to_string(lineCount) +
                " center=" + std::to_string(centerX) + " y=" + std::to_string(yBase) +
                " font=" + PtrText(font) + " chars=" + std::to_string(chars));
}

// PostRender handler: optionally BLANK the game's subtitle before the native HUD draws it (so only the redraw shows),
// let the game render, restore the text, then draw the redraw on top. All on the game thread, transient write+restore.
void RunPostRenderWithRedraw(void* context, void* function, void* params, void* result, ProcessEventFn original) noexcept
{
    // There are MULTIPLE UBioSubtitles instances; the one drawn at the top isn't necessarily the world's. So
    // blank the text length (@0x68) on ALL live instances before the native HUD draws, then restore after.
    static void* s_subs[8] = {};   // game-thread only (this is the sole caller)
    static int   s_n = 0;
    static uint32_t s_scan = 0;

    const bool hideOrig = g_subtitleHideOriginal.load(std::memory_order_acquire);

    if (hideOrig)
    {
        bool valid = s_n > 0;
        for (int i = 0; i < s_n && valid; ++i)
            if (!IsBioSubtitles(s_subs[i]) || IsDefaultArchetype(s_subs[i])) valid = false;
        if (!valid && (s_scan++ % 120) == 0) s_n = FindAllBioSubtitles(s_subs, 8);
    }

    int32_t saved[8] = {};
    bool    hid[8] = {};
    int     hidN = 0;
    if (hideOrig)
    {
        for (int i = 0; i < s_n; ++i)
        {
            const int32_t c = ReadI32AtSEH(s_subs[i], kSubtitleFString + 8);
            if (c > 1 && WriteU32AtSEH(s_subs[i], kSubtitleFString + 8, 0)) { saved[i] = c; hid[i] = true; ++hidN; }
        }
    }

    original(context, function, params, result);   // game renders the HUD (all subtitle instances blanked)

    for (int i = 0; i < s_n; ++i)
        if (hid[i]) WriteU32AtSEH(s_subs[i], kSubtitleFString + 8, static_cast<uint32_t>(saved[i]));  // restore

    RedrawSubtitleOnCanvas(context);               // draw the subtitle on top, at the configured pos/scale

    static uint64_t s_log = 0;
    if (s_log++ < 12)
        LogLine("[SUBHIDE] instances=" + std::to_string(s_n) + " blanked=" + std::to_string(hidN));
}

void* FindProcessEventTarget() noexcept
{
    ObjectArrayHeader* objects = ObjectsSEH();
    if (objects == nullptr || !Readable(objects->data, sizeof(void*) * static_cast<size_t>(objects->count)))
    {
        return nullptr;
    }

    const int32_t limit = (std::min)(objects->count, kProcessEventTargetScanLimit);
    for (int32_t index = 0; index < limit; ++index)
    {
        void* obj = nullptr;
        if (!ReadObjectSlotSEH(objects->data, index, &obj)) return nullptr;
        if (!PointerLooksCanonicalAligned(obj) || !Readable(obj, sizeof(void*))) continue;

        void* vtable = nullptr;
        void* target = nullptr;
        if (ReadPtrAtSEH(obj, 0, &vtable) &&
            PointerLooksCanonicalAligned(vtable) &&
            ReadPtrAtSEH(vtable, kProcessEventVtableOffset, &target) &&
            IsExecutableAddress(target))
        {
            LogLine("[PCHUD] ProcessEvent target=" + PtrText(target) +
                    " obj=" + PtrText(obj) +
                    " class=" + SafeAnsi(ClassNameSEH(obj)));
            return target;
        }
    }

    return nullptr;
}

bool InstallNow() noexcept
{
    if (g_installed.load(std::memory_order_acquire)) return true;

    void* target = FindProcessEventTarget();
    if (target == nullptr)
    {
        // Object table not ready yet (early boot). Quiet failure; Tick() retries next frame.
        return false;
    }

    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
    {
        LogLine("[PCHUD] MH_Initialize failed status=" + std::to_string(static_cast<int>(init)));
        return false;
    }

    const MH_STATUS create = MH_CreateHook(target,
                                           reinterpret_cast<void*>(&ProcessEventHook),
                                           reinterpret_cast<void**>(&g_originalProcessEvent));
    if (create != MH_OK && create != MH_ERROR_ALREADY_CREATED)
    {
        LogLine("[PCHUD] MH_CreateHook ProcessEvent failed status=" +
                std::to_string(static_cast<int>(create)));
        return false;
    }

    const MH_STATUS enable = MH_EnableHook(target);
    if (enable != MH_OK && enable != MH_ERROR_ENABLED)
    {
        LogLine("[PCHUD] MH_EnableHook ProcessEvent failed status=" +
                std::to_string(static_cast<int>(enable)));
        return false;
    }

    g_processEventTarget = target;
    // InstallNativeSubtitleProbes();   // SUBTITLE PROBES DISABLED FOR TEST 2026-07-07 (see Tick block note)
    g_installed.store(true, std::memory_order_release);
    ResolveSetVariableFloat();
    LogLine("[PCHUD] installed PCHUD movie transform hook.");
    return true;
}
}  // namespace

namespace MELEVR::Pchud
{
void Install() noexcept
{
    InstallNow();
}

void SetTransform(bool enabled, float scaleX, float scaleY, float offsetX, float offsetY) noexcept
{
    if (!std::isfinite(scaleX)) scaleX = 0.82f;
    if (!std::isfinite(scaleY)) scaleY = 0.82f;
    if (!std::isfinite(offsetX)) offsetX = 0.0f;
    if (!std::isfinite(offsetY)) offsetY = 0.0f;

    const bool prevEnabled = g_enabled.load(std::memory_order_acquire);
    const bool changed = (prevEnabled != enabled) ||
                         (g_scaleX.load(std::memory_order_relaxed) != scaleX) ||
                         (g_scaleY.load(std::memory_order_relaxed) != scaleY) ||
                         (g_offsetX.load(std::memory_order_relaxed) != offsetX) ||
                         (g_offsetY.load(std::memory_order_relaxed) != offsetY);

    g_scaleX.store(scaleX, std::memory_order_relaxed);
    g_scaleY.store(scaleY, std::memory_order_relaxed);
    g_offsetX.store(offsetX, std::memory_order_relaxed);
    g_offsetY.store(offsetY, std::memory_order_relaxed);
    g_enabled.store(enabled, std::memory_order_release);

    if (changed) g_settingsRev.fetch_add(1, std::memory_order_acq_rel);
}

void SetConvoTransform(bool enabled, float scaleX, float scaleY, float offsetX, float offsetY) noexcept
{
    if (!std::isfinite(scaleX)) scaleX = 1.0f;
    if (!std::isfinite(scaleY)) scaleY = 1.0f;
    if (!std::isfinite(offsetX)) offsetX = 0.0f;
    if (!std::isfinite(offsetY)) offsetY = 0.0f;

    const bool prevEnabled = g_convoEnabled.load(std::memory_order_acquire);
    const bool changed = (prevEnabled != enabled) ||
                         (g_convoScaleX.load(std::memory_order_relaxed) != scaleX) ||
                         (g_convoScaleY.load(std::memory_order_relaxed) != scaleY) ||
                         (g_convoOffsetX.load(std::memory_order_relaxed) != offsetX) ||
                         (g_convoOffsetY.load(std::memory_order_relaxed) != offsetY);

    g_convoScaleX.store(scaleX, std::memory_order_relaxed);
    g_convoScaleY.store(scaleY, std::memory_order_relaxed);
    g_convoOffsetX.store(offsetX, std::memory_order_relaxed);
    g_convoOffsetY.store(offsetY, std::memory_order_relaxed);
    g_convoEnabled.store(enabled, std::memory_order_release);

    if (changed) g_convoSettingsRev.fetch_add(1, std::memory_order_acq_rel);
}

// [LASERUI] mining-laser layout transform set (applies only while the laser layout is latched; see
// ApplyDesignerUiToLivePanel). Bumps the shared designer settings rev so live edits reapply promptly.
void SetLaserUiTransform(bool enabled, float scaleX, float scaleY, float offsetX, float offsetY) noexcept
{
    if (!std::isfinite(scaleX)) scaleX = 1.0f;
    if (!std::isfinite(scaleY)) scaleY = 1.0f;
    if (!std::isfinite(offsetX)) offsetX = 0.0f;
    if (!std::isfinite(offsetY)) offsetY = 0.0f;
    const bool changed = (g_laserUiEnabled.load(std::memory_order_acquire) != enabled) ||
                         (g_laserUiScaleX.load(std::memory_order_relaxed) != scaleX) ||
                         (g_laserUiScaleY.load(std::memory_order_relaxed) != scaleY) ||
                         (g_laserUiOffsetX.load(std::memory_order_relaxed) != offsetX) ||
                         (g_laserUiOffsetY.load(std::memory_order_relaxed) != offsetY);
    g_laserUiScaleX.store(scaleX, std::memory_order_relaxed);
    g_laserUiScaleY.store(scaleY, std::memory_order_relaxed);
    g_laserUiOffsetX.store(offsetX, std::memory_order_relaxed);
    g_laserUiOffsetY.store(offsetY, std::memory_order_relaxed);
    g_laserUiEnabled.store(enabled, std::memory_order_release);
    if (changed) g_designerUiSettingsRev.fetch_add(1, std::memory_order_acq_rel);
}

void SetDesignerUiTransform(bool enabled, float scaleX, float scaleY, float offsetX, float offsetY) noexcept
{
    if (!std::isfinite(scaleX)) scaleX = 1.0f;
    if (!std::isfinite(scaleY)) scaleY = 1.0f;
    if (!std::isfinite(offsetX)) offsetX = 0.0f;
    if (!std::isfinite(offsetY)) offsetY = 0.0f;

    const bool prevEnabled = g_designerUiEnabled.load(std::memory_order_acquire);
    const bool changed = (prevEnabled != enabled) ||
                         (g_designerUiScaleX.load(std::memory_order_relaxed) != scaleX) ||
                         (g_designerUiScaleY.load(std::memory_order_relaxed) != scaleY) ||
                         (g_designerUiOffsetX.load(std::memory_order_relaxed) != offsetX) ||
                         (g_designerUiOffsetY.load(std::memory_order_relaxed) != offsetY);

    g_designerUiScaleX.store(scaleX, std::memory_order_relaxed);
    g_designerUiScaleY.store(scaleY, std::memory_order_relaxed);
    g_designerUiOffsetX.store(offsetX, std::memory_order_relaxed);
    g_designerUiOffsetY.store(offsetY, std::memory_order_relaxed);
    g_designerUiEnabled.store(enabled, std::memory_order_release);

    if (changed)
    {
        g_designerUiSettingsRev.fetch_add(1, std::memory_order_acq_rel);
        // DIAGNOSTIC (2026-07-12): the config's designerUiEnabled reads 0 from disk (verified) but the panel
        // still applies at runtime - log every actual enabled-value TRANSITION this function receives, so it's clear
        // can see directly whether the caller ever passes false, or whether it's true from the very first call.
        LogLine("[DESIGNERUI_DIAG] enabled transition " + std::to_string(prevEnabled ? 1 : 0) +
                " -> " + std::to_string(enabled ? 1 : 0));
    }
}

void SetSubtitleMode(bool force, int mode) noexcept
{
    g_subtitleMode.store(mode, std::memory_order_relaxed);
    g_subtitleForce.store(force, std::memory_order_release);
}

void SetSubtitleFontIndex(int index) noexcept
{
    g_subtitleFontIndex.store(index, std::memory_order_release);
}

int GetSubtitleFontCount() noexcept
{
    return g_fontCount.load(std::memory_order_acquire);
}

const char* GetSubtitleFontName(int index) noexcept
{
    const int count = g_fontCount.load(std::memory_order_acquire);
    if (index < 0 || index >= count) return "";
    return g_fontNames[index];
}

// Narrow name tables (extern in pchud.h; used by menu + config for labels/keys).
const char* const kHudElemNames[kHudElemCount] = {
    "BottomUI", "radarMC", "targetMC", "topLeft", "topRight", "leftUI", "rightUI",
    "squadMC", "weaponAbilityTab", "EventHolder", "TeamBG", "EquipBG", "vehiclePause",
};
const char* const kConvoElemNames[kConvoElemCount] = {
    "SubtitleConversation", "SubtitleTop", "SubtitleBottom", "ConversationWheel",
};

void SetHudElements(const HudElemState* elems, int count) noexcept
{
    if (elems == nullptr || count <= 0) return;
    const int n = (std::min)(count, kHudElemCount);
    const int cur = g_hudElemActive.load(std::memory_order_acquire);
    const int next = cur ^ 1;
    bool changed = false;
    for (int i = 0; i < kHudElemCount; ++i)
    {
        const HudElemState v = (i < n) ? elems[i] : HudElemState{};
        g_hudElemBuf[next][i] = v;
        const HudElemState& o = g_hudElemBuf[cur][i];
        if (v.on != o.on || v.hide != o.hide || v.offX != o.offX || v.offY != o.offY ||
            v.scaleX != o.scaleX || v.scaleY != o.scaleY)
        {
            changed = true;
        }
    }
    if (changed)
    {
        g_hudElemActive.store(next, std::memory_order_release);
        g_hudElemRev.fetch_add(1, std::memory_order_acq_rel);
    }
}

void SetHudGroups(const HudElemState& top, const HudElemState& bottom) noexcept
{
    // Which group each kHudElemNames entry belongs to: 0=none (untouched), 1=top, 2=bottom.
    // RESTRICTED 2026-07-06 after the live glitch pass: groups only touch STATIC on-screen containers.
    // Everything the game animates or parks off-screen is permanently excluded - topLeft (authored
    // x=-265, parked offscreen), leftUI/rightUI (slide-in side panels), EventHolder (XP popups strobed),
    // EquipBG (equipment backdrop = the "big metal door"), topRight/weaponAbilityTab (transition-animated),
    // squadMC/vehiclePause (center overlays).
    static constexpr uint8_t kGroupOf[kHudElemCount] = {
        2,  // BottomUI          (health/weapon bar - static)
        2,  // radarMC           (radar - static)
        1,  // targetMC          (target name bar - static)
        0,  // topLeft           (parked offscreen, game tweens it in)
        0,  // topRight          (transition-animated)
        0,  // leftUI            (slide-in side panel)
        0,  // rightUI           (slide-in side panel, mirrored)
        0,  // squadMC           (center overlay)
        0,  // weaponAbilityTab  (transition-animated)
        0,  // EventHolder       (XP/event popups - game strobes visibility)
        2,  // TeamBG            (squad backdrop - static)
        0,  // EquipBG           (equipment backdrop, normally hidden)
        0,  // vehiclePause      (center overlay)
    };
    // Authored placements (must mirror kHudElemBase in the anonymous namespace; duplicated here because
    // that table is file-internal - keep the two in sync if elements are ever rebucketed).
    static constexpr float kBaseX[kHudElemCount] = {
        434.80f, 1089.00f, 400.55f, -264.90f, 929.80f, -169.05f, 1565.85f,
        635.50f, 923.45f, 1175.00f, 477.25f, 64.85f, 640.00f,
    };
    static constexpr float kBaseY[kHudElemCount] = {
        601.95f, 556.60f, 36.05f, 0.30f, 144.80f, 100.00f, 100.00f,
        355.55f, 101.50f, 527.00f, 526.30f, 685.40f, 183.30f,
    };
    // Group anchors in movie coordinates (1280x720 nominal stage).
    constexpr float kTopAnchorX = 640.0f,  kTopAnchorY = 0.0f;
    constexpr float kBotAnchorX = 640.0f,  kBotAnchorY = 720.0f;

    HudElemState elems[kHudElemCount] = {};
    for (int i = 0; i < kHudElemCount; ++i)
    {
        const uint8_t grp = kGroupOf[i];
        if (grp == 0) continue;   // stays default-constructed = untouched
        const HudElemState& g = (grp == 1) ? top : bottom;
        const float ax = (grp == 1) ? kTopAnchorX : kBotAnchorX;
        const float ay = (grp == 1) ? kTopAnchorY : kBotAnchorY;

        HudElemState& e = elems[i];
        e.on = g.on;
        e.hide = g.hide;
        e.scaleX = g.scaleX;
        e.scaleY = g.scaleY;
        // The group scales AS A UNIT about its anchor: the member's new position is
        // anchor + (authored - anchor) * scale + groupOffset. ApplyElementsToPanel writes
        // base + off, so store the difference as this member's offset.
        e.offX = (ax + (kBaseX[i] - ax) * g.scaleX + g.offX) - kBaseX[i];
        e.offY = (ay + (kBaseY[i] - ay) * g.scaleY + g.offY) - kBaseY[i];
    }
    SetHudElements(elems, kHudElemCount);
}

void SetConvoElements(const HudElemState* elems, int count) noexcept
{
    if (elems == nullptr || count <= 0) return;
    const int n = (std::min)(count, kConvoElemCount);
    const int cur = g_convoElemActive.load(std::memory_order_acquire);
    const int next = cur ^ 1;
    bool changed = false;
    for (int i = 0; i < kConvoElemCount; ++i)
    {
        const HudElemState v = (i < n) ? elems[i] : HudElemState{};
        g_convoElemBuf[next][i] = v;
        const HudElemState& o = g_convoElemBuf[cur][i];
        if (v.on != o.on || v.hide != o.hide || v.offX != o.offX || v.offY != o.offY ||
            v.scaleX != o.scaleX || v.scaleY != o.scaleY)
        {
            changed = true;
        }
    }
    if (changed)
    {
        g_convoElemActive.store(next, std::memory_order_release);
        g_convoElemRev.fetch_add(1, std::memory_order_acq_rel);
    }
}

void SetSubtitleRedraw(bool enabled, bool hideOriginal, float posXFrac, float posYFrac,
                       float scaleX, float scaleY) noexcept
{
    if (!std::isfinite(posXFrac)) posXFrac = 0.28f;
    if (!std::isfinite(posYFrac)) posYFrac = 0.82f;
    if (!std::isfinite(scaleX) || scaleX <= 0.05f) scaleX = 1.0f;
    if (!std::isfinite(scaleY) || scaleY <= 0.05f) scaleY = 1.0f;
    g_subPosXFrac.store(posXFrac, std::memory_order_relaxed);
    g_subPosYFrac.store(posYFrac, std::memory_order_relaxed);
    g_subScaleX.store(scaleX, std::memory_order_relaxed);
    g_subScaleY.store(scaleY, std::memory_order_relaxed);
    g_subtitleHideOriginal.store(hideOriginal, std::memory_order_release);
    g_subtitleRedraw.store(enabled, std::memory_order_release);
}

void SetNativeSubtitleMove(bool enabled, float regionLeftX, float regionTopY, float regionRightX,
                           float regionBottomY, float fontSize) noexcept
{
    static bool  s_prevEnabled = false;
    static float s_prevX = -1.0f;
    static float s_prevY = -1.0f;
    static float s_prevMaxX = -1.0f;
    static float s_prevMaxY = -1.0f;
    static float s_prevFontSize = -1.0f;
    static uint64_t s_cfgLogs = 0;

    if (!std::isfinite(regionLeftX))   regionLeftX = 0.0f;
    if (!std::isfinite(regionTopY))    regionTopY = 0.82f;
    if (!std::isfinite(regionRightX))  regionRightX = 1.0f;
    if (!std::isfinite(regionBottomY)) regionBottomY = 1.0f;
    if (!std::isfinite(fontSize) || fontSize < 0.0f) fontSize = 0.0f;

    regionLeftX = (std::max)(0.0f, (std::min)(0.95f, regionLeftX));
    regionTopY = (std::max)(0.0f, (std::min)(0.98f, regionTopY));
    regionRightX = (std::max)(regionLeftX + 0.05f, (std::min)(1.0f, regionRightX));
    regionBottomY = (std::max)(regionTopY + 0.02f, (std::min)(1.0f, regionBottomY));
    fontSize = (std::min)(128.0f, fontSize);

    g_nativeSubPosXFrac.store(regionLeftX, std::memory_order_relaxed);
    g_nativeSubPosYFrac.store(regionTopY, std::memory_order_relaxed);
    g_nativeSubScaleX.store(regionRightX, std::memory_order_relaxed);
    g_nativeSubScaleY.store(regionBottomY, std::memory_order_relaxed);
    g_nativeSubFontSize.store(fontSize, std::memory_order_release);
    g_nativeSubMove.store(enabled, std::memory_order_release);

    const bool changed = enabled != s_prevEnabled ||
                         std::fabs(regionLeftX - s_prevX) > 0.0005f ||
                         std::fabs(regionTopY - s_prevY) > 0.0005f ||
                         std::fabs(regionRightX - s_prevMaxX) > 0.0005f ||
                         std::fabs(regionBottomY - s_prevMaxY) > 0.0005f ||
                         std::fabs(fontSize - s_prevFontSize) > 0.0005f;
    if (changed) g_nativeSubSettingsRev.fetch_add(1, std::memory_order_acq_rel);
    if (changed && s_cfgLogs < 48)
    {
        ++s_cfgLogs;
        LogLine("[SUBMOVE_CFG] enabled=" + std::to_string(enabled ? 1 : 0) +
                " region min=(" + std::to_string(regionLeftX) + "," + std::to_string(regionTopY) + ")" +
                " max=(" + std::to_string(regionRightX) + "," + std::to_string(regionBottomY) + ")" +
                " fontSize=" + std::to_string(fontSize));
        s_prevEnabled = enabled;
        s_prevX = regionLeftX;
        s_prevY = regionTopY;
        s_prevMaxX = regionRightX;
        s_prevMaxY = regionBottomY;
        s_prevFontSize = fontSize;
    }
}

void Tick() noexcept
{
    g_subtitleTraceFrame.fetch_add(1, std::memory_order_acq_rel);

    // Lazy one-time install: retries each frame until the game object table is populated.
    if (!g_installed.load(std::memory_order_acquire))
    {
        static bool s_tried = false;   // present-thread only; single-flag latch is fine here
        if (!s_tried || !g_installed.load(std::memory_order_acquire))
        {
            s_tried = true;
            if (!InstallNow()) return;   // not ready yet; try again next frame
        }
    }

    // Scaleform applies (PCHUD, conversation) happen ONLY inside the ProcessEvent hook, on the LIVE handler's
    // freshly-read panel (guaranteed alive at that instant). A Tick-side Scaleform apply used a stale cached
    // panel and crashed the game on a HUD transition (freed-panel read).
    // Subtitles are different: no ProcessEvent-driven handler, so they're steered here - but only ever a
    // byte-write to a freshly validated (re-found-on-miss) object, never a method call on a cached pointer.
    // ============================================================================================
    // ALL SUBTITLE HOOKS DISABLED FOR TEST 2026-07-07 (by request): render subtitles with ZERO mod
    // interference to observe the game's own native behavior. Backup: SOURCE_BACKUPS zip + git tag
    // pre-subtitle-strip-20260707. To restore, uncomment this block, the InstallNativeSubtitleProbes() call,
    // and remove the "false &&" guard on the BioHUD.PostRender special-case. HUD (pchud transform + elements)
    // is untouched.
    // TickSubtitles();                  // subtitle mode/region steering
    // ProbeLiveBioSubtitles();          // passive [SUBPROBE] diagnostic
    // TickNativeSubtitleFontSize();     // native m_FontSize write
    // ResolveCanvasFns();               // resolves DrawText/PostRender/GetSubtitleRegion (redraw + overlay)
    // if (g_nativeSubMove.load(std::memory_order_acquire) ||
    //     g_subtitleRedraw.load(std::memory_order_acquire))
    // {
    //     DumpSubtitleOwnershipCandidates();
    // }
    // ============================================================================================
}
}  // namespace MELEVR::Pchud
