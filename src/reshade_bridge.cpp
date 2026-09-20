// [RESHADE] Detect a user-installed ReShade (add-on support build), register as an addon, hold the
// effect runtime, and render enabled techniques onto each OpenXR eye at submit. No ReShade -> inert.
// This TU MUST stay ImGui-free: reshade::register_addon does an ImGui-table version check only when
// IMGUI_VERSION_NUM is defined, and a mismatch there would fail registration. ImGui is used elsewhere,
// never here.
#include <windows.h>
#include <psapi.h>
#include <d3d11.h>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <string>
#include <reshade.hpp>
#include "reshade_bridge.h"
#include "logger.h"

extern HMODULE g_thisModule;   // defined at global scope in dllmain.cpp

namespace
{
std::atomic_bool g_registered{false};
std::atomic_bool g_exhausted{false};
std::atomic_bool g_enabled{true};
std::atomic_bool g_dead{false};            // one-shot disable after any failure
std::atomic_bool g_appliedLogged{false};
std::atomic<int> g_tries{0};
std::atomic<reshade::api::effect_runtime*> g_runtime{nullptr};
std::unordered_map<ID3D11Texture2D*, ID3D11RenderTargetView*> g_rtvCache;   // render-thread only
ID3D11ShaderResourceView* g_lastDepthBound = nullptr;                       // render-thread only

// In-headset technique + parameter lists (render/menu thread only). Re-enumerated when the menu draws.
struct TechEntry { reshade::api::effect_technique handle; std::string name; };
std::vector<TechEntry> g_techniques;
struct ParamEntry
{
    reshade::api::effect_uniform_variable handle;
    std::string label;
    int kind;        // 0=bool, 1=int, 2=float
    int comps;       // 1..4
    float fmin, fmax;
};
std::vector<ParamEntry> g_params;

// Scan loaded modules for the ReShade addon exports (ReShade may be d3d11.dll, opengl32.dll, etc. -
// never assume a name). Mirrors reshade.hpp's own get_reshade_module_handle.
HMODULE FindReShadeModule() noexcept
{
    HMODULE mods[1024]; DWORD needed = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return nullptr;
    const DWORD count = (needed < sizeof(mods) ? needed : sizeof(mods)) / sizeof(HMODULE);
    for (DWORD i = 0; i < count; ++i)
        if (GetProcAddress(mods[i], "ReShadeRegisterAddon") && GetProcAddress(mods[i], "ReShadeUnregisterAddon"))
            return mods[i];
    return nullptr;
}

void OnInitEffectRuntime(reshade::api::effect_runtime* rt)
{
    g_runtime.store(rt);
    g_lastDepthBound = nullptr;   // force a re-bind on the new runtime
    MELEVR::Logger::LogLine("[RESHADE] effect runtime ready");
}
// Fallback: init_effect_runtime fires at swapchain creation, BEFORE registration happens (registration
// happens from the present hook, ~frame 30), so it gets missed. reshade_present fires every frame and
// carries the runtime, so it's captured on the first present after registering. This is what actually makes the runtime live.
void OnReShadePresent(reshade::api::effect_runtime* rt)
{
    if (rt != nullptr && g_runtime.load() == nullptr)
    {
        g_runtime.store(rt);
        g_lastDepthBound = nullptr;
        MELEVR::Logger::LogLine("[RESHADE] effect runtime ready (captured at present)");
    }
}
void OnDestroyEffectRuntime(reshade::api::effect_runtime* rt)
{
    reshade::api::effect_runtime* cur = g_runtime.load();
    if (cur == rt)
    {
        g_runtime.store(nullptr);
        g_lastDepthBound = nullptr;
        for (auto& e : g_rtvCache) if (e.second) e.second->Release();
        g_rtvCache.clear();
    }
}

// OpenXR runtimes (Meta's in particular) hand back TYPELESS swapchain textures; a null-desc RTV cannot
// infer a format from those (E_INVALIDARG - the exact 'eye RTV create failed' from the 12:49 log). Map the
// texture's format family to an explicit typed format instead.
DXGI_FORMAT TypedRtvFormat(DXGI_FORMAT texFmt) noexcept
{
    switch (texFmt)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;   // the eye swapchains are SRGB
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:   return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:  return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return texFmt;   // already typed; use as-is
    }
}

// Cache/create the eye RTV outside any SEH scope (map + COM create can throw C++ exceptions).
ID3D11RenderTargetView* GetOrCreateRtv(ID3D11Texture2D* image) noexcept
{
    ID3D11RenderTargetView*& rtv = g_rtvCache[image];
    if (rtv == nullptr)
    {
        ID3D11Device* dev = nullptr; image->GetDevice(&dev);
        if (dev == nullptr) return nullptr;
        D3D11_TEXTURE2D_DESC td = {};
        image->GetDesc(&td);
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = TypedRtvFormat(td.Format);
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        rd.Texture2D.MipSlice = 0;
        HRESULT hr = dev->CreateRenderTargetView(image, &rd, &rtv);
        if (FAILED(hr) || rtv == nullptr)
        {
            rtv = nullptr;
            hr = dev->CreateRenderTargetView(image, nullptr, &rtv);   // typed-texture fallback
        }
        dev->Release();
        if (FAILED(hr) || rtv == nullptr)
        {
            rtv = nullptr;
            g_dead.store(true);
            char line[160];
            std::snprintf(line, sizeof(line),
                          "[RESHADE] eye RTV create failed (texFmt=%d viewFmt=%d hr=0x%08X) - disabled this session",
                          static_cast<int>(td.Format), static_cast<int>(rd.Format), static_cast<unsigned>(hr));
            MELEVR::Logger::LogLine(line);
            return nullptr;
        }
    }
    return rtv;
}

// SEH-only wrapper: POD locals exclusively (no C++ object unwinding allowed alongside __try/__except).
int RenderEffectsSEH(reshade::api::effect_runtime* rt, ID3D11RenderTargetView* rtv) noexcept
{
    __try
    {
        const reshade::api::resource_view v = { reinterpret_cast<uint64_t>(rtv) };
        rt->render_effects(rt->get_command_queue()->get_immediate_command_list(), v, v);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 1;
    }
}
int BindDepthSEH(reshade::api::effect_runtime* rt, ID3D11ShaderResourceView* srv) noexcept
{
    __try
    {
        const reshade::api::resource_view v = { reinterpret_cast<uint64_t>(srv) };
        rt->update_texture_bindings("DEPTH", v, v);
        return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 1;
    }
}
}  // namespace

namespace MELEVR::ReShadeBridge
{
bool Detected() noexcept { return FindReShadeModule() != nullptr; }
bool RuntimeReady() noexcept { return g_registered.load() && g_runtime.load() != nullptr && !g_dead.load(); }
const char* StatusLine() noexcept
{
    if (g_dead.load()) return "ReShade link errored this session (see MELEVR_Log.txt).";
    if (!g_registered.load()) return g_exhausted.load() ? "ReShade found but addon registration failed." : "Connecting to ReShade...";
    if (g_runtime.load() == nullptr) return "Waiting for ReShade's effect runtime...";
    return "";
}
void SetEnabled(bool on) noexcept { g_enabled.store(on); }

void TickRegistration() noexcept
{
    if (g_registered.load() || g_exhausted.load()) return;
    if (g_tries.fetch_add(1) > 120)   // ~30s of retries at ~4Hz, then give up quietly
    {
        g_exhausted.store(true);
        MELEVR::Logger::LogLine("[RESHADE] not detected - inert");
        return;
    }
    if (FindReShadeModule() == nullptr) return;
    if (reshade::register_addon(::g_thisModule))
    {
        reshade::register_event<reshade::addon_event::init_effect_runtime>(&OnInitEffectRuntime);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(&OnDestroyEffectRuntime);
        reshade::register_event<reshade::addon_event::reshade_present>(&OnReShadePresent);   // late-registration fallback
        g_registered.store(true);
        MELEVR::Logger::LogLine("[RESHADE] addon registered");
    }
    else
    {
        g_exhausted.store(true);   // ReShade present but registration refused (non-addon build / version)
        MELEVR::Logger::LogLine("[RESHADE] detected but addon registration failed - install the add-on support build");
    }
}

void ApplyToImage(ID3D11Texture2D* image) noexcept
{
    if (image == nullptr || !g_enabled.load() || !RuntimeReady()) return;
    reshade::api::effect_runtime* rt = g_runtime.load();
    if (rt == nullptr) return;
    ID3D11RenderTargetView* rtv = GetOrCreateRtv(image);
    if (rtv == nullptr) return;
    if (RenderEffectsSEH(rt, rtv) != 0)
    {
        g_dead.store(true);
        MELEVR::Logger::LogLine("[RESHADE] render_effects crashed - disabled this session");
        return;
    }
    if (!g_appliedLogged.exchange(true)) MELEVR::Logger::LogLine("[RESHADE] first per-eye render_effects applied");
}

void BindDepth(ID3D11ShaderResourceView* depthSrv) noexcept
{
    if (!g_enabled.load() || !RuntimeReady()) return;
    if (depthSrv == g_lastDepthBound) return;   // only re-bind on change (cheap steady state)
    reshade::api::effect_runtime* rt = g_runtime.load();
    if (rt == nullptr) return;
    if (BindDepthSEH(rt, depthSrv) == 0) g_lastDepthBound = depthSrv;
}

void RefreshTechniques() noexcept
{
    g_techniques.clear();
    g_params.clear();
    reshade::api::effect_runtime* rt = RuntimeReady() ? g_runtime.load() : nullptr;
    if (rt == nullptr) return;
    rt->enumerate_techniques(nullptr,
        [](reshade::api::effect_runtime* r, reshade::api::effect_technique t, void* ud)
        {
            char nm[128] = {};
            r->get_technique_name(t, nm);
            static_cast<std::vector<TechEntry>*>(ud)->push_back({ t, std::string(nm) });
        }, &g_techniques);
    rt->enumerate_uniform_variables(nullptr,
        [](reshade::api::effect_runtime* r, reshade::api::effect_uniform_variable v, void* ud)
        {
            // Only surface variables with a UI annotation (the rest are internal/hidden).
            char uitype[32] = {};
            r->get_annotation_string_from_uniform_variable(v, "ui_type", uitype);
            if (uitype[0] == '\0') return;
            if (strcmp(uitype, "hidden") == 0) return;

            ParamEntry e{};
            e.handle = v;
            char label[128] = {};
            if (!r->get_annotation_string_from_uniform_variable(v, "ui_label", label) || label[0] == '\0')
                r->get_uniform_variable_name(v, label);
            e.label = label;

            reshade::api::format base = reshade::api::format::unknown;
            uint32_t rows = 1, cols = 1, arr = 0;
            r->get_uniform_variable_type(v, &base, &rows, &cols, &arr);
            e.comps = static_cast<int>(rows * cols);
            if (e.comps < 1) e.comps = 1;
            if (e.comps > 4) e.comps = 4;

            if (strcmp(uitype, "checkbox") == 0)      e.kind = 0;   // bool
            else if (base == reshade::api::format::r32_float) e.kind = 2;   // float
            else                                      e.kind = 1;   // int/uint

            e.fmin = 0.0f; e.fmax = (e.kind == 2) ? 1.0f : 100.0f;
            r->get_annotation_float_from_uniform_variable(v, "ui_min", &e.fmin, 1);
            r->get_annotation_float_from_uniform_variable(v, "ui_max", &e.fmax, 1);
            if (e.fmax <= e.fmin) e.fmax = e.fmin + 1.0f;

            static_cast<std::vector<ParamEntry>*>(ud)->push_back(std::move(e));
        }, &g_params);
}
int ParamCount() noexcept { return static_cast<int>(g_params.size()); }
const char* ParamName(int index) noexcept
{
    return (index >= 0 && index < static_cast<int>(g_params.size())) ? g_params[index].label.c_str() : "";
}
int ParamKind(int index) noexcept
{
    return (index >= 0 && index < static_cast<int>(g_params.size())) ? g_params[index].kind : 2;
}
int ParamComponents(int index) noexcept
{
    return (index >= 0 && index < static_cast<int>(g_params.size())) ? g_params[index].comps : 1;
}
float ParamMin(int index) noexcept
{
    return (index >= 0 && index < static_cast<int>(g_params.size())) ? g_params[index].fmin : 0.0f;
}
float ParamMax(int index) noexcept
{
    return (index >= 0 && index < static_cast<int>(g_params.size())) ? g_params[index].fmax : 1.0f;
}
void ParamGetFloat(int index, float* out4) noexcept
{
    if (out4 == nullptr) return;
    out4[0] = out4[1] = out4[2] = out4[3] = 0.0f;
    reshade::api::effect_runtime* rt = RuntimeReady() ? g_runtime.load() : nullptr;
    if (rt == nullptr || index < 0 || index >= static_cast<int>(g_params.size())) return;
    rt->get_uniform_value_float(g_params[index].handle, out4, static_cast<size_t>(g_params[index].comps));
}
void ParamSetFloat(int index, const float* v) noexcept
{
    reshade::api::effect_runtime* rt = RuntimeReady() ? g_runtime.load() : nullptr;
    if (rt == nullptr || v == nullptr || index < 0 || index >= static_cast<int>(g_params.size())) return;
    rt->set_uniform_value_float(g_params[index].handle, v, static_cast<size_t>(g_params[index].comps));
}
int ParamGetInt(int index) noexcept
{
    int32_t val = 0;
    reshade::api::effect_runtime* rt = RuntimeReady() ? g_runtime.load() : nullptr;
    if (rt == nullptr || index < 0 || index >= static_cast<int>(g_params.size())) return 0;
    rt->get_uniform_value_int(g_params[index].handle, &val, 1);
    return val;
}
void ParamSetInt(int index, int v) noexcept
{
    reshade::api::effect_runtime* rt = RuntimeReady() ? g_runtime.load() : nullptr;
    if (rt == nullptr || index < 0 || index >= static_cast<int>(g_params.size())) return;
    const int32_t val = v;
    rt->set_uniform_value_int(g_params[index].handle, &val, 1);
}
bool ParamGetBool(int index) noexcept
{
    bool val = false;
    reshade::api::effect_runtime* rt = RuntimeReady() ? g_runtime.load() : nullptr;
    if (rt == nullptr || index < 0 || index >= static_cast<int>(g_params.size())) return false;
    rt->get_uniform_value_bool(g_params[index].handle, &val, 1);
    return val;
}
void ParamSetBool(int index, bool v) noexcept
{
    reshade::api::effect_runtime* rt = RuntimeReady() ? g_runtime.load() : nullptr;
    if (rt == nullptr || index < 0 || index >= static_cast<int>(g_params.size())) return;
    rt->set_uniform_value_bool(g_params[index].handle, &v, 1);
}
int TechniqueCount() noexcept { return static_cast<int>(g_techniques.size()); }
const char* TechniqueName(int index) noexcept
{
    return (index >= 0 && index < static_cast<int>(g_techniques.size())) ? g_techniques[index].name.c_str() : "";
}
bool TechniqueEnabled(int index) noexcept
{
    reshade::api::effect_runtime* rt = RuntimeReady() ? g_runtime.load() : nullptr;
    if (rt == nullptr || index < 0 || index >= static_cast<int>(g_techniques.size())) return false;
    return rt->get_technique_state(g_techniques[index].handle);
}
void SetTechniqueEnabled(int index, bool on) noexcept
{
    reshade::api::effect_runtime* rt = RuntimeReady() ? g_runtime.load() : nullptr;
    if (rt == nullptr || index < 0 || index >= static_cast<int>(g_techniques.size())) return;
    rt->set_technique_state(g_techniques[index].handle, on);
}
}
