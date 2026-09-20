#include "d3d_capture.h"

#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "logger.h"
#include "render_hook.h"
#include "vr_config.h"
#include "vr_menu.h"
#include "xr_session.h"
#include "MinHook.h"

// Clean vtable-slot capture, authored from the proven research pattern (raw VirtualProtect
// slot swap - no MinHook needed for FlatMono). Step 0b-1 only LOGS what it captures; the
// game keeps rendering exactly as normal. This hooks ALL the DXGI swapchain entry points
// (legacy CreateSwapChain + the modern IDXGIFactory2 ones) because LE1 uses ForHwnd.

namespace
{
using MELEVR::Logger::LogLine;

constexpr UINT kPresentVTableIndex = 8;                          // IDXGISwapChain::Present
constexpr UINT kCreateSwapChainVTableIndex = 10;                 // IDXGIFactory::CreateSwapChain
constexpr UINT kCreateSwapChainForHwndVTableIndex = 15;          // IDXGIFactory2::CreateSwapChainForHwnd
constexpr UINT kCreateSwapChainForCoreWindowVTableIndex = 16;    // IDXGIFactory2::CreateSwapChainForCoreWindow
constexpr UINT kCreateSwapChainForCompositionVTableIndex = 24;   // IDXGIFactory2::CreateSwapChainForComposition
constexpr UINT kDeviceCreateBufferVTableIndex = 3;               // ID3D11Device::CreateBuffer
constexpr UINT kContextVSSetConstantBuffersVTableIndex = 7;      // ID3D11DeviceContext::VSSetConstantBuffers
constexpr UINT kContextDrawIndexedVTableIndex = 12;              // ID3D11DeviceContext::DrawIndexed
constexpr UINT kContextDrawVTableIndex = 13;                     // ID3D11DeviceContext::Draw
constexpr UINT kContextMapVTableIndex = 14;                      // ID3D11DeviceContext::Map
constexpr UINT kContextUnmapVTableIndex = 15;                    // ID3D11DeviceContext::Unmap
constexpr UINT kContextDrawIndexedInstancedVTableIndex = 20;     // ID3D11DeviceContext::DrawIndexedInstanced
constexpr UINT kContextDrawInstancedVTableIndex = 21;            // ID3D11DeviceContext::DrawInstanced
constexpr UINT kContextDrawAutoVTableIndex = 38;                 // ID3D11DeviceContext::DrawAuto
constexpr UINT kContextUpdateSubresourceVTableIndex = 48;        // ID3D11DeviceContext::UpdateSubresource
constexpr UINT kContextExecuteCommandListVTableIndex = 58;       // ID3D11DeviceContext::ExecuteCommandList
constexpr UINT kDeviceCreateDeferredContextVTableIndex = 27;     // ID3D11Device::CreateDeferredContext
constexpr UINT kDeviceCreatePixelShaderVTableIndex = 15;         // ID3D11Device::CreatePixelShader

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCoreWindowFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainForCompositionFn = HRESULT(STDMETHODCALLTYPE*)(
    IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
using CreateBufferFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**);
using VSSetConstantBuffersFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using DrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using MapFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using UnmapFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using DrawIndexedInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
using DrawInstancedFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using DrawAutoFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using UpdateSubresourceFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
using ExecuteCommandListFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
using CreateDeferredContextFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT, ID3D11DeviceContext**);
using CreatePixelShaderFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
using ClearDSVFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
constexpr UINT kClearDSVSlot = 53;   // ID3D11DeviceContext::ClearDepthStencilView vtable index
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
constexpr UINT kOMSetRenderTargetsSlot = 33;   // ID3D11DeviceContext::OMSetRenderTargets vtable index

std::mutex g_hookMutex;
PresentFn g_originalPresent = nullptr;
void** g_presentSlot = nullptr;
CreateSwapChainFn g_originalCreateSwapChain = nullptr;
void** g_createSwapChainSlot = nullptr;
CreateSwapChainForHwndFn g_originalCreateSwapChainForHwnd = nullptr;
void** g_createSwapChainForHwndSlot = nullptr;
CreateSwapChainForCoreWindowFn g_originalCreateSwapChainForCoreWindow = nullptr;
void** g_createSwapChainForCoreWindowSlot = nullptr;
CreateSwapChainForCompositionFn g_originalCreateSwapChainForComposition = nullptr;
void** g_createSwapChainForCompositionSlot = nullptr;
CreateBufferFn g_originalCreateBuffer = nullptr;
void** g_createBufferSlot = nullptr;
CreatePixelShaderFn g_originalCreatePixelShader = nullptr;
void** g_createPixelShaderSlot = nullptr;
VSSetConstantBuffersFn g_originalVSSetConstantBuffers = nullptr;
void** g_vsSetConstantBuffersSlot = nullptr;
DrawIndexedFn g_originalDrawIndexed = nullptr;
void** g_drawIndexedSlot = nullptr;
DrawFn g_originalDraw = nullptr;
void** g_drawSlot = nullptr;
MapFn g_originalMap = nullptr;
void** g_mapSlot = nullptr;
UnmapFn g_originalUnmap = nullptr;
void** g_unmapSlot = nullptr;
DrawIndexedInstancedFn g_originalDrawIndexedInstanced = nullptr;
void** g_drawIndexedInstancedSlot = nullptr;
DrawInstancedFn g_originalDrawInstanced = nullptr;
void** g_drawInstancedSlot = nullptr;
DrawAutoFn g_originalDrawAuto = nullptr;
void** g_drawAutoSlot = nullptr;
UpdateSubresourceFn g_originalUpdateSubresource = nullptr;
void** g_updateSubresourceSlot = nullptr;
ExecuteCommandListFn g_originalExecuteCommandList = nullptr;
void** g_executeCommandListSlot = nullptr;
DrawIndexedFn g_originalDeferredDrawIndexed = nullptr;
void** g_deferredDrawIndexedSlot = nullptr;
DrawFn g_originalDeferredDraw = nullptr;
void** g_deferredDrawSlot = nullptr;
VSSetConstantBuffersFn g_originalDeferredVSSetConstantBuffers = nullptr;
void** g_deferredVSSetConstantBuffersSlot = nullptr;
MapFn g_originalDeferredMap = nullptr;
void** g_deferredMapSlot = nullptr;
UnmapFn g_originalDeferredUnmap = nullptr;
void** g_deferredUnmapSlot = nullptr;
DrawIndexedInstancedFn g_originalDeferredDrawIndexedInstanced = nullptr;
void** g_deferredDrawIndexedInstancedSlot = nullptr;
DrawInstancedFn g_originalDeferredDrawInstanced = nullptr;
void** g_deferredDrawInstancedSlot = nullptr;
DrawAutoFn g_originalDeferredDrawAuto = nullptr;
void** g_deferredDrawAutoSlot = nullptr;
UpdateSubresourceFn g_originalDeferredUpdateSubresource = nullptr;
void** g_deferredUpdateSubresourceSlot = nullptr;
CreateDeferredContextFn g_originalCreateDeferredContext = nullptr;
void** g_createDeferredContextSlot = nullptr;
std::atomic_bool g_captured{false};
std::atomic<unsigned long long> g_presentCount{0};
std::atomic<unsigned long long> g_d3dDrawCallCount{0};
std::atomic<int> g_d3dDrawStackLogs{0};
std::atomic<int> g_d3dExecuteStackLogs{0};
std::atomic<int> g_cbCreateLogs{0};
std::atomic<int> g_cbBindLogs{0};
std::atomic<int> g_cbWriteLogs{0};
std::atomic<int> g_cbMatrixLogs{0};
std::atomic<int> g_cbDrawLogs{0};
std::atomic<int> g_camCb96Logs{0};
std::atomic_bool g_cbTraceHotPathEnabled{false};
std::atomic_bool g_drawTraceHotPathEnabled{false};
std::atomic_bool g_camCbHotPathEnabled{false};
std::atomic<int> g_camCb96NudgeLogs{0};
std::atomic<bool> g_camCb96NudgeMarkerLogged{false};
std::atomic<int> g_camCb96AtlasLogs{0};
std::atomic<bool> g_camCb96AtlasMarkerLogged{false};
std::atomic<int> g_camCb96SidecarLogs{0};
std::atomic<bool> g_camCb96SidecarMarkerLogged{false};
std::atomic<unsigned long long> g_camCb96SidecarLastClearPresent{0};
std::atomic<int> g_camCb96SidecarCopyLogs{0};
std::atomic<int> g_camCb96SidecarCreateFailLogs{0};
std::atomic<int> g_camCb96SidecarSkipLogs{0};
ULONGLONG g_presentWindowStartMs = 0;
unsigned int g_presentWindowFrames = 0;
UINT g_lastOriginalSyncInterval = 0;
UINT g_lastOriginalPresentFlags = 0;
// [TEARING] Decouple the flat present from the display COMPOSITION rate. On a flip-model swapchain,
// syncInterval=0 alone is still bound to the compositor's refresh (the desktop monitor's), so on a
// low-refresh monitor the present - and therefore the XR submit driven off it - hard-caps at the monitor's
// Hz even when the headset runs faster. Log-proven 2026-07-18 (measured: fps=60 at present flags=0x0 vs 85-95
// at 0x200 on a 120Hz headset / 60Hz monitor). DXGI_PRESENT_ALLOW_TEARING lifts that cap, but it is ONLY
// legal when the swapchain was created with DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING - otherwise Present returns
// DXGI_ERROR_INVALID_CALL. g_swapchainHasTearingFlag records that (set at every (re)creation); if it is ever
// false, tearing is simply not forced, so the behaviour is identical to before = fail-safe.
constexpr UINT kDxgiSwapChainFlagAllowTearing = 2048;  // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING (dxgi1_5.h)
constexpr UINT kDxgiPresentAllowTearing       = 512;   // DXGI_PRESENT_ALLOW_TEARING (dxgi1_5.h)
std::atomic_bool g_swapchainHasTearingFlag{false};
bool g_forceTearThisPresent = false;   // decided once per present (render thread only); mirrored into [PRESENTHZ]
std::atomic_bool g_openXrDisableLogged{false};
IDXGISwapChain* g_gameSwapChain = nullptr;
ID3D11Device* g_gameDevice = nullptr;
ID3D11DeviceContext* g_gameContext = nullptr;
std::mutex g_sidecarMutex;
ID3D11Texture2D* g_sidecarColor = nullptr;          // LEFT eye (full-size, isolated)
ID3D11RenderTargetView* g_sidecarRtv = nullptr;
ID3D11ShaderResourceView* g_sidecarSrv = nullptr;
ID3D11Texture2D* g_sidecarDepth = nullptr;
ID3D11DepthStencilView* g_sidecarDsv = nullptr;
ID3D11Texture2D* g_sidecarColorR = nullptr;         // RIGHT eye (full-size, isolated) - own target, no atlas/crop
ID3D11RenderTargetView* g_sidecarRtvR = nullptr;
ID3D11Texture2D* g_sidecarDepthR = nullptr;
ID3D11DepthStencilView* g_sidecarDsvR = nullptr;
D3D11_TEXTURE2D_DESC g_sidecarColorDesc = {};
D3D11_TEXTURE2D_DESC g_sidecarDepthDesc = {};
D3D11_DEPTH_STENCIL_VIEW_DESC g_sidecarDsvDesc = {};
bool g_sidecarValid = false;

// --- DIBR depth capture: the game's scene depth is DSV-only, so copy it into a sampleable R24 texture
//     the instant before the game clears it (proven parked-build mechanism, reimplemented clean). ---
ClearDSVFn g_originalClearDSV = nullptr;
void** g_clearDSVSlot = nullptr;
ID3D11Texture2D* g_depthCopy = nullptr;            // capture scratch: every populated scene-clear lands here (may be a flat/wrong pooled pass)
// [RELIEF M0.4] g_depthPublished = the last VERIFIED-REAL depth. In stereo the scene depth resource is
// POOLED - cleared several times per frame, some passes flat - so capturing "at clear time" catches the
// wrong pass intermittently (the white<->normal flicker). Fix: only promote a capture to g_depthPublished
// when its probe verifies real depth (has spread); hold the last good frame otherwise. The SRV the viz +
// warp read lives on g_depthPublished, so they always see stable good depth, never the flicker.
ID3D11Texture2D* g_depthPublished = nullptr;
ID3D11ShaderResourceView* g_depthSrv = nullptr;    // views g_depthPublished (the good one), NOT g_depthCopy
UINT g_depthCopyW = 0;
UINT g_depthCopyH = 0;
std::atomic<bool> g_depthReady{false};
std::atomic<int> g_depthCapLogs{0};
std::atomic<int> g_depthMissLogs{0};              // diagnostic: non-scene-sized depth clears
// LIVE render resolution (== backbuffer; LE1 locks scene-depth to the display mode). The scene-depth capture
// gate matches THIS, not a hardcoded 2096 (which broke DIBR at every other resolution). Set at session start.
std::atomic<UINT> g_sceneRenderW{0};
std::atomic<UINT> g_sceneRenderH{0};
ID3D11Texture2D* g_depthStaging[2] = {nullptr, nullptr};  // DOUBLE-BUFFERED non-blocking readback for the depth probe
bool g_depthStagingInFlight[2] = {false, false};            // a copy was issued into this slot, not yet mapped
int  g_depthStagingWrite = 0;                               // slot the NEXT copy goes into
// [RELIEF M0.4b] GPU-side snapshot ring paired 1:1 with the staging ring. Promote reads from THIS (GPU->GPU,
// ~free) instead of from the staging texture (staging is CPU memory -> a 33MB upload on the render thread
// every promote = the "hang" that was seen). Each holds the exact frame its staging twin was probed from.
ID3D11Texture2D* g_depthGpuSnap[2] = {nullptr, nullptr};
std::atomic<int> g_depthProbeLogs{0};

// Per-depth-buffer geometry-draw tracking (all render-thread - no locks). The scene depth is the one that
// receives MANY draws between clears; a blank/recycled buffer receives ~0. At clear time only a
// buffer that crossed the draw threshold, so a freshly-cleared (all-far) depth can't paint the viz black.
ID3D11DepthStencilView* g_boundDsv = nullptr;            // last DSV bound via OMSetRenderTargets (identity)
// [RELIEF M0.1] the RESOURCE behind the bound DSV. Stereo@4K broke the old "the game clears the same DSV
// object it binds" assumption (draws landed on one DSV object, the clear came through another view of the
// same/another resource -> drawsSince=0 in gameplay and a UI depth full of 1.0 won the capture while the
// Insert menu was open). Attribution + capture are keyed by resource pointer now. Raw identity only -
// never dereferenced, so a stale pointer is harmless.
void* g_boundDepthRes = nullptr;
// [RELIEF M0.5] mid-frame STEREO depth vantage. In stereo the pooled scene depth is trampled to 1.0 by
// the UI stage BEFORE any hooked clear, so clear-time captures only ever photographed the corpse (every
// stereo probe: flat 1.0). The scene depth is alive mid-frame; the moment the FIRST UI draw of the frame
// arrives (the stereo UI-dup hook already detects exactly that), the scene render is complete and intact.
// g_busyDepthTex = AddRef'd texture of the last DSV whose segment crossed the draw threshold (= the scene
// depth, stashed by NoteDepthDraw); MaybeCaptureDepthAtUiStart snapshots it once per present.
void* g_lastDepthResRaw = nullptr;               // change detector for the QI cache below
ID3D11Texture2D* g_boundDepthTex = nullptr;      // AddRef'd tex of the CURRENTLY bound DSV
ID3D11Texture2D* g_busyDepthTex = nullptr;       // AddRef'd tex of the MOST-DRAWN depth (the true scene depth)
uint32_t g_busyDepthDraws = 0;                   // leader's draw count (the scene depth climbs fastest)
bool g_uiDepthCaptured = false;                  // once-per-present latch for the UI-time capture
OMSetRenderTargetsFn g_originalOMSetRenderTargets = nullptr;
void** g_omSetRtSlot = nullptr;
struct DepthDrawEntry { void* dsv; uint32_t draws; };   // .dsv now holds the depth RESOURCE pointer
DepthDrawEntry g_depthDraws[16] = {};
// [RELIEF M0.4] the blacklist (M0.1-M0.3) is GONE - it fought the stereo depth pool by parking whole
// resources, which flickered/froze capture. Replaced by content-gated promotion (only a spread-verified
// capture is published; hold-last-good otherwise). See the probe block + g_depthPublished.
// [RELIEF M0.3] draw-attribution meter: total scene-thread draws seen while capture is armed vs those that
// had a bound depth resource to attribute to. total>>attrib in a mode = the depth is bound by a path
// not seen (the tell for why stereo undercounts). Reset each [RELIEF] tick (exchange).
std::atomic<uint32_t> g_dbgDrawsTotal{0};
std::atomic<uint32_t> g_dbgDrawsAttrib{0};
// [RELIEF M0.2] the depth copy adapts to the SOURCE format now. The old code hardcoded R24G8_TYPELESS;
// if the game's depth is D32_FLOAT(_S8X24) the CopyResource is ILLEGAL and D3D silently drops it -> the
// copy/staging read uninitialized memory (the flat-1.0 probes). decode: 0=24-bit uint (4B texel),
// 1=float (4B texel), 2=float (8B texel, X24 stencil ignored).
DXGI_FORMAT g_depthCopyFmt = DXGI_FORMAT_UNKNOWN;   // source format the current copy was built for
int g_depthDecodeKind = 0;
constexpr uint32_t kSceneDepthDrawThreshold = 30;       // >= this many draws since last clear = a real geometry pass

// --- Per-eye UI duplication (ported from ME2) --------------------------------
// In SBS stereo the Scaleform 2D UI is drawn ONCE full-width across the SBS backbuffer, so each eye
// samples half of it (cross-eyed/scrambled). Fix: for each UI draw, re-issue it into each eye's
// half-viewport (half W, half H, centered vertically to preserve aspect). Only runs when STEREO mode
// is active (not AER/mono/DIBR - those render one full-frame eye). Gated on g_stereoUiActive (set by
// xr_session RunFrame each frame) AND g_uiDupEnabled (Insert-menu toggle).
std::atomic_bool g_uiDupEnabled{true};          // Insert-menu master enable for the per-eye UI dup
std::atomic_bool g_stereoUiActive{false};       // true only while SBS stereo mode is the active submit path
// Stereo UI vertical align: the per-eye dup letterboxes the UI into the middle half, which pulls the crosshair
// toward center so shots land higher than it. This shifts the letterboxed UI up (+) / down (-) as a fraction of
// eye height so the crosshair can be re-aligned to where shots actually go. 0 = ME2-exact centered.
std::atomic<float> g_stereoUiYShift{0.0f};
// Current bound RTV format/width (set in OMSetRenderTargetsHook), so the hot draw path can cheaply
// discriminate the LDR backbuffer (UI target) from the HDR world intermediates without re-querying.
UINT g_currentRtvWidth = 0;
UINT g_currentRtvHeight = 0;
void* g_currentRtvTexPtr = nullptr;             // identity only for [QPOST] dup keys; never dereferenced
DXGI_FORMAT g_currentRtvFormat = DXGI_FORMAT_UNKNOWN;
// UISTATE counters (per ~180-frame window) so the fix is verifiable in-headset.
std::atomic<unsigned> g_uiDI{0};                // UI-signature DrawIndexed (panels)
std::atomic<unsigned> g_uiDII{0};               // UI-signature DrawIndexedInstanced (safety)
std::atomic<unsigned> g_uiDnon{0};              // UI-signature non-indexed Draw (text)
std::atomic<unsigned> g_cntUiDup{0};            // total UI draws duplicated per window

constexpr int kTrackedConstantBuffers = 1024;
constexpr int kTrackedMaps = 32;
constexpr int kTrackedVsSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
struct ConstantBufferRecord
{
    ID3D11Buffer* buffer = nullptr;
    UINT byteWidth = 0;
    UINT bindFlags = 0;
    UINT usage = 0;
    UINT cpuAccess = 0;
    UINT lastHash = 0;
    UINT writes = 0;
    UINT lastBytes = 0;
    unsigned char lastData[256] = {};
};
struct MapRecord
{
    ID3D11Resource* resource = nullptr;
    void* data = nullptr;
    UINT bytes = 0;
};
std::mutex g_cbMutex;
ConstantBufferRecord g_cbRecords[kTrackedConstantBuffers] = {};
MapRecord g_maps[kTrackedMaps] = {};
ID3D11Buffer* g_vsBound[kTrackedVsSlots] = {};
constexpr int kStereoPassBanks = 3;
ID3D11Texture2D* g_stereoPassTex[kStereoPassBanks][2] = {};
ID3D11RenderTargetView* g_sfrPass0Rtv[kStereoPassBanks] = {};   // [SFR-UI] lazy RTVs onto pass 0 for the UI mirror
D3D11_TEXTURE2D_DESC g_stereoPassDesc = {};
bool g_stereoPassDescValid = false;
std::atomic<unsigned long long> g_stereoPassCopies[2] = {};
std::mutex g_stereoPassMutex;
int g_stereoWriteBank = 0;
int g_stereoPublishedBank = -1;
bool g_stereoWriteHasPass0 = false;
unsigned long long g_stereoPublishedPairCount = 0;
// [CAPTUREPOSE] Head-look arm captured with each texture bank. This is the ownership link that
// the old submit-time live RenderHook stamp was missing.
unsigned long long g_stereoPairArm[kStereoPassBanks] = {};

constexpr UINT kSavedVertexBufferCount = 8;
constexpr UINT kSavedConstantBufferCount = 16;
constexpr UINT kSavedSrvCount = 16;
constexpr UINT kSavedSamplerCount = 16;

struct ContextStateSnapshot
{
    bool valid = false;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    ID3D11RasterizerState* rasterizerState = nullptr;
    ID3D11BlendState* blendState = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0xFFFFFFFF;
    ID3D11DepthStencilState* depthStencilState = nullptr;
    UINT stencilRef = 0;
    ID3D11InputLayout* inputLayout = nullptr;
    ID3D11Buffer* vertexBuffers[kSavedVertexBufferCount] = {};
    UINT vertexStrides[kSavedVertexBufferCount] = {};
    UINT vertexOffsets[kSavedVertexBufferCount] = {};
    ID3D11Buffer* indexBuffer = nullptr;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    UINT indexOffset = 0;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11GeometryShader* geometryShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    ID3D11Buffer* vsConstantBuffers[kSavedConstantBufferCount] = {};
    ID3D11Buffer* gsConstantBuffers[kSavedConstantBufferCount] = {};
    ID3D11Buffer* psConstantBuffers[kSavedConstantBufferCount] = {};
    ID3D11ShaderResourceView* psSrvs[kSavedSrvCount] = {};
    ID3D11SamplerState* psSamplers[kSavedSamplerCount] = {};
};

ContextStateSnapshot g_replayState;
std::mutex g_replayStateMutex;
ULONGLONG g_lastVramLogMs = 0;

std::string HexPointer(const void* p)
{
    char buffer[32] = {};
    sprintf_s(buffer, "0x%p", p);
    return buffer;
}

std::string HexRva(std::uintptr_t v)
{
    char buffer[32] = {};
    sprintf_s(buffer, "0x%llX", static_cast<unsigned long long>(v));
    return buffer;
}

std::string AddrLabel(void* p)
{
    const auto addr = reinterpret_cast<std::uintptr_t>(p);
    const auto exe = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (exe != 0 && addr >= exe && addr < exe + 0x20000000)
    {
        return std::string("MassEffect1.exe+") + HexRva(addr - exe);
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
        return std::string(name) + "+" + HexRva(addr - reinterpret_cast<std::uintptr_t>(mod));
    }

    return HexPointer(p);
}

void LogD3DDrawStack(const char* kind) noexcept
{
    if (!g_drawTraceHotPathEnabled.load(std::memory_order_relaxed))
    {
        return;
    }

    const unsigned long long drawN = g_d3dDrawCallCount.fetch_add(1, std::memory_order_relaxed) + 1;
    const int logN = g_d3dDrawStackLogs.fetch_add(1, std::memory_order_acq_rel);
    if (logN >= 80)
    {
        return;
    }

    void* frames[20] = {};
    const USHORT frameCount = RtlCaptureStackBackTrace(0, 20, frames, nullptr);
    std::string line = std::string("[D3DDRAW] #") + std::to_string(drawN) +
                       " kind=" + kind +
                       " thread=" + std::to_string(GetCurrentThreadId()) +
                       " stack=";
    for (USHORT i = 1; i < frameCount && i < 14; ++i)
    {
        if (i > 1) line += " <- ";
        line += AddrLabel(frames[i]);
    }
    LogLine(line);
}

void LogExecuteCommandListStack() noexcept
{
    if (!g_drawTraceHotPathEnabled.load(std::memory_order_relaxed))
    {
        return;
    }

    const int logN = g_d3dExecuteStackLogs.fetch_add(1, std::memory_order_acq_rel);
    if (logN >= 80)
    {
        return;
    }

    void* frames[20] = {};
    const USHORT frameCount = RtlCaptureStackBackTrace(0, 20, frames, nullptr);
    std::string line = "[D3DEXEC] thread=" + std::to_string(GetCurrentThreadId()) + " stack=";
    for (USHORT i = 1; i < frameCount && i < 14; ++i)
    {
        if (i > 1) line += " <- ";
        line += AddrLabel(frames[i]);
    }
    LogLine(line);
}

std::string HexHRESULT(HRESULT hr)
{
    char buffer[16] = {};
    sprintf_s(buffer, "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

UINT HashBytes(const void* data, UINT bytes) noexcept
{
    if (data == nullptr || bytes == 0) return 0;
    const auto* p = static_cast<const unsigned char*>(data);
    UINT h = 2166136261u;
    for (UINT i = 0; i < bytes; ++i)
    {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

bool CbTraceEnabled() noexcept
{
    return g_cbTraceHotPathEnabled.load(std::memory_order_relaxed);
}

bool DrawTraceEnabled() noexcept
{
    return g_drawTraceHotPathEnabled.load(std::memory_order_relaxed);
}

bool IsFiniteMatrix(const float* m) noexcept
{
    float maxAbs = 0.0f;
    for (int i = 0; i < 16; ++i)
    {
        if (!std::isfinite(m[i])) return false;
        maxAbs = (std::max)(maxAbs, std::fabs(m[i]));
    }
    return maxAbs > 0.05f && maxAbs < 1000000.0f;
}

int ScoreMatrixWindow(const float* m) noexcept
{
    if (!IsFiniteMatrix(m)) return 0;

    int score = 0;
    const float row0 = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    const float row1 = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    const float row2 = std::sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    const float wrow = std::sqrt(m[3] * m[3] + m[7] * m[7] + m[11] * m[11]);
    if (row0 > 0.25f && row0 < 5.0f) ++score;
    if (row1 > 0.25f && row1 < 5.0f) ++score;
    if (row2 > 0.25f && row2 < 5.0f) ++score;
    if (std::fabs(m[15]) < 0.10f && (wrow > 0.50f && wrow < 2.0f)) score += 3; // perspective VP, row-vector style.
    if (std::fabs(m[15] - 1.0f) < 0.10f) score += 1;                           // affine/view-like.
    if (std::fabs(m[0]) > 0.05f && std::fabs(m[5]) > 0.05f) ++score;
    return score;
}

ConstantBufferRecord* FindOrAddConstantBuffer(ID3D11Buffer* buffer, const D3D11_BUFFER_DESC* knownDesc = nullptr) noexcept
{
    if (buffer == nullptr) return nullptr;
    std::lock_guard<std::mutex> lock(g_cbMutex);
    int empty = -1;
    for (int i = 0; i < kTrackedConstantBuffers; ++i)
    {
        if (g_cbRecords[i].buffer == buffer) return &g_cbRecords[i];
        if (empty < 0 && g_cbRecords[i].buffer == nullptr) empty = i;
    }
    if (empty < 0) return nullptr;

    D3D11_BUFFER_DESC d = {};
    if (knownDesc != nullptr)
    {
        d = *knownDesc;
    }
    else
    {
        buffer->GetDesc(&d);
    }
    if ((d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0) return nullptr;
    g_cbRecords[empty].buffer = buffer;
    g_cbRecords[empty].byteWidth = d.ByteWidth;
    g_cbRecords[empty].bindFlags = d.BindFlags;
    g_cbRecords[empty].usage = d.Usage;
    g_cbRecords[empty].cpuAccess = d.CPUAccessFlags;
    return &g_cbRecords[empty];
}

void ScanConstantBufferBytes(const char* source, ID3D11Buffer* buffer, const void* data, UINT bytes) noexcept
{
    if (!CbTraceEnabled()) return;
    if (source == nullptr || buffer == nullptr || data == nullptr || bytes < 64) return;
    ConstantBufferRecord* rec = FindOrAddConstantBuffer(buffer);
    if (rec == nullptr) return;
    if (rec->byteWidth != 0) bytes = (std::min)(bytes, rec->byteWidth);
    if (bytes > 65536) bytes = 65536;

    const UINT hash = HashBytes(data, bytes);
    rec->lastHash = hash;
    ++rec->writes;
    rec->lastBytes = (std::min)(bytes, static_cast<UINT>(sizeof(rec->lastData)));
    if (rec->lastBytes > 0)
    {
        memcpy(rec->lastData, data, rec->lastBytes);
    }
    const int writeLog = g_cbWriteLogs.fetch_add(1, std::memory_order_relaxed);
    if (writeLog < 160 || (rec->writes % 300) == 1)
    {
        LogLine(std::string("[CBWRITE] source=") + source +
                " cb=" + HexPointer(buffer) +
                " bytes=" + std::to_string(bytes) +
                " hash=" + HexRva(hash) +
                " writes=" + std::to_string(rec->writes));
    }

    const auto* floats = static_cast<const float*>(data);
    const UINT floatCount = bytes / sizeof(float);
    for (UINT f = 0; f + 16 <= floatCount; f += 4)
    {
        const float* m = floats + f;
        const int score = ScoreMatrixWindow(m);
        if (score < 5) continue;
        const int n = g_cbMatrixLogs.fetch_add(1, std::memory_order_relaxed);
        if (n >= 240) return;
        char line[512] = {};
        sprintf_s(line,
                  "[CBMAT] source=%s cb=%p off=0x%X score=%d hash=0x%08X diag=(%.4f,%.4f,%.4f,%.4f) wrow=(%.4f,%.4f,%.4f,%.4f)",
                  source,
                  buffer,
                  f * 4,
                  score,
                  hash,
                  m[0], m[5], m[10], m[15],
                  m[3], m[7], m[11], m[15]);
        LogLine(line);
    }
}

void LogCameraCb96Candidate(const char* kind, ID3D11DeviceContext* context, ID3D11RenderTargetView* rtv,
                            ID3D11DepthStencilView* dsv, const D3D11_VIEWPORT& vp) noexcept
{
    if (kind == nullptr || strcmp(kind, "DrawIndexed") != 0 || context == nullptr || dsv == nullptr)
    {
        return;
    }
    if (vp.Width < 512.0f || vp.Height < 512.0f)
    {
        return;
    }

    ConstantBufferRecord snapshot = {};
    {
        std::lock_guard<std::mutex> lock(g_cbMutex);
        ID3D11Buffer* slot1 = g_vsBound[1];
        if (slot1 == nullptr) return;
        for (int r = 0; r < kTrackedConstantBuffers; ++r)
        {
            if (g_cbRecords[r].buffer == slot1)
            {
                snapshot = g_cbRecords[r];
                break;
            }
        }
    }

    if (snapshot.buffer == nullptr || snapshot.byteWidth != 96 || snapshot.lastBytes < 96)
    {
        return;
    }

    const int n = g_camCb96Logs.fetch_add(1, std::memory_order_relaxed);
    if (n >= 80)
    {
        return;
    }

    const float* f = reinterpret_cast<const float*>(snapshot.lastData);
    char line[1400] = {};
    sprintf_s(line,
              "[CAMCB96] #%d thread=%lu ctx=%p rtv=%p dsv=%p vp=%.0fx%.0f+%.0f,%.0f cb=%p hash=0x%08X "
              "r0=(%.6f,%.6f,%.6f,%.6f) r1=(%.6f,%.6f,%.6f,%.6f) r2=(%.6f,%.6f,%.6f,%.6f) "
              "r3=(%.6f,%.6f,%.6f,%.6f) r4=(%.6f,%.6f,%.6f,%.6f) r5=(%.6f,%.6f,%.6f,%.6f)",
              n + 1,
              GetCurrentThreadId(),
              context,
              rtv,
              dsv,
              vp.Width,
              vp.Height,
              vp.TopLeftX,
              vp.TopLeftY,
              snapshot.buffer,
              snapshot.lastHash,
              f[0], f[1], f[2], f[3],
              f[4], f[5], f[6], f[7],
              f[8], f[9], f[10], f[11],
              f[12], f[13], f[14], f[15],
              f[16], f[17], f[18], f[19],
              f[20], f[21], f[22], f[23]);
    LogLine(line);
}

// Uncached marker check: a filesystem stat for <markerfile> next to the game exe.
static bool MarkerEnabledUncached(const wchar_t* fileName) noexcept
{
    wchar_t exePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
    {
        return false;
    }

    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash == nullptr)
    {
        return false;
    }
    *(slash + 1) = L'\0';

    wchar_t marker[MAX_PATH] = {};
    if (swprintf_s(marker, L"%s%s", exePath, fileName) <= 0)
    {
        return false;
    }

    return GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES;
}

// Cached wrapper. The original ran an uncached GetFileAttributesW (a filesystem
// stat) on EVERY call. Several callers live in the per-draw DrawIndexed hot path
// (the camcb96 sidecar/atlas marker gates), so that was thousands of disk stats
// per frame - the dominant cost behind the bad AER framerate. Markers are toggled
// by hand (create/delete a file), so sub-second staleness is invisible: re-stat
// each marker at most ~twice a second and serve a cached bool otherwise.
// All callers run on the present/render thread, so the static cache needs no lock.
bool MarkerEnabled(const wchar_t* fileName) noexcept
{
    if (fileName == nullptr || fileName[0] == L'\0')
    {
        return false;
    }

    struct MarkerCacheEntry { const wchar_t* name; ULONGLONG checkedMs; bool result; };
    static MarkerCacheEntry s_cache[16] = {};
    static int s_cacheCount = 0;
    constexpr ULONGLONG kMarkerTtlMs = 500;
    const ULONGLONG nowMs = GetTickCount64();

    for (int i = 0; i < s_cacheCount; ++i)
    {
        if (s_cache[i].name != nullptr && wcscmp(s_cache[i].name, fileName) == 0)
        {
            if (nowMs - s_cache[i].checkedMs < kMarkerTtlMs)
            {
                return s_cache[i].result;
            }
            s_cache[i].result = MarkerEnabledUncached(fileName);
            s_cache[i].checkedMs = nowMs;
            return s_cache[i].result;
        }
    }

    const bool result = MarkerEnabledUncached(fileName);
    if (s_cacheCount < 16)
    {
        s_cache[s_cacheCount].name = fileName;   // callers pass string literals (static lifetime)
        s_cache[s_cacheCount].checkedMs = nowMs;
        s_cache[s_cacheCount].result = result;
        ++s_cacheCount;
    }
    return result;
}

bool LooksLikeSceneCameraCb96(const ConstantBufferRecord& snapshot) noexcept
{
    if (snapshot.buffer == nullptr || snapshot.byteWidth != 96 || snapshot.lastBytes < 96)
    {
        return false;
    }

    const float* f = reinterpret_cast<const float*>(snapshot.lastData);
    for (int i = 0; i < 24; ++i)
    {
        if (!std::isfinite(f[i]))
        {
            return false;
        }
    }

    const bool row4Identity =
        std::fabs(f[16]) < 0.001f &&
        std::fabs(f[17]) < 0.001f &&
        std::fabs(f[18]) < 0.001f &&
        std::fabs(f[19] - 1.0f) < 0.001f;
    const bool row5Position =
        std::fabs(f[23] - 1.0f) < 0.001f &&
        ((std::fabs(f[20]) > 100.0f) ||
         (std::fabs(f[21]) > 100.0f) ||
         (std::fabs(f[22]) > 100.0f));
    const bool hasProjectionShape =
        (std::fabs(f[0]) + std::fabs(f[1]) + std::fabs(f[4]) + std::fabs(f[5]) +
         std::fabs(f[8]) + std::fabs(f[9]) + std::fabs(f[10])) > 2.0f;

    return row4Identity && row5Position && hasProjectionShape;
}

bool GetVsSlot1CamCb96(ConstantBufferRecord* outSnapshot) noexcept;

void TryNudgeCameraCb96(const char* kind, ID3D11DeviceContext* context,
                        ID3D11DepthStencilView* dsv, const D3D11_VIEWPORT& vp) noexcept
{
    if (kind == nullptr || strcmp(kind, "DrawIndexed") != 0 || context == nullptr || dsv == nullptr)
    {
        return;
    }
    if (vp.Width < 512.0f || vp.Height < 512.0f)
    {
        return;
    }
    if (!MarkerEnabled(L"MELEVR_ENABLE_CAMCB96_NUDGE.txt"))
    {
        return;
    }

    if (!g_camCb96NudgeMarkerLogged.exchange(true, std::memory_order_relaxed))
    {
        LogLine("[CAMCB96_NUDGE] marker enabled; patching VS slot 1 96-byte scene camera packet before DrawIndexed.");
    }

    ConstantBufferRecord snapshot = {};
    {
        std::lock_guard<std::mutex> lock(g_cbMutex);
        ID3D11Buffer* slot1 = g_vsBound[1];
        if (slot1 == nullptr) return;
        for (int r = 0; r < kTrackedConstantBuffers; ++r)
        {
            if (g_cbRecords[r].buffer == slot1)
            {
                snapshot = g_cbRecords[r];
                break;
            }
        }
    }

    if (!LooksLikeSceneCameraCb96(snapshot))
    {
        return;
    }

    unsigned char patched[96] = {};
    memcpy(patched, snapshot.lastData, sizeof(patched));
    float* f = reinterpret_cast<float*>(patched);

    // Deliberately visible proof nudge: squeeze the horizontal clip row and move the
    // translated-world anchor. If final pixels obey this packet, the monitor image moves.
    f[0] *= 0.72f;
    f[1] *= 0.72f;
    f[2] *= 0.72f;
    f[3] *= 0.72f;
    f[20] += 350.0f;

    UpdateSubresourceFn original = g_originalUpdateSubresource;
    if (original != nullptr)
    {
        original(context, snapshot.buffer, 0, nullptr, patched, 0, 0);
    }

    const int n = g_camCb96NudgeLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 80 || (n % 300) == 0)
    {
        char line[512] = {};
        sprintf_s(line,
                  "[CAMCB96_NUDGE] #%d thread=%lu ctx=%p vp=%.0fx%.0f cb=%p hash=0x%08X row0x %.6f->%.6f row5x %.3f->%.3f",
                  n + 1,
                  GetCurrentThreadId(),
                  context,
                  vp.Width,
                  vp.Height,
                  snapshot.buffer,
                  snapshot.lastHash,
                  reinterpret_cast<const float*>(snapshot.lastData)[0],
                  f[0],
                  reinterpret_cast<const float*>(snapshot.lastData)[20],
                  f[20]);
        LogLine(line);
    }
}

bool GetVsSlot1CamCb96(ConstantBufferRecord* outSnapshot) noexcept
{
    if (outSnapshot == nullptr)
    {
        return false;
    }

    ConstantBufferRecord snapshot = {};
    {
        std::lock_guard<std::mutex> lock(g_cbMutex);
        ID3D11Buffer* slot1 = g_vsBound[1];
        if (slot1 == nullptr) return false;
        for (int r = 0; r < kTrackedConstantBuffers; ++r)
        {
            if (g_cbRecords[r].buffer == slot1)
            {
                snapshot = g_cbRecords[r];
                break;
            }
        }
    }

    if (!LooksLikeSceneCameraCb96(snapshot))
    {
        return false;
    }

    *outSnapshot = snapshot;
    return true;
}

bool TryStereoAtlasCameraCb96DrawIndexed(ID3D11DeviceContext* context,
                                         UINT indexCount,
                                         UINT startIndexLocation,
                                         INT baseVertexLocation,
                                         DrawIndexedFn original) noexcept
{
    if (context == nullptr || original == nullptr)
    {
        return false;
    }
    if (!MarkerEnabled(L"MELEVR_ENABLE_CAMCB96_ATLAS.txt"))
    {
        return false;
    }

    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(1, &rtv, &dsv);
    UINT vpCount = 1;
    D3D11_VIEWPORT vp = {};
    context->RSGetViewports(&vpCount, &vp);

    const bool sceneViewport = dsv != nullptr && vpCount > 0 && vp.Width >= 512.0f && vp.Height >= 512.0f;
    if (!sceneViewport)
    {
        if (rtv != nullptr) rtv->Release();
        if (dsv != nullptr) dsv->Release();
        return false;
    }

    ConstantBufferRecord snapshot = {};
    if (!GetVsSlot1CamCb96(&snapshot))
    {
        if (rtv != nullptr) rtv->Release();
        if (dsv != nullptr) dsv->Release();
        return false;
    }

    float rightX = 0.0f;
    float rightY = 0.0f;
    float rightZ = 0.0f;
    if (!MELEVR::RenderHook::GetLastViewRight(&rightX, &rightY, &rightZ))
    {
        if (rtv != nullptr) rtv->Release();
        if (dsv != nullptr) dsv->Release();
        return false;
    }

    const float len = std::sqrt(rightX * rightX + rightY * rightY + rightZ * rightZ);
    if (!(len > 0.01f && len < 10.0f))
    {
        if (rtv != nullptr) rtv->Release();
        if (dsv != nullptr) dsv->Release();
        return false;
    }
    rightX /= len;
    rightY /= len;
    rightZ /= len;

    if (!g_camCb96AtlasMarkerLogged.exchange(true, std::memory_order_relaxed))
    {
        LogLine("[CAMCB96_ATLAS] marker enabled; replacing selected scene DrawIndexed calls with left/right row5-only atlas draws.");
    }

    constexpr float kHalfEyeUU = 1.6f;   // ~1:1 life-size (matches the proven AER halfEye); 60.0 was the debug value
    unsigned char leftBytes[96] = {};
    unsigned char rightBytes[96] = {};
    memcpy(leftBytes, snapshot.lastData, sizeof(leftBytes));
    memcpy(rightBytes, snapshot.lastData, sizeof(rightBytes));
    float* left = reinterpret_cast<float*>(leftBytes);
    float* right = reinterpret_cast<float*>(rightBytes);

    // Row 5 is the translated-world/pre-view-translation packet. Existing FSceneView offset
    // semantics are pvt -= worldOffset for a +right camera move. Use opposite signs per eye.
    left[20] += rightX * kHalfEyeUU;
    left[21] += rightY * kHalfEyeUU;
    left[22] += rightZ * kHalfEyeUU;
    right[20] -= rightX * kHalfEyeUU;
    right[21] -= rightY * kHalfEyeUU;
    right[22] -= rightZ * kHalfEyeUU;

    D3D11_VIEWPORT leftVp = vp;
    D3D11_VIEWPORT rightVp = vp;
    leftVp.Width = vp.Width * 0.5f;
    rightVp.Width = vp.Width * 0.5f;
    rightVp.TopLeftX = vp.TopLeftX + leftVp.Width;

    UpdateSubresourceFn update = g_originalUpdateSubresource;
    if (update == nullptr)
    {
        if (rtv != nullptr) rtv->Release();
        if (dsv != nullptr) dsv->Release();
        return false;
    }

    context->RSSetViewports(1, &leftVp);
    update(context, snapshot.buffer, 0, nullptr, leftBytes, 0, 0);
    original(context, indexCount, startIndexLocation, baseVertexLocation);

    context->RSSetViewports(1, &rightVp);
    update(context, snapshot.buffer, 0, nullptr, rightBytes, 0, 0);
    original(context, indexCount, startIndexLocation, baseVertexLocation);

    update(context, snapshot.buffer, 0, nullptr, snapshot.lastData, 0, 0);
    context->RSSetViewports(1, &vp);

    const int n = g_camCb96AtlasLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 80 || (n % 300) == 0)
    {
        const float* orig = reinterpret_cast<const float*>(snapshot.lastData);
        char line[768] = {};
        sprintf_s(line,
                  "[CAMCB96_ATLAS] #%d thread=%lu ctx=%p vp=%.0fx%.0f cb=%p halfEye=%.1f right=(%.4f,%.4f,%.4f) "
                  "row5L=(%.3f,%.3f,%.3f) row5R=(%.3f,%.3f,%.3f) orig=(%.3f,%.3f,%.3f)",
                  n + 1,
                  GetCurrentThreadId(),
                  context,
                  vp.Width,
                  vp.Height,
                  snapshot.buffer,
                  kHalfEyeUU,
                  rightX,
                  rightY,
                  rightZ,
                  left[20], left[21], left[22],
                  right[20], right[21], right[22],
                  orig[20], orig[21], orig[22]);
        LogLine(line);
    }

    if (rtv != nullptr) rtv->Release();
    if (dsv != nullptr) dsv->Release();
    return true;
}

void RememberMap(ID3D11Resource* resource, void* data, UINT bytes) noexcept
{
    if (resource == nullptr || data == nullptr || bytes == 0) return;
    std::lock_guard<std::mutex> lock(g_cbMutex);
    for (int i = 0; i < kTrackedMaps; ++i)
    {
        if (g_maps[i].resource == nullptr || g_maps[i].resource == resource)
        {
            g_maps[i].resource = resource;
            g_maps[i].data = data;
            g_maps[i].bytes = bytes;
            return;
        }
    }
}

MapRecord TakeMap(ID3D11Resource* resource) noexcept
{
    MapRecord out = {};
    if (resource == nullptr) return out;
    std::lock_guard<std::mutex> lock(g_cbMutex);
    for (int i = 0; i < kTrackedMaps; ++i)
    {
        if (g_maps[i].resource == resource)
        {
            out = g_maps[i];
            g_maps[i] = MapRecord{};
            return out;
        }
    }
    return out;
}

void LogDrawCensus(const char* kind, ID3D11DeviceContext* context) noexcept
{
    if (!DrawTraceEnabled())
    {
        return;
    }

    const int n = g_cbDrawLogs.fetch_add(1, std::memory_order_relaxed);
    if (context == nullptr) return;

    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    context->OMGetRenderTargets(1, &rtv, &dsv);
    UINT vpCount = 1;
    D3D11_VIEWPORT vp = {};
    context->RSGetViewports(&vpCount, &vp);

    if (n < 220)
    {
        std::string slots;
        {
            std::lock_guard<std::mutex> lock(g_cbMutex);
            for (int i = 0; i < 8; ++i)
            {
                if (g_vsBound[i] == nullptr) continue;
                if (!slots.empty()) slots += ",";
                slots += std::to_string(i) + ":" + HexPointer(g_vsBound[i]);
                for (int r = 0; r < kTrackedConstantBuffers; ++r)
                {
                    if (g_cbRecords[r].buffer == g_vsBound[i])
                    {
                        slots += "/b" + std::to_string(g_cbRecords[r].byteWidth) +
                                 "/h" + HexRva(g_cbRecords[r].lastHash);
                        break;
                    }
                }
            }
        }

        char line[1024] = {};
        sprintf_s(line,
                  "[CBDRAW] #%d kind=%s thread=%lu ctx=%p rtv=%p dsv=%p vp=%.0fx%.0f+%.0f,%.0f vsCB=%s",
                  n + 1,
                  kind != nullptr ? kind : "?",
                  GetCurrentThreadId(),
                  context,
                  rtv,
                  dsv,
                  vp.Width,
                  vp.Height,
                  vp.TopLeftX,
                  vp.TopLeftY,
                  slots.c_str());
        LogLine(line);
        LogCameraCb96Candidate(kind, context, rtv, dsv, vp);
    }
    TryNudgeCameraCb96(kind, context, dsv, vp);
    if (rtv != nullptr) rtv->Release();
    if (dsv != nullptr) dsv->Release();
}


bool IsOpenXrDisabledForBoundary() noexcept
{
    wchar_t value[16] = {};
    if (GetEnvironmentVariableW(L"MELEVR_DISABLE_OPENXR", value, static_cast<DWORD>(sizeof(value) / sizeof(value[0]))) > 0)
    {
        return true;
    }

    wchar_t exePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
    {
        return false;
    }

    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash == nullptr)
    {
        return false;
    }
    *(slash + 1) = L'\0';

    wchar_t marker[MAX_PATH] = {};
    if (swprintf_s(marker, L"%sMELEVR_DISABLE_OPENXR.txt", exePath) <= 0)
    {
        return false;
    }
    return GetFileAttributesW(marker) != INVALID_FILE_ATTRIBUTES;
}

template <typename T>
void SafeRelease(T*& p)
{
    if (p != nullptr)
    {
        p->Release();
        p = nullptr;
    }
}

void ReleaseSidecarTargets() noexcept
{
    SafeRelease(g_sidecarSrv);
    SafeRelease(g_sidecarRtv);
    SafeRelease(g_sidecarColor);
    SafeRelease(g_sidecarDsv);
    SafeRelease(g_sidecarDepth);
    SafeRelease(g_sidecarRtvR);
    SafeRelease(g_sidecarColorR);
    SafeRelease(g_sidecarDsvR);
    SafeRelease(g_sidecarDepthR);
    g_sidecarColorDesc = {};
    g_sidecarDepthDesc = {};
    g_sidecarDsvDesc = {};
    g_sidecarValid = false;
}

bool GetTexture2DDescFromView(ID3D11View* view, D3D11_TEXTURE2D_DESC* outDesc) noexcept
{
    if (view == nullptr || outDesc == nullptr)
    {
        return false;
    }

    ID3D11Resource* resource = nullptr;
    view->GetResource(&resource);
    if (resource == nullptr)
    {
        return false;
    }

    ID3D11Texture2D* tex = nullptr;
    const HRESULT hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
    resource->Release();
    if (FAILED(hr) || tex == nullptr)
    {
        return false;
    }

    tex->GetDesc(outDesc);
    tex->Release();
    return true;
}

bool SameSidecarDesc(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) noexcept
{
    return a.Width == b.Width &&
           a.Height == b.Height &&
           a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality;
}

bool SameDsvDesc(const D3D11_DEPTH_STENCIL_VIEW_DESC& a, const D3D11_DEPTH_STENCIL_VIEW_DESC& b) noexcept
{
    if (a.Format != b.Format || a.ViewDimension != b.ViewDimension || a.Flags != b.Flags)
    {
        return false;
    }
    switch (a.ViewDimension)
    {
    case D3D11_DSV_DIMENSION_TEXTURE2D:
        return a.Texture2D.MipSlice == b.Texture2D.MipSlice;
    case D3D11_DSV_DIMENSION_TEXTURE2DMS:
        return true;
    case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
        return a.Texture2DArray.MipSlice == b.Texture2DArray.MipSlice &&
               a.Texture2DArray.FirstArraySlice == b.Texture2DArray.FirstArraySlice &&
               a.Texture2DArray.ArraySize == b.Texture2DArray.ArraySize;
    case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
        return a.Texture2DMSArray.FirstArraySlice == b.Texture2DMSArray.FirstArraySlice &&
               a.Texture2DMSArray.ArraySize == b.Texture2DMSArray.ArraySize;
    default:
        return true;
    }
}

bool EnsureSidecarTargets(const D3D11_TEXTURE2D_DESC& srcColorDesc,
                          const D3D11_TEXTURE2D_DESC& srcDepthDesc,
                          const D3D11_DEPTH_STENCIL_VIEW_DESC& srcDsvDesc) noexcept
{
    if (g_gameDevice == nullptr)
    {
        return false;
    }

    D3D11_TEXTURE2D_DESC colorDesc = srcColorDesc;
    colorDesc.MipLevels = 1;
    colorDesc.ArraySize = 1;
    colorDesc.Usage = D3D11_USAGE_DEFAULT;
    colorDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    colorDesc.CPUAccessFlags = 0;
    colorDesc.MiscFlags = 0;

    D3D11_TEXTURE2D_DESC depthDesc = srcDepthDesc;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    depthDesc.CPUAccessFlags = 0;
    depthDesc.MiscFlags = 0;

    std::lock_guard<std::mutex> lock(g_sidecarMutex);
    if (g_sidecarValid &&
        SameSidecarDesc(g_sidecarColorDesc, colorDesc) &&
        SameSidecarDesc(g_sidecarDepthDesc, depthDesc) &&
        SameDsvDesc(g_sidecarDsvDesc, srcDsvDesc) &&
        g_sidecarColor != nullptr && g_sidecarRtv != nullptr &&
        g_sidecarDepth != nullptr && g_sidecarDsv != nullptr)
    {
        return true;
    }

    ReleaseSidecarTargets();

    HRESULT hr = g_gameDevice->CreateTexture2D(&colorDesc, nullptr, &g_sidecarColor);
    if (FAILED(hr) || g_sidecarColor == nullptr)
    {
        const int n = g_camCb96SidecarCreateFailLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 300) == 0)
        {
            LogLine("[CAMCB96_SIDECAR] failed CreateTexture2D color hr=" + HexHRESULT(hr));
        }
        ReleaseSidecarTargets();
        return false;
    }

    hr = g_gameDevice->CreateRenderTargetView(g_sidecarColor, nullptr, &g_sidecarRtv);
    if (FAILED(hr) || g_sidecarRtv == nullptr)
    {
        const int n = g_camCb96SidecarCreateFailLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 300) == 0)
        {
            LogLine("[CAMCB96_SIDECAR] failed CreateRenderTargetView hr=" + HexHRESULT(hr));
        }
        ReleaseSidecarTargets();
        return false;
    }

    hr = g_gameDevice->CreateShaderResourceView(g_sidecarColor, nullptr, &g_sidecarSrv);
    if (FAILED(hr))
    {
        const int n = g_camCb96SidecarCreateFailLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 300) == 0)
        {
            LogLine("[CAMCB96_SIDECAR] CreateShaderResourceView color failed hr=" + HexHRESULT(hr) + " (continuing; copy path does not need SRV)");
        }
    }

    hr = g_gameDevice->CreateTexture2D(&depthDesc, nullptr, &g_sidecarDepth);
    if (FAILED(hr) || g_sidecarDepth == nullptr)
    {
        const int n = g_camCb96SidecarCreateFailLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 300) == 0)
        {
            LogLine("[CAMCB96_SIDECAR] failed CreateTexture2D depth hr=" + HexHRESULT(hr));
        }
        ReleaseSidecarTargets();
        return false;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC sidecarDsvDesc = srcDsvDesc;
    switch (sidecarDsvDesc.ViewDimension)
    {
    case D3D11_DSV_DIMENSION_TEXTURE2D:
        sidecarDsvDesc.Texture2D.MipSlice = 0;
        break;
    case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
        sidecarDsvDesc.Texture2DArray.MipSlice = 0;
        sidecarDsvDesc.Texture2DArray.FirstArraySlice = 0;
        sidecarDsvDesc.Texture2DArray.ArraySize = 1;
        break;
    case D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY:
        sidecarDsvDesc.Texture2DMSArray.FirstArraySlice = 0;
        sidecarDsvDesc.Texture2DMSArray.ArraySize = 1;
        break;
    default:
        break;
    }

    hr = g_gameDevice->CreateDepthStencilView(g_sidecarDepth, &sidecarDsvDesc, &g_sidecarDsv);
    if (FAILED(hr) || g_sidecarDsv == nullptr)
    {
        const int n = g_camCb96SidecarCreateFailLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 300) == 0)
        {
            char failLine[384] = {};
            sprintf_s(failLine,
                      "[CAMCB96_SIDECAR] failed CreateDepthStencilView hr=%s texFmt=%u dsvFmt=%u dim=%u",
                      HexHRESULT(hr).c_str(),
                      static_cast<unsigned>(depthDesc.Format),
                      static_cast<unsigned>(sidecarDsvDesc.Format),
                      static_cast<unsigned>(sidecarDsvDesc.ViewDimension));
            LogLine(failLine);
        }
        ReleaseSidecarTargets();
        return false;
    }

    // RIGHT-eye targets: same full-size desc, its OWN color+depth (isolated - no atlas, no crop, no squish).
    hr = g_gameDevice->CreateTexture2D(&colorDesc, nullptr, &g_sidecarColorR);
    if (SUCCEEDED(hr) && g_sidecarColorR != nullptr)
        hr = g_gameDevice->CreateRenderTargetView(g_sidecarColorR, nullptr, &g_sidecarRtvR);
    if (SUCCEEDED(hr) && g_sidecarRtvR != nullptr)
        hr = g_gameDevice->CreateTexture2D(&depthDesc, nullptr, &g_sidecarDepthR);
    if (SUCCEEDED(hr) && g_sidecarDepthR != nullptr)
        hr = g_gameDevice->CreateDepthStencilView(g_sidecarDepthR, &sidecarDsvDesc, &g_sidecarDsvR);
    if (FAILED(hr) || g_sidecarColorR == nullptr || g_sidecarRtvR == nullptr ||
        g_sidecarDepthR == nullptr || g_sidecarDsvR == nullptr)
    {
        LogLine("[CAMCB96_SIDECAR] failed to create RIGHT-eye targets hr=" + HexHRESULT(hr));
        ReleaseSidecarTargets();
        return false;
    }

    g_sidecarColorDesc = colorDesc;
    g_sidecarDepthDesc = depthDesc;
    g_sidecarDsvDesc = sidecarDsvDesc;
    g_sidecarValid = true;

    char line[384] = {};
    sprintf_s(line,
              "[CAMCB96_SIDECAR] created isolated atlas color=%ux%u fmt=%u depthFmt=%u sample=%u",
              colorDesc.Width,
              colorDesc.Height,
              static_cast<unsigned>(colorDesc.Format),
              static_cast<unsigned>(sidecarDsvDesc.Format),
              colorDesc.SampleDesc.Count);
    LogLine(line);
    return true;
}

bool TryStereoSidecarCameraCb96DrawIndexed(ID3D11DeviceContext* context,
                                           UINT indexCount,
                                           UINT startIndexLocation,
                                           INT baseVertexLocation,
                                           DrawIndexedFn original) noexcept
{
    if (context == nullptr || original == nullptr)
    {
        return false;
    }
    if (!MarkerEnabled(L"MELEVR_ENABLE_CAMCB96_SIDECAR.txt"))
    {
        return false;
    }

    ID3D11RenderTargetView* originalRtv = nullptr;
    ID3D11DepthStencilView* originalDsv = nullptr;
    context->OMGetRenderTargets(1, &originalRtv, &originalDsv);

    UINT vpCount = 1;
    D3D11_VIEWPORT originalVp = {};
    context->RSGetViewports(&vpCount, &originalVp);

    const bool sceneViewport = originalRtv != nullptr && originalDsv != nullptr &&
                               vpCount > 0 && originalVp.Width >= 512.0f && originalVp.Height >= 512.0f;
    if (!sceneViewport)
    {
        SafeRelease(originalRtv);
        SafeRelease(originalDsv);
        return false;
    }

    D3D11_TEXTURE2D_DESC colorDesc = {};
    D3D11_TEXTURE2D_DESC depthDesc = {};
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    if (!GetTexture2DDescFromView(originalRtv, &colorDesc) ||
        !GetTexture2DDescFromView(originalDsv, &depthDesc))
    {
        SafeRelease(originalRtv);
        SafeRelease(originalDsv);
        return false;
    }
    originalDsv->GetDesc(&dsvDesc);

    // First isolated proof target: only duplicate the final-sized LDR scene-compatible target.
    // The full sidecar pipeline needs per-RT mirrors + SRV remapping; blindly chasing HDR,
    // quarter-size, and LDR targets destroys the isolated atlas every few draws.
    if (colorDesc.Width != 2096 || colorDesc.Height != 2096 || colorDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM)
    {
        const int n = g_camCb96SidecarSkipLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 40 || (n % 1000) == 0)
        {
            char line[256] = {};
            sprintf_s(line,
                      "[CAMCB96_SIDECAR_SKIP] #%d rt=%ux%u fmt=%u",
                      n + 1,
                      colorDesc.Width,
                      colorDesc.Height,
                      static_cast<unsigned>(colorDesc.Format));
            LogLine(line);
        }
        SafeRelease(originalRtv);
        SafeRelease(originalDsv);
        return false;
    }

    if (!EnsureSidecarTargets(colorDesc, depthDesc, dsvDesc))
    {
        SafeRelease(originalRtv);
        SafeRelease(originalDsv);
        return false;
    }

    ConstantBufferRecord snapshot = {};
    if (!GetVsSlot1CamCb96(&snapshot))
    {
        SafeRelease(originalRtv);
        SafeRelease(originalDsv);
        return false;
    }

    float rightX = 0.0f;
    float rightY = 0.0f;
    float rightZ = 0.0f;
    if (!MELEVR::RenderHook::GetLastViewRight(&rightX, &rightY, &rightZ))
    {
        SafeRelease(originalRtv);
        SafeRelease(originalDsv);
        return false;
    }

    const float len = std::sqrt(rightX * rightX + rightY * rightY + rightZ * rightZ);
    if (!(len > 0.01f && len < 10.0f))
    {
        SafeRelease(originalRtv);
        SafeRelease(originalDsv);
        return false;
    }
    rightX /= len;
    rightY /= len;
    rightZ /= len;

    if (!g_camCb96SidecarMarkerLogged.exchange(true, std::memory_order_relaxed))
    {
        LogLine("[CAMCB96_SIDECAR] marker enabled; duplicating selected scene DrawIndexed calls into isolated left/right atlas.");
    }

    constexpr float kHalfEyeUU = 1.6f;   // ~1:1 life-size (matches the proven AER halfEye); 60.0 was the debug value
    unsigned char leftBytes[96] = {};
    unsigned char rightBytes[96] = {};
    memcpy(leftBytes, snapshot.lastData, sizeof(leftBytes));
    memcpy(rightBytes, snapshot.lastData, sizeof(rightBytes));
    float* left = reinterpret_cast<float*>(leftBytes);
    float* right = reinterpret_cast<float*>(rightBytes);

    left[20] += rightX * kHalfEyeUU;
    left[21] += rightY * kHalfEyeUU;
    left[22] += rightZ * kHalfEyeUU;
    right[20] -= rightX * kHalfEyeUU;
    right[21] -= rightY * kHalfEyeUU;
    right[22] -= rightZ * kHalfEyeUU;

    UpdateSubresourceFn update = g_originalUpdateSubresource;
    if (update == nullptr)
    {
        SafeRelease(originalRtv);
        SafeRelease(originalDsv);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_sidecarMutex);
        if (!g_sidecarValid || g_sidecarRtv == nullptr || g_sidecarDsv == nullptr ||
            g_sidecarRtvR == nullptr || g_sidecarDsvR == nullptr)
        {
            SafeRelease(originalRtv);
            SafeRelease(originalDsv);
            return false;
        }

        // Clear BOTH eye targets once per present (the proven present-counter sync).
        const unsigned long long presentId = g_presentCount.load(std::memory_order_relaxed);
        const unsigned long long oldClear = g_camCb96SidecarLastClearPresent.load(std::memory_order_relaxed);
        if (oldClear != presentId)
        {
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            context->ClearRenderTargetView(g_sidecarRtv, black);
            context->ClearDepthStencilView(g_sidecarDsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            context->ClearRenderTargetView(g_sidecarRtvR, black);
            context->ClearDepthStencilView(g_sidecarDsvR, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
            g_camCb96SidecarLastClearPresent.store(presentId, std::memory_order_relaxed);
        }

        // FULL-viewport render of each eye into its OWN target - no half-split, no squish, no crop.
        context->RSSetViewports(1, &originalVp);

        context->OMSetRenderTargets(1, &g_sidecarRtv, g_sidecarDsv);    // LEFT eye
        update(context, snapshot.buffer, 0, nullptr, leftBytes, 0, 0);
        original(context, indexCount, startIndexLocation, baseVertexLocation);

        context->OMSetRenderTargets(1, &g_sidecarRtvR, g_sidecarDsvR);  // RIGHT eye
        update(context, snapshot.buffer, 0, nullptr, rightBytes, 0, 0);
        original(context, indexCount, startIndexLocation, baseVertexLocation);
    }

    update(context, snapshot.buffer, 0, nullptr, snapshot.lastData, 0, 0);
    context->OMSetRenderTargets(1, &originalRtv, originalDsv);
    context->RSSetViewports(1, &originalVp);

    const int n = g_camCb96SidecarLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 80 || (n % 300) == 0)
    {
        char line[768] = {};
        sprintf_s(line,
                  "[CAMCB96_SIDECAR] #%d thread=%lu ctx=%p vp=%.0fx%.0f cb=%p halfEye=%.1f right=(%.4f,%.4f,%.4f) "
                  "row5L=(%.3f,%.3f,%.3f) row5R=(%.3f,%.3f,%.3f)",
                  n + 1,
                  GetCurrentThreadId(),
                  context,
                  originalVp.Width,
                  originalVp.Height,
                  snapshot.buffer,
                  kHalfEyeUU,
                  rightX,
                  rightY,
                  rightZ,
                  left[20], left[21], left[22],
                  right[20], right[21], right[22]);
        LogLine(line);
    }

    SafeRelease(originalRtv);
    SafeRelease(originalDsv);
    return true;
}

// --- DIBR depth viz: render the captured depth/warp onto the flat backbuffer, so it can be
//     can confirm on the MONITOR that the depth is real + geometrically sensible before reprojecting from it.
//     In headset mode this must not paint the game backbuffer before XR copies it into the eyes. ---
ID3D11VertexShader* g_depthVizVs = nullptr;
ID3D11PixelShader* g_depthVizPs = nullptr;
ID3D11SamplerState* g_depthVizSampler = nullptr;
ID3D11RasterizerState* g_depthVizRaster = nullptr;
ID3D11DepthStencilState* g_depthVizDepthState = nullptr;
ID3D11BlendState* g_depthVizBlend = nullptr;
bool g_depthVizReady = false;
bool g_depthVizTried = false;
std::atomic<int> g_depthVizLogs{0};
ID3D11Texture2D* g_dibrColorCopy = nullptr;       // SRV-able copy of the game's color frame (warp reads this)
ID3D11ShaderResourceView* g_dibrColorSrv = nullptr;
UINT g_dibrColorW = 0;
UINT g_dibrColorH = 0;
ID3D11Texture2D* g_dibrWarpedTex = nullptr;        // full-frame synthesized right-eye (CopyResource'd to the OpenXR right eye)
ID3D11RenderTargetView* g_dibrWarpedRtv = nullptr;
UINT g_dibrWarpedW = 0;
UINT g_dibrWarpedH = 0;
// Grid-warp ghost fix (ported from 1ef5d8d): a depth-displaced vertex grid + a depth test so NEAR geometry
// occludes FAR, instead of the fullscreen shift that smears foreground over background (= ghost). Self-contained:
// its own shaders + a 4-float cbuffer, so it does not disturb the gold's fullscreen warp (kept for the left path).
ID3D11DepthStencilState* g_dibrGridDepthState = nullptr; // GREATER (reversed-Z): nearest (highest d) wins
// Tear + inpaint (ported from the parked 76f38c5 experiment - written but never tested). The TEAR (GS) culls
// triangles stretched across a disocclusion gap (= the comb); the INPAINT pass fills the resulting holes from
// the farthest horizontal neighbour (= the background), so it never drags the foreground in (= no ghost).
ID3D11ShaderResourceView* g_dibrWarpedSrv = nullptr;     // warped eye (color + coverage/depth in alpha) = inpaint input
ID3D11Texture2D* g_dibrLeftTex = nullptr;          // framed left eye when source-scale != 1.0
ID3D11RenderTargetView* g_dibrLeftRtv = nullptr;
UINT g_dibrLeftW = 0;
UINT g_dibrLeftH = 0;
// [RELIEF M1] the relief warp target: the SBS backbuffer re-rendered with a small per-half depth-driven
// pop added on top of the real stereo pair. Backbuffer-sized; CopySbsHalves reads it into the eyes.
ID3D11Texture2D* g_reliefTex = nullptr;
ID3D11RenderTargetView* g_reliefRtv = nullptr;
UINT g_reliefW = 0;
UINT g_reliefH = 0;
ID3D11PixelShader* g_reliefPs = nullptr;           // SBS-aware gather (per-half sign + seam clamp)
ID3D11Buffer* g_reliefParamsCb = nullptr;          // {strength, convergence, signBase, curve}
std::atomic<float> g_reliefStrength{0.0f};         // 0 = off (identical to plain stereo); UV-shift scale
std::atomic<float> g_reliefConvergence{0.985f};    // depth that stays put (zero added pop) - usually the subject
std::atomic<float> g_reliefSign{1.0f};             // +1 / -1 flip if pop sinks in instead of out
std::atomic<float> g_reliefCurve{1.0f};            // depth response shaping (1 = linear)
std::atomic<float> g_reliefEdge{0.5f};             // silhouette-damp strength (0 = raw gather, 1 = strong anti-ghost)
std::atomic<float> g_reliefNearFreeze{1.02f};      // depth nearer than this is frozen (no warp) -> protects Shepard; >=1 = off
std::atomic<float> g_reliefDarkStrength{0.0f};     // [DEPTH DARKEN] background-side edge darkening (0 = off)
std::atomic<float> g_reliefDarkRadius{6.0f};       // [DEPTH DARKEN] depth-unsharp neighborhood radius in pixels
std::atomic<float> g_reliefUnsharpStrength{0.0f};  // [UNSHARP POP] local depth-contrast warp term (0 = off)
std::atomic<float> g_reliefUnsharpRadius{24.0f};   // [UNSHARP POP] neighborhood radius in pixels
std::atomic<bool>  g_depthVizAutoFit{true};        // [RELIEF] greyscale window auto-fits to the live range (off = manual)
// Live depth-warp tunables, set from the Insert menu (reversed-Z scene depth ~[0.94,1.0]).
std::atomic<float> g_dibrGain{0.50f};              // depth strength
std::atomic<float> g_dibrConvergence{0.985f};      // depth that fuses at zero disparity
std::atomic<float> g_dibrSign{1.0f};               // +1 / -1 to flip depth direction
std::atomic<float> g_dibrNearCut{0.965f};          // lower depth values are close foreground in the captured source
std::atomic<float> g_dibrNearScale{85.0f};         // how quickly close foreground freezes instead of warping
std::atomic<float> g_dibrEdgeScale{120.0f};        // same-depth edge rejection
std::atomic<float> g_dibrCrossScale{140.0f};       // source/probe depth mismatch rejection
std::atomic<float> g_dibrLeakScale{400.0f};        // foreground/background pull rejection
std::atomic<float> g_dibrSilhouetteScale{180.0f};  // rejects pulls that cross a closer foreground layer
std::atomic<float> g_dibrSourceScale{1.0f};         // VR-only source framing: >1 shrinks/frames the source image
// Enhancements > "Depth map": gates the depth CAPTURE + the greyscale viz, decoupled from DIBR stereo (which is
// off). This is the foundation for combining AER with the depth map (depth-warp / composite). Default off.
// Two toggles: g_depthMapEnabled is the MASTER (runs the capture pipeline = the enhancement on/off, for
// comparing); g_depthVizShow gates the greyscale overlay (the visible proof), only meaningful when master on.
std::atomic<bool> g_depthMapEnabled{false};
std::atomic<bool> g_depthVizShow{false};
// Greyscale depth-map viz tuning (pushed from the menu each frame). Reversed-Z: near=high d, far=low d.
std::atomic<float> g_depthNear{1.0f};     // depth value -> white
std::atomic<float> g_depthFar{0.95f};     // depth value -> black
std::atomic<float> g_depthFlip{0.0f};     // >0.5 inverts
std::atomic<float> g_depthGamma{1.0f};    // <1 lifts far, >1 emphasises near
// Live depth-range readout (filled by the continuous probe while the map is on): center=subject, corners=walls.
std::atomic<float> g_probeCenter{0.0f};
std::atomic<float> g_probeTL{0.0f};
std::atomic<float> g_probeBR{0.0f};
std::atomic<float> g_probeTR{0.0f};
// [RELIEF M0] quarter-center taps: in SBS stereo these land on the per-eye view centers (the true center
// is the seam between the halves); in mono/AER they're just left/right scene samples. Diagnostic-only.
std::atomic<float> g_probeLC{0.0f};
std::atomic<float> g_probeRC{0.0f};
// [RELIEF M0] cumulative depth-copy counter -> capture rate (copies/sec) in the [RELIEF] log + menu readout.
std::atomic<uint32_t> g_depthCopyCount{0};
// Greyscale-map shader + its own cbuffer (separate from the warp tunables).
ID3D11PixelShader* g_depthMapPs = nullptr;
ID3D11Buffer*      g_depthMapCb = nullptr;
// Far-only GATHER warp (preserve-near combine): near pixels sample their own UV (untouched - Shepard pristine),
// far pixels sample a depth-shifted UV. No grid/tear/inpaint, so silhouettes are never eaten.
ID3D11Buffer* g_dibrParamsCb = nullptr;

void FillDibrParamBuffer(float* p, float eyeScale) noexcept
{
    if (p == nullptr) return;
    p[0] = g_dibrGain.load(std::memory_order_relaxed);
    p[1] = g_dibrConvergence.load(std::memory_order_relaxed);
    p[2] = g_dibrSign.load(std::memory_order_relaxed);
    p[3] = g_dibrNearCut.load(std::memory_order_relaxed);
    p[4] = g_dibrNearScale.load(std::memory_order_relaxed);
    p[5] = g_dibrEdgeScale.load(std::memory_order_relaxed);
    p[6] = g_dibrCrossScale.load(std::memory_order_relaxed);
    p[7] = g_dibrLeakScale.load(std::memory_order_relaxed);
    p[8] = 0.0f;   // (was labMode; lab shader removed, cb layout kept)
    p[9] = g_dibrSilhouetteScale.load(std::memory_order_relaxed);
    p[10] = g_dibrSourceScale.load(std::memory_order_relaxed);
    p[11] = eyeScale;
}

// (Re)create the SRV-able color copy to match the backbuffer (so the warp can sample the finished frame
// while the monitor/debug result renders back into that same backbuffer).
bool EnsureColorCopy(const D3D11_TEXTURE2D_DESC& bbDesc) noexcept
{
    if (g_gameDevice == nullptr) return false;
    if (g_dibrColorCopy != nullptr && g_dibrColorW == bbDesc.Width && g_dibrColorH == bbDesc.Height) return true;
    SafeRelease(g_dibrColorSrv);
    SafeRelease(g_dibrColorCopy);
    D3D11_TEXTURE2D_DESC cd = bbDesc;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.CPUAccessFlags = 0;
    cd.MiscFlags = 0;
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    HRESULT hr = g_gameDevice->CreateTexture2D(&cd, nullptr, &g_dibrColorCopy);
    if (FAILED(hr) || g_dibrColorCopy == nullptr) { LogLine("[DIBR_VIZ] color copy create failed hr=" + HexHRESULT(hr)); return false; }
    hr = g_gameDevice->CreateShaderResourceView(g_dibrColorCopy, nullptr, &g_dibrColorSrv);
    if (FAILED(hr) || g_dibrColorSrv == nullptr) { LogLine("[DIBR_VIZ] color srv create failed hr=" + HexHRESULT(hr)); SafeRelease(g_dibrColorCopy); return false; }
    g_dibrColorW = bbDesc.Width; g_dibrColorH = bbDesc.Height;
    LogLine("[DIBR_VIZ] color copy created " + std::to_string(bbDesc.Width) + "x" + std::to_string(bbDesc.Height));
    return true;
}

// (Re)create the synthesized-right-eye render target, same format/size as the backbuffer so it CopyResource's
// straight into the OpenXR right-eye image (exactly like the game backbuffer does for the left eye).
bool EnsureWarpedTex(const D3D11_TEXTURE2D_DESC& bbDesc) noexcept
{
    if (g_gameDevice == nullptr) return false;
    if (g_dibrWarpedTex != nullptr && g_dibrWarpedW == bbDesc.Width && g_dibrWarpedH == bbDesc.Height) return true;
    SafeRelease(g_dibrWarpedRtv);
    SafeRelease(g_dibrWarpedTex);
    D3D11_TEXTURE2D_DESC cd = bbDesc;
    cd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;   // warp INTO it; also sampled by the inpaint pass
    cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = 0; cd.MiscFlags = 0; cd.MipLevels = 1; cd.ArraySize = 1;
    HRESULT hr = g_gameDevice->CreateTexture2D(&cd, nullptr, &g_dibrWarpedTex);
    if (FAILED(hr) || g_dibrWarpedTex == nullptr) { LogLine("[DIBR_VIZ] warped tex create failed hr=" + HexHRESULT(hr)); return false; }
    hr = g_gameDevice->CreateRenderTargetView(g_dibrWarpedTex, nullptr, &g_dibrWarpedRtv);
    if (FAILED(hr) || g_dibrWarpedRtv == nullptr) { LogLine("[DIBR_VIZ] warped rtv create failed hr=" + HexHRESULT(hr)); SafeRelease(g_dibrWarpedTex); return false; }
    SafeRelease(g_dibrWarpedSrv);
    g_gameDevice->CreateShaderResourceView(g_dibrWarpedTex, nullptr, &g_dibrWarpedSrv);
    g_dibrWarpedW = bbDesc.Width; g_dibrWarpedH = bbDesc.Height;
    LogLine("[DIBR_VIZ] warped eye tex created " + std::to_string(bbDesc.Width) + "x" + std::to_string(bbDesc.Height));
    return true;
}

bool EnsureLeftDibrTex(const D3D11_TEXTURE2D_DESC& bbDesc) noexcept
{
    if (g_dibrLeftTex != nullptr && g_dibrLeftW == bbDesc.Width && g_dibrLeftH == bbDesc.Height) return true;
    SafeRelease(g_dibrLeftRtv); SafeRelease(g_dibrLeftTex);
    D3D11_TEXTURE2D_DESC cd = bbDesc;
    cd.BindFlags = D3D11_BIND_RENDER_TARGET;
    cd.CPUAccessFlags = 0;
    cd.MiscFlags = 0;
    cd.Usage = D3D11_USAGE_DEFAULT;
    HRESULT hr = g_gameDevice->CreateTexture2D(&cd, nullptr, &g_dibrLeftTex);
    if (FAILED(hr) || g_dibrLeftTex == nullptr) { LogLine("[DIBR_VIZ] left tex create failed hr=" + HexHRESULT(hr)); return false; }
    hr = g_gameDevice->CreateRenderTargetView(g_dibrLeftTex, nullptr, &g_dibrLeftRtv);
    if (FAILED(hr) || g_dibrLeftRtv == nullptr) { LogLine("[DIBR_VIZ] left rtv create failed hr=" + HexHRESULT(hr)); SafeRelease(g_dibrLeftTex); return false; }
    g_dibrLeftW = bbDesc.Width; g_dibrLeftH = bbDesc.Height;
    LogLine("[DIBR_VIZ] framed left eye tex created " + std::to_string(bbDesc.Width) + "x" + std::to_string(bbDesc.Height));
    return true;
}

// [RELIEF M1] (re)create the relief warp target - same format/size as the SBS backbuffer, so CopySbsHalves
// reads warped halves into the eyes exactly like it reads the raw backbuffer.
bool EnsureReliefTex(const D3D11_TEXTURE2D_DESC& bbDesc) noexcept
{
    if (g_gameDevice == nullptr) return false;
    if (g_reliefTex != nullptr && g_reliefW == bbDesc.Width && g_reliefH == bbDesc.Height) return true;
    SafeRelease(g_reliefRtv); SafeRelease(g_reliefTex);
    D3D11_TEXTURE2D_DESC cd = bbDesc;
    cd.BindFlags = D3D11_BIND_RENDER_TARGET;
    cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = 0; cd.MiscFlags = 0; cd.MipLevels = 1; cd.ArraySize = 1;
    HRESULT hr = g_gameDevice->CreateTexture2D(&cd, nullptr, &g_reliefTex);
    if (FAILED(hr) || g_reliefTex == nullptr) { LogLine("[RELIEF] tex create failed hr=" + HexHRESULT(hr)); return false; }
    hr = g_gameDevice->CreateRenderTargetView(g_reliefTex, nullptr, &g_reliefRtv);
    if (FAILED(hr) || g_reliefRtv == nullptr) { LogLine("[RELIEF] rtv create failed hr=" + HexHRESULT(hr)); SafeRelease(g_reliefTex); return false; }
    g_reliefW = bbDesc.Width; g_reliefH = bbDesc.Height;
    LogLine("[RELIEF] warp target created " + std::to_string(bbDesc.Width) + "x" + std::to_string(bbDesc.Height));
    return true;
}

bool CompileDepthShader(const char* src, const char* entry, const char* profile, ID3DBlob** blob) noexcept
{
    if (blob == nullptr) return false;
    *blob = nullptr;
    HMODULE comp = LoadLibraryW(L"d3dcompiler_47.dll");
    if (comp == nullptr) comp = LoadLibraryW(L"d3dcompiler_43.dll");
    if (comp == nullptr) { LogLine("[DIBR_VIZ] no d3dcompiler dll"); return false; }
    using D3DCompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*,
                                          LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    auto compile = reinterpret_cast<D3DCompileFn>(GetProcAddress(comp, "D3DCompile"));
    if (compile == nullptr) { FreeLibrary(comp); LogLine("[DIBR_VIZ] no D3DCompile export"); return false; }
    ID3DBlob* err = nullptr;
    const HRESULT hr = compile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, profile, 0, 0, blob, &err);
    if (FAILED(hr))
    {
        if (err) { LogLine(std::string("[DIBR_VIZ] compile err: ") + reinterpret_cast<const char*>(err->GetBufferPointer())); err->Release(); }
        FreeLibrary(comp);
        return false;
    }
    if (err) err->Release();
    FreeLibrary(comp);
    return true;
}

bool EnsureDepthVizShaders() noexcept
{
    if (g_depthVizReady) return true;
    if (g_depthVizTried) return false;
    g_depthVizTried = true;
    if (g_gameDevice == nullptr) return false;
    // DIBR warp, shown side-by-side on the monitor so parallax can be judged before the headset:
    //   LEFT half  = the game's real frame (reference)
    //   RIGHT half = the synthesized eye - each pixel sampled at a horizontal offset set by its depth
    //                (near pixels shift most). gain/sign are tunable here; iterate by rebuild.
    const char* src =
        "Texture2D colorTex : register(t0);\n"
        "Texture2D depthTex : register(t1);\n"
        "SamplerState s0 : register(s0);\n"
        "cbuffer DibrParams : register(b0) { float gain; float convergence; float sgn; float nearCut; float nearScale; float edgeScale; float crossScale; float leakScale; float labMode; float silhouetteScale; float sourceScale; float eyeScale; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "VSOut VSMain(uint id : SV_VertexID){ float2 p=float2((id==2)?3.0:-1.0,(id==1)?3.0:-1.0);\n"
        "  VSOut o; o.pos=float4(p,0.0,1.0); o.uv=float2((p.x+1.0)*0.5, 1.0-((p.y+1.0)*0.5)); return o; }\n"
        "float2 SourceUv(float2 uv){ return saturate((uv - 0.5) / max(sourceScale, 0.01) + 0.5); }\n"
        "float DepthEdge(float2 uv, float d){\n"
        "  float2 t=float2(1.0/1024.0,1.0/1024.0);\n"
        "  float dl=depthTex.Sample(s0, float2(saturate(uv.x-t.x),uv.y)).r;\n"
        "  float dr=depthTex.Sample(s0, float2(saturate(uv.x+t.x),uv.y)).r;\n"
        "  float du=depthTex.Sample(s0, float2(uv.x,saturate(uv.y-t.y))).r;\n"
        "  float dd=depthTex.Sample(s0, float2(uv.x,saturate(uv.y+t.y))).r;\n"
        "  return saturate(max(max(abs(d-dl),abs(d-dr)),max(abs(d-du),abs(d-dd))) * edgeScale);\n"
        "}\n"
        "float SilhouetteCross(float2 uv, float2 warpedUv, float d){\n"
        "  float occ=0.0;\n"
        "  float2 u1=lerp(uv, warpedUv, 0.25);\n"
        "  float2 u2=lerp(uv, warpedUv, 0.50);\n"
        "  float2 u3=lerp(uv, warpedUv, 0.75);\n"
        "  float2 u4=warpedUv;\n"
        "  occ=max(occ, saturate((d - depthTex.Sample(s0, u1).r - 0.0010) * silhouetteScale));\n"
        "  occ=max(occ, saturate((d - depthTex.Sample(s0, u2).r - 0.0010) * silhouetteScale));\n"
        "  occ=max(occ, saturate((d - depthTex.Sample(s0, u3).r - 0.0010) * silhouetteScale));\n"
        "  occ=max(occ, saturate((d - depthTex.Sample(s0, u4).r - 0.0010) * silhouetteScale));\n"
        "  return occ;\n"
        "}\n"
        "float4 PSMain(VSOut i):SV_Target{\n"
        "  float2 baseUv = SourceUv(i.uv);\n"
        "  float d = depthTex.Sample(s0, baseUv).r;\n"
        "  float disparity = sgn * gain * eyeScale * (convergence - d);\n"   // reversed-Z (near=high d); (conv-d) = correct depth direction by default
        "  float edge = DepthEdge(baseUv, d);\n"
        "  float2 probeUv = SourceUv(float2(saturate(i.uv.x + disparity), i.uv.y));\n"
        "  float probeD = depthTex.Sample(s0, probeUv).r;\n"
        "  float cross = saturate(abs(probeD - d) * crossScale);\n"
        "  float nearFg = saturate((nearCut - d) * nearScale);\n"
        "  float fgLeak = saturate((probeD - d - 0.0005) * leakScale);\n"
        "  float silhouette = SilhouetteCross(baseUv, probeUv, d);\n"
        "  float guard = 1.0 - saturate(max(max(max(edge, cross), fgLeak), silhouette));\n"
        "  guard *= (1.0 - nearFg);\n"
        "  guard = guard * guard;\n"
        "  float2 warpedUv = SourceUv(float2(saturate(i.uv.x + disparity * guard), i.uv.y));\n"
        "  return colorTex.Sample(s0, warpedUv);\n"
        "}\n";
    // Enhancements > Depth map: plain greyscale, with a tunable [far..near] window + flip + gamma so the tiny
    // reversed-Z range becomes a readable gradient. Reuses VSMain (same VSOut signature).
    const char* mapSrc =
        "Texture2D depthTex : register(t1);\n"
        "SamplerState s0 : register(s0);\n"
        "cbuffer DepthMapParams : register(b0) { float nearD; float farD; float flipD; float gammaD; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        "float4 PSMap(VSOut i):SV_Target{\n"
        "  float d = depthTex.Sample(s0, i.uv).r;\n"
        "  float g = saturate((d - farD) / max(nearD - farD, 1e-5));\n"
        "  if (flipD > 0.5) g = 1.0 - g;\n"
        "  g = pow(saturate(g), max(gammaD, 0.05));\n"
        "  return float4(g, g, g, 1.0);\n"
        "}\n";
    // [RELIEF M1] SBS-aware relief gather: adds a small per-eye depth-driven pop ON TOP of the real stereo
    // pair. Left half [0,0.5] and right half [0.5,1] shift in OPPOSITE directions (more binocular disparity
    // for near objects = pop), CLAMPED inside their own half so the gather never crosses the seam. Non-
    // physical extra disparity -> pop WITHOUT the world-shrink that raising the real eye separation causes.
    const char* reliefSrc =
        "Texture2D colorTex : register(t0);\n"
        "Texture2D depthTex : register(t1);\n"
        "SamplerState s0 : register(s0);\n"
        "cbuffer ReliefParams : register(b0) { float strength; float convergence; float signBase; float curve; float edgeGuard; float nearFreeze; float mode; float eyeSign; float darkStrength; float darkRadius; float texelX; float texelY; float unsharpStrength; float unsharpRadius; float pad0; float pad1; };\n"
        "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
        // 8-tap ring average of depth around uv (4 axis + 4 diagonal at radius r in UV units). Horizontal
        // taps clamp to [lo,hi] so an SBS half never reads the other eye's depth across the seam.
        "float RingAvgD(float2 uv, float2 r, float lo, float hi){\n"
        "  float a = depthTex.Sample(s0, float2(clamp(uv.x - r.x, lo, hi), uv.y)).r;\n"
        "  a += depthTex.Sample(s0, float2(clamp(uv.x + r.x, lo, hi), uv.y)).r;\n"
        "  a += depthTex.Sample(s0, float2(uv.x, uv.y - r.y)).r;\n"
        "  a += depthTex.Sample(s0, float2(uv.x, uv.y + r.y)).r;\n"
        "  float2 rd = r * 0.7071;\n"
        "  a += depthTex.Sample(s0, float2(clamp(uv.x - rd.x, lo, hi), uv.y - rd.y)).r;\n"
        "  a += depthTex.Sample(s0, float2(clamp(uv.x + rd.x, lo, hi), uv.y - rd.y)).r;\n"
        "  a += depthTex.Sample(s0, float2(clamp(uv.x - rd.x, lo, hi), uv.y + rd.y)).r;\n"
        "  a += depthTex.Sample(s0, float2(clamp(uv.x + rd.x, lo, hi), uv.y + rd.y)).r;\n"
        "  return a * 0.125;\n"
        "}\n"
        // 4-tap (axis-only) variant for the WIDE unsharp ring: the unsharp average is a low-frequency
        // term, so 4 taps read the same perceptually at half the fetch cost ([RELIEF_PERF], 6144-class res).
        "float RingAvgD4(float2 uv, float2 r, float lo, float hi){\n"
        "  float a = depthTex.Sample(s0, float2(clamp(uv.x - r.x, lo, hi), uv.y)).r;\n"
        "  a += depthTex.Sample(s0, float2(clamp(uv.x + r.x, lo, hi), uv.y)).r;\n"
        "  a += depthTex.Sample(s0, float2(uv.x, uv.y - r.y)).r;\n"
        "  a += depthTex.Sample(s0, float2(uv.x, uv.y + r.y)).r;\n"
        "  return a * 0.25;\n"
        "}\n"
        "float4 PSRelief(VSOut i):SV_Target{\n"
        "  float2 uv = i.uv;\n"
        "  bool full = mode > 0.5;\n"                           // full = AER (one full-frame eye); else SBS stereo
        "  bool isLeft = uv.x < 0.5;\n"
        "  float lo = full ? 0.0 : (isLeft ? 0.0 : 0.5);\n"     // eye-half bounds, needed by every ring tap below
        "  float hi = full ? 1.0 : (isLeft ? 0.5 : 1.0);\n"
        "  float d = depthTex.Sample(s0, uv).r;\n"
        "  float md = sign(convergence - d) * pow(abs(convergence - d), max(curve, 0.05));\n"  // shaped depth delta
        // per-eye sign: AER = the whole frame is one eye (eyeSign); stereo = each half opposite.
        "  float sgn = full ? (eyeSign * signBase) : (isLeft ? -signBase : signBase);\n"
        "  float disp = sgn * strength * md;\n"
        // [UNSHARP POP] (Didyk 2011): local depth-contrast term - disparity from (neighborhood avg - d)
        // instead of (convergence - d). Zero-mean, so global range (= world scale) holds while local
        // relief amplifies. (avg - d) uses the same sign convention as (convergence - d): nearer than
        // the surround shifts the same direction as nearer than convergence. Raw depth is affine in 1/z,
        // so this difference is already disparity-space.
        "  if (unsharpStrength > 0.0001) {\n"
        "    float avgW = RingAvgD4(uv, float2(texelX, texelY) * unsharpRadius, lo, hi);\n"
        "    disp += sgn * unsharpStrength * (avgW - d);\n"
        "  }\n"
        // NEAR FREEZE (Shepard/weapon): reversed-Z near = HIGH d. Anything nearer than nearFreeze gets no
        // warp (stays flat, zero disparity) -> the foreground subject can't ghost; only the environment
        // behind it takes the pop. nearFreeze >= 1.0 disables it (nothing is nearer than full-near).
        // Applies to the COMBINED displacement (base + unsharp) so the subject is protected from both.
        "  float freeze = 1.0 - saturate((d - nearFreeze) * 80.0);\n"   // 0 for near (frozen), 1 for far (warped)
        "  disp *= freeze;\n"
        // ghost guard: a plain gather drags foreground color into background disocclusions at silhouettes.
        // Probe the depth at the shifted spot; if it jumps (crossed a silhouette) damp the shift there.
        // [RELIEF_PERF] gated: no displacement (or guard off) = skip the probe tap entirely.
        "  if (edgeGuard > 0.0001 && abs(disp) > 0.00002) {\n"
        "    float uProbe = clamp(uv.x + disp, lo + 0.0006, hi - 0.0006);\n"
        "    float dProbe = depthTex.Sample(s0, float2(uProbe, uv.y)).r;\n"
        "    float edge = saturate(abs(dProbe - d) * 90.0 * edgeGuard);\n"
        "    disp *= (1.0 - edge);\n"
        "  }\n"
        "  float u = clamp(uv.x + disp, lo + 0.0006, hi - 0.0006);\n"   // never sample across the seam
        "  float4 col = colorTex.Sample(s0, float2(u, uv.y));\n"
        // [DEPTH DARKEN] (Luft et al. 2006, depth unsharp masking): compare this pixel's depth to a small
        // neighborhood average; where the pixel sits FARTHER than its surround (reversed-Z: d below avg),
        // darken it. Result = a soft shadow hugging the background side of every silhouette - a monocular
        // solidity/ordering cue that spends zero disparity and is identical in both eyes (no rivalry).
        // Taps clamp inside the eye's own half so the SBS seam cannot bleed depth across eyes. The raw
        // depth value is affine in 1/z (any perspective projection: d = A + B/z), so this difference is
        // already a disparity-space unsharp mask - no linearization needed. Scale 150 saturates at a raw
        // delta of ~0.0067, sized to LE1's packed-depth silhouette steps (same family as the 80/90 above).
        "  if (darkStrength > 0.0001) {\n"
        "    float avg = RingAvgD(uv, float2(texelX, texelY) * darkRadius, lo, hi);\n"
        "    float dark = saturate((avg - d) * 150.0) * darkStrength;\n"
        "    col.rgb *= (1.0 - dark);\n"
        "  }\n"
        "  return col;\n"
        "}\n";
    ID3DBlob* vs = nullptr; ID3DBlob* ps = nullptr; ID3DBlob* mapPs = nullptr; ID3DBlob* reliefPs = nullptr;
    if (!CompileDepthShader(src, "VSMain", "vs_4_0", &vs) ||
        !CompileDepthShader(src, "PSMain", "ps_4_0", &ps) ||
        !CompileDepthShader(mapSrc, "PSMap", "ps_4_0", &mapPs) ||
        !CompileDepthShader(reliefSrc, "PSRelief", "ps_4_0", &reliefPs))
    { if (vs) vs->Release(); if (ps) ps->Release(); if (mapPs) mapPs->Release(); if (reliefPs) reliefPs->Release(); return false; }
    HRESULT hr = g_gameDevice->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_depthVizVs);
    if (SUCCEEDED(hr)) hr = g_gameDevice->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &g_depthVizPs);
    if (SUCCEEDED(hr)) hr = g_gameDevice->CreatePixelShader(mapPs->GetBufferPointer(), mapPs->GetBufferSize(), nullptr, &g_depthMapPs);
    if (SUCCEEDED(hr)) hr = g_gameDevice->CreatePixelShader(reliefPs->GetBufferPointer(), reliefPs->GetBufferSize(), nullptr, &g_reliefPs);
    vs->Release(); ps->Release(); mapPs->Release(); reliefPs->Release();
    if (FAILED(hr) || g_depthVizVs == nullptr || g_depthVizPs == nullptr || g_depthMapPs == nullptr || g_reliefPs == nullptr) { LogLine("[DIBR_VIZ] shader create failed"); return false; }
    D3D11_SAMPLER_DESC sd = {}; sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_gameDevice->CreateSamplerState(&sd, &g_depthVizSampler);
    D3D11_RASTERIZER_DESC rd = {}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
    g_gameDevice->CreateRasterizerState(&rd, &g_depthVizRaster);
    D3D11_DEPTH_STENCIL_DESC dd = {}; dd.DepthEnable = FALSE; dd.StencilEnable = FALSE;
    g_gameDevice->CreateDepthStencilState(&dd, &g_depthVizDepthState);
    D3D11_BLEND_DESC bd = {}; bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_gameDevice->CreateBlendState(&bd, &g_depthVizBlend);
    D3D11_BUFFER_DESC cbd = {}; cbd.ByteWidth = 48; cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_gameDevice->CreateBuffer(&cbd, nullptr, &g_dibrParamsCb);   // warp tunables
    D3D11_BUFFER_DESC mcbd = {}; mcbd.ByteWidth = 16; mcbd.Usage = D3D11_USAGE_DYNAMIC;
    mcbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; mcbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_gameDevice->CreateBuffer(&mcbd, nullptr, &g_depthMapCb);     // greyscale-map near/far/flip/gamma
    D3D11_BUFFER_DESC rcbd = {}; rcbd.ByteWidth = 64; rcbd.Usage = D3D11_USAGE_DYNAMIC;   // 16 floats (16-byte aligned)
    rcbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; rcbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_gameDevice->CreateBuffer(&rcbd, nullptr, &g_reliefParamsCb);  // [RELIEF M1] {strength..eyeSign} + [DEPTH DARKEN] + [UNSHARP POP] + pad2
    if (g_depthVizSampler == nullptr || g_depthVizRaster == nullptr || g_depthVizDepthState == nullptr ||
        g_depthMapCb == nullptr || g_reliefParamsCb == nullptr)
    { LogLine("[DIBR_VIZ] state create failed"); return false; }
    g_depthVizReady = true;
    LogLine("[DIBR_VIZ] depth-viz shaders ready");
    return true;
}

void TryPresentDepthViz(IDXGISwapChain* swapChain) noexcept
{
    if (swapChain == nullptr || g_gameContext == nullptr || g_gameDevice == nullptr) return;

    const bool depthMapOn = g_depthMapEnabled.load(std::memory_order_relaxed);
    const bool showViz = depthMapOn && g_depthVizShow.load(std::memory_order_relaxed);
    const bool forceViz = MarkerEnabled(L"MELEVR_ENABLE_DEPTH_VIZ.txt");
    if (!showViz && !forceViz) return;
    // [M0.3b] these two gates were silent; when the viz "shows nothing" this names the reason. Rate-limited.
    if (!g_depthReady.load(std::memory_order_acquire) || g_depthSrv == nullptr)
    {
        static int s_notReadyLogs = 0;
        if (s_notReadyLogs++ < 6) LogLine("[DIBR_VIZ] waiting: depth not ready yet (capture hasn't locked a real source)");
        return;
    }
    if (!EnsureDepthVizShaders()) return;

    // [RELIEF M0.1] the three exits below were SILENT - a session showed "shaders ready" then no paint,
    // with no way to tell which step bailed. First-3 logs on each.
    static int s_vizBailLogs = 0;
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) || backBuffer == nullptr)
    {
        if (s_vizBailLogs++ < 3) LogLine("[DIBR_VIZ] bail: GetBuffer failed");
        return;
    }
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(g_gameDevice->CreateRenderTargetView(backBuffer, nullptr, &rtv)) || rtv == nullptr)
    {
        if (s_vizBailLogs++ < 3) LogLine("[DIBR_VIZ] bail: CreateRenderTargetView on backbuffer failed");
        backBuffer->Release();
        return;
    }
    {
        D3D11_TEXTURE2D_DESC bbd = {};
        backBuffer->GetDesc(&bbd);
        if (!EnsureColorCopy(bbd) || g_dibrColorCopy == nullptr || g_dibrColorSrv == nullptr)
        {
            if (s_vizBailLogs++ < 3) LogLine("[DIBR_VIZ] bail: EnsureColorCopy failed");
            rtv->Release();
            backBuffer->Release();
            return;
        }
        g_gameContext->CopyResource(g_dibrColorCopy, backBuffer);

        // Save the state that gets touched (this runs mid game-frame on the present thread).
        ID3D11RenderTargetView* oldRtv[8] = {}; ID3D11DepthStencilView* oldDsv = nullptr;
        g_gameContext->OMGetRenderTargets(8, oldRtv, &oldDsv);
        D3D11_VIEWPORT oldVp[16] = {}; UINT oldVpN = 16; g_gameContext->RSGetViewports(&oldVpN, oldVp);
        ID3D11RasterizerState* oldRs = nullptr; g_gameContext->RSGetState(&oldRs);
        ID3D11DepthStencilState* oldDs = nullptr; UINT oldRef = 0; g_gameContext->OMGetDepthStencilState(&oldDs, &oldRef);
        float oldBlendFactor[4] = {}; UINT oldSampleMask = 0xffffffff; ID3D11BlendState* oldBlend = nullptr;
        g_gameContext->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
        D3D11_PRIMITIVE_TOPOLOGY oldTopo; g_gameContext->IAGetPrimitiveTopology(&oldTopo);
        ID3D11InputLayout* oldIl = nullptr; g_gameContext->IAGetInputLayout(&oldIl);
        ID3D11VertexShader* oldVs = nullptr; g_gameContext->VSGetShader(&oldVs, nullptr, nullptr);
        ID3D11PixelShader* oldPs = nullptr; g_gameContext->PSGetShader(&oldPs, nullptr, nullptr);
        ID3D11ShaderResourceView* oldSrv[2] = {}; g_gameContext->PSGetShaderResources(0, 2, oldSrv);
        ID3D11SamplerState* oldSamp = nullptr; g_gameContext->PSGetSamplers(0, 1, &oldSamp);
        ID3D11Buffer* oldPsCb = nullptr; g_gameContext->PSGetConstantBuffers(0, 1, &oldPsCb);

        D3D11_VIEWPORT vp = {}; vp.Width = static_cast<float>(bbd.Width); vp.Height = static_cast<float>(bbd.Height); vp.MaxDepth = 1.0f;
        const float bf[4] = {0, 0, 0, 0};
        g_gameContext->OMSetRenderTargets(1, &rtv, nullptr);
        g_gameContext->RSSetViewports(1, &vp);
        g_gameContext->RSSetState(g_depthVizRaster);
        g_gameContext->OMSetDepthStencilState(g_depthVizDepthState, 0);
        g_gameContext->OMSetBlendState(g_depthVizBlend, bf, 0xffffffff);
        g_gameContext->IASetInputLayout(nullptr);
        g_gameContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_gameContext->VSSetShader(g_depthVizVs, nullptr, 0);
        g_gameContext->PSSetShader(g_depthMapPs, nullptr, 0);
        ID3D11ShaderResourceView* mapSrvs[2] = { g_dibrColorSrv, g_depthSrv };
        g_gameContext->PSSetShaderResources(0, 2, mapSrvs);
        ID3D11SamplerState* sampler = g_depthVizSampler;
        g_gameContext->PSSetSamplers(0, 1, &sampler);

        if (g_depthMapCb != nullptr)
        {
            D3D11_MAPPED_SUBRESOURCE mp = {};
            if (SUCCEEDED(g_gameContext->Map(g_depthMapCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp)))
            {
                float* p = static_cast<float*>(mp.pData);
                p[0] = g_depthNear.load(std::memory_order_relaxed);
                p[1] = g_depthFar.load(std::memory_order_relaxed);
                p[2] = g_depthFlip.load(std::memory_order_relaxed);
                p[3] = g_depthGamma.load(std::memory_order_relaxed);
                g_gameContext->Unmap(g_depthMapCb, 0);
            }
            g_gameContext->PSSetConstantBuffers(0, 1, &g_depthMapCb);
        }
        g_gameContext->Draw(3, 0);

        {
            const int n = g_depthVizLogs.fetch_add(1, std::memory_order_relaxed);
            if (n < 3)
            {
                LogLine("[DIBR_VIZ] rendered greyscale depth map to backbuffer (Enhancements > Depth map)");
            }
        }

        // Restore.
        ID3D11ShaderResourceView* nullSrv[2] = {}; g_gameContext->PSSetShaderResources(0, 2, nullSrv);
        g_gameContext->OMSetRenderTargets(8, oldRtv, oldDsv);
        g_gameContext->RSSetViewports(oldVpN, oldVp);
        g_gameContext->RSSetState(oldRs);
        g_gameContext->OMSetDepthStencilState(oldDs, oldRef);
        g_gameContext->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
        g_gameContext->IASetPrimitiveTopology(oldTopo);
        g_gameContext->IASetInputLayout(oldIl);
        g_gameContext->VSSetShader(oldVs, nullptr, 0);
        g_gameContext->PSSetShader(oldPs, nullptr, 0);
        g_gameContext->PSSetShaderResources(0, 2, oldSrv);
        g_gameContext->PSSetSamplers(0, 1, &oldSamp);
        g_gameContext->PSSetConstantBuffers(0, 1, &oldPsCb);
        for (auto*& r : oldRtv) SafeRelease(r);
        SafeRelease(oldDsv); SafeRelease(oldRs); SafeRelease(oldDs); SafeRelease(oldBlend); SafeRelease(oldIl);
        SafeRelease(oldVs); SafeRelease(oldPs); SafeRelease(oldSrv[0]); SafeRelease(oldSrv[1]); SafeRelease(oldSamp); SafeRelease(oldPsCb);
        rtv->Release();
    }
    backBuffer->Release();
}
void TryPresentFlatMenu(IDXGISwapChain* swapChain) noexcept
{
    if (swapChain == nullptr || g_gameDevice == nullptr || g_gameContext == nullptr) return;
    if (!IsOpenXrDisabledForBoundary()) return;

    DXGI_SWAP_CHAIN_DESC scd = {};
    if (FAILED(swapChain->GetDesc(&scd)) || scd.BufferDesc.Width == 0 || scd.BufferDesc.Height == 0) return;

    static bool menuInitialized = false;
    static bool insertWasDown = false;
    if (!menuInitialized)
    {
        MELEVR::Menu::Init(g_gameDevice, g_gameContext,
                           static_cast<int>(scd.BufferDesc.Width),
                           static_cast<int>(scd.BufferDesc.Height));
        MELEVR::Menu::InstallInputBlock();
        if (scd.OutputWindow != nullptr) MELEVR::Menu::SetGameWindow(scd.OutputWindow);
        menuInitialized = true;
        LogLine("[FLATMENU] initialized monitor menu path (OpenXR disabled).");
    }

    const bool insertDown = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (insertDown && !insertWasDown) MELEVR::Menu::Toggle();
    insertWasDown = insertDown;
    if (!MELEVR::Menu::IsOpen()) return;

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) || backBuffer == nullptr) return;
    ID3D11RenderTargetView* rtv = nullptr;
    if (SUCCEEDED(g_gameDevice->CreateRenderTargetView(backBuffer, nullptr, &rtv)) && rtv != nullptr)
    {
        MELEVR::Menu::RenderFlat(rtv,
                                 static_cast<int>(scd.BufferDesc.Width),
                                 static_cast<int>(scd.BufferDesc.Height));
        rtv->Release();
    }
    backBuffer->Release();
}

void TryPresentSidecarAtlas(IDXGISwapChain* swapChain) noexcept
{
    if (swapChain == nullptr || g_gameContext == nullptr)
    {
        return;
    }
    if (!MarkerEnabled(L"MELEVR_ENABLE_CAMCB96_SIDECAR.txt"))
    {
        return;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    const HRESULT hr = swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer));
    if (FAILED(hr) || backBuffer == nullptr)
    {
        return;
    }

    D3D11_TEXTURE2D_DESC backDesc = {};
    backBuffer->GetDesc(&backDesc);

    bool copied = false;
    {
        std::lock_guard<std::mutex> lock(g_sidecarMutex);
        if (g_sidecarValid && g_sidecarColor != nullptr &&
            g_sidecarColorDesc.Width == backDesc.Width &&
            g_sidecarColorDesc.Height == backDesc.Height &&
            g_sidecarColorDesc.Format == backDesc.Format &&
            g_sidecarColorDesc.SampleDesc.Count == backDesc.SampleDesc.Count &&
            g_sidecarColorDesc.SampleDesc.Quality == backDesc.SampleDesc.Quality)
        {
            g_gameContext->CopyResource(backBuffer, g_sidecarColor);
            copied = true;
        }
    }

    const int n = g_camCb96SidecarCopyLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 20 || (n % 120) == 0)
    {
        char line[512] = {};
        sprintf_s(line,
                  "[CAMCB96_SIDECAR_PRESENT] #%d copied=%d back=%ux%u fmt=%u sidecar=%ux%u fmt=%u",
                  n + 1,
                  copied ? 1 : 0,
                  backDesc.Width,
                  backDesc.Height,
                  static_cast<unsigned>(backDesc.Format),
                  g_sidecarColorDesc.Width,
                  g_sidecarColorDesc.Height,
                  static_cast<unsigned>(g_sidecarColorDesc.Format));
        LogLine(line);
    }

    backBuffer->Release();
}

void ReleaseSnapshot(ContextStateSnapshot& s) noexcept
{
    for (auto*& rtv : s.rtvs) SafeRelease(rtv);
    SafeRelease(s.dsv);
    SafeRelease(s.rasterizerState);
    SafeRelease(s.blendState);
    SafeRelease(s.depthStencilState);
    SafeRelease(s.inputLayout);
    for (auto*& vb : s.vertexBuffers) SafeRelease(vb);
    SafeRelease(s.indexBuffer);
    SafeRelease(s.vertexShader);
    SafeRelease(s.geometryShader);
    SafeRelease(s.pixelShader);
    for (auto*& cb : s.vsConstantBuffers) SafeRelease(cb);
    for (auto*& cb : s.gsConstantBuffers) SafeRelease(cb);
    for (auto*& cb : s.psConstantBuffers) SafeRelease(cb);
    for (auto*& srv : s.psSrvs) SafeRelease(srv);
    for (auto*& sampler : s.psSamplers) SafeRelease(sampler);
    s = ContextStateSnapshot{};
}

bool SaveContextState(ContextStateSnapshot& s) noexcept
{
    if (g_gameContext == nullptr) return false;

    ReleaseSnapshot(s);
    g_gameContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtvs, &s.dsv);
    s.viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    g_gameContext->RSGetViewports(&s.viewportCount, s.viewports);
    s.scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    g_gameContext->RSGetScissorRects(&s.scissorCount, s.scissors);
    g_gameContext->RSGetState(&s.rasterizerState);
    g_gameContext->OMGetBlendState(&s.blendState, s.blendFactor, &s.sampleMask);
    g_gameContext->OMGetDepthStencilState(&s.depthStencilState, &s.stencilRef);

    g_gameContext->IAGetInputLayout(&s.inputLayout);
    g_gameContext->IAGetVertexBuffers(0, kSavedVertexBufferCount, s.vertexBuffers, s.vertexStrides, s.vertexOffsets);
    g_gameContext->IAGetIndexBuffer(&s.indexBuffer, &s.indexFormat, &s.indexOffset);
    g_gameContext->IAGetPrimitiveTopology(&s.topology);

    g_gameContext->VSGetShader(&s.vertexShader, nullptr, nullptr);
    g_gameContext->GSGetShader(&s.geometryShader, nullptr, nullptr);
    g_gameContext->PSGetShader(&s.pixelShader, nullptr, nullptr);
    g_gameContext->VSGetConstantBuffers(0, kSavedConstantBufferCount, s.vsConstantBuffers);
    g_gameContext->GSGetConstantBuffers(0, kSavedConstantBufferCount, s.gsConstantBuffers);
    g_gameContext->PSGetConstantBuffers(0, kSavedConstantBufferCount, s.psConstantBuffers);
    g_gameContext->PSGetShaderResources(0, kSavedSrvCount, s.psSrvs);
    g_gameContext->PSGetSamplers(0, kSavedSamplerCount, s.psSamplers);

    s.valid = true;
    return true;
}

void RestoreContextState(ContextStateSnapshot& s) noexcept
{
    if (g_gameContext == nullptr || !s.valid) return;

    g_gameContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.rtvs, s.dsv);
    g_gameContext->RSSetViewports(s.viewportCount, s.viewports);
    g_gameContext->RSSetScissorRects(s.scissorCount, s.scissors);
    g_gameContext->RSSetState(s.rasterizerState);
    g_gameContext->OMSetBlendState(s.blendState, s.blendFactor, s.sampleMask);
    g_gameContext->OMSetDepthStencilState(s.depthStencilState, s.stencilRef);

    g_gameContext->IASetInputLayout(s.inputLayout);
    g_gameContext->IASetVertexBuffers(0, kSavedVertexBufferCount, s.vertexBuffers, s.vertexStrides, s.vertexOffsets);
    g_gameContext->IASetIndexBuffer(s.indexBuffer, s.indexFormat, s.indexOffset);
    g_gameContext->IASetPrimitiveTopology(s.topology);

    g_gameContext->VSSetShader(s.vertexShader, nullptr, 0);
    g_gameContext->GSSetShader(s.geometryShader, nullptr, 0);
    g_gameContext->PSSetShader(s.pixelShader, nullptr, 0);
    g_gameContext->VSSetConstantBuffers(0, kSavedConstantBufferCount, s.vsConstantBuffers);
    g_gameContext->GSSetConstantBuffers(0, kSavedConstantBufferCount, s.gsConstantBuffers);
    g_gameContext->PSSetConstantBuffers(0, kSavedConstantBufferCount, s.psConstantBuffers);
    g_gameContext->PSSetShaderResources(0, kSavedSrvCount, s.psSrvs);
    g_gameContext->PSSetSamplers(0, kSavedSamplerCount, s.psSamplers);

    ReleaseSnapshot(s);
}

bool SameTextureDesc(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) noexcept
{
    return a.Width == b.Width &&
           a.Height == b.Height &&
           a.MipLevels == b.MipLevels &&
           a.ArraySize == b.ArraySize &&
           a.Format == b.Format &&
           a.SampleDesc.Count == b.SampleDesc.Count &&
           a.SampleDesc.Quality == b.SampleDesc.Quality;
}

bool EnsureStereoPassTextures(const D3D11_TEXTURE2D_DESC& srcDesc) noexcept
{
    if (g_gameDevice == nullptr) return false;

    D3D11_TEXTURE2D_DESC d = srcDesc;
    // [SFR-UI] pass 0 needs RENDER_TARGET so per-present UI draws can be mirrored into the left eye
    // (Scaleform draws once per present, after both passes - see the SFR UI dup in the draw hooks).
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    d.Usage = D3D11_USAGE_DEFAULT;

    bool allBanksValid = true;
    for (int bank = 0; bank < kStereoPassBanks; ++bank)
    {
        allBanksValid = allBanksValid && g_stereoPassTex[bank][0] != nullptr && g_stereoPassTex[bank][1] != nullptr;
    }
    if (g_stereoPassDescValid && SameTextureDesc(g_stereoPassDesc, d) && allBanksValid)
    {
        return true;
    }

    for (int bank = 0; bank < kStereoPassBanks; ++bank)
    {
        SafeRelease(g_sfrPass0Rtv[bank]);
        SafeRelease(g_stereoPassTex[bank][0]);
        SafeRelease(g_stereoPassTex[bank][1]);
    }
    g_stereoPassDescValid = false;
    g_stereoPassDesc = d;
    g_stereoWriteBank = 0;
    g_stereoPublishedBank = -1;
    g_stereoWriteHasPass0 = false;
    g_stereoPublishedPairCount = 0;
    for (int bank = 0; bank < kStereoPassBanks; ++bank) g_stereoPairArm[bank] = 0;

    bool created = true;
    for (int bank = 0; bank < kStereoPassBanks; ++bank)
    {
        created = created &&
            SUCCEEDED(g_gameDevice->CreateTexture2D(&d, nullptr, &g_stereoPassTex[bank][0])) &&
            SUCCEEDED(g_gameDevice->CreateTexture2D(&d, nullptr, &g_stereoPassTex[bank][1]));
    }
    if (!created)
    {
        for (int bank = 0; bank < kStereoPassBanks; ++bank)
        {
            SafeRelease(g_stereoPassTex[bank][0]);
            SafeRelease(g_stereoPassTex[bank][1]);
        }
        LogLine("[PASSCAP] texture creation FAILED");
        return false;
    }

    g_stereoPassDescValid = true;
    LogLine("[PASSCAP] stereo pass texture banks created " + std::to_string(kStereoPassBanks) + "x " +
            std::to_string(d.Width) + "x" +
            std::to_string(d.Height) + " fmt=" + std::to_string(static_cast<int>(d.Format)));
    return true;
}

std::string DxgiFormatName(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    default: return "fmt#" + std::to_string(static_cast<int>(format));
    }
}

bool PatchPointerSlot(void** slot, void* replacement, void** original, const char* name) noexcept
{
    if (slot == nullptr || replacement == nullptr || original == nullptr)
    {
        return false;
    }
    if (*slot == replacement)
    {
        return true;  // already hooked; *original already holds the real pointer
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
    {
        MELEVR::Logger::LogWindowsError((std::string(name) + " VirtualProtect failed").c_str(), GetLastError());
        return false;
    }

    *original = *slot;
    *slot = replacement;

    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

    LogLine(std::string("[CAPTURE] hooked ") + name + " (original " + HexPointer(*original) + ")");
    return true;
}

// ---- Bink movie detection (2026-07-07) --------------------------------------------------------------
// GROUND-TRUTH "a prerendered movie is decoding": hook bink2w64.dll's DoFrame family and stamp a tick.
// The present-gap heuristic alone confused cutscene FADES (no 3D for ~12 presents, no Bink) with movies and
// missed movie precache blips (a stray 3D view mid-movie). xr_session combines: BinkRecentlyActive AND no-3D
// = fullscreen movie; small in-world video panels keep the 3D scene alive so they never trip it.
std::atomic<unsigned long long> g_lastBinkFrameMs{0};
bool g_binkHooksTried = false;
using BinkGenericFn = std::uintptr_t(*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
BinkGenericFn g_origBinkDoFrame = nullptr;
BinkGenericFn g_origBinkDoFrameAsync = nullptr;
BinkGenericFn g_origBinkDoFrameAsyncWait = nullptr;

std::uintptr_t HookBinkDoFrame(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) noexcept
{
    g_lastBinkFrameMs.store(GetTickCount64(), std::memory_order_relaxed);
    return g_origBinkDoFrame ? g_origBinkDoFrame(a, b, c, d) : 0;
}
std::uintptr_t HookBinkDoFrameAsync(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) noexcept
{
    g_lastBinkFrameMs.store(GetTickCount64(), std::memory_order_relaxed);
    return g_origBinkDoFrameAsync ? g_origBinkDoFrameAsync(a, b, c, d) : 0;
}
std::uintptr_t HookBinkDoFrameAsyncWait(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) noexcept
{
    g_lastBinkFrameMs.store(GetTickCount64(), std::memory_order_relaxed);
    return g_origBinkDoFrameAsyncWait ? g_origBinkDoFrameAsyncWait(a, b, c, d) : 0;
}

void InstallInlineHook(void* target, void* replacement, void** original, const char* name) noexcept
{
    if (target == nullptr || replacement == nullptr || original == nullptr || target == replacement)
    {
        return;
    }

    MH_STATUS init = MH_Initialize();
    if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED)
    {
        LogLine(std::string("[D3DINLINE] MH_Initialize failed for ") + name +
                " status=" + std::to_string(static_cast<int>(init)));
        return;
    }

    MH_STATUS status = MH_CreateHook(target, replacement, original);
    if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED)
    {
        LogLine(std::string("[D3DINLINE] MH_CreateHook failed for ") + name +
                " target=" + HexPointer(target) +
                " status=" + std::to_string(static_cast<int>(status)));
        return;
    }

    status = MH_EnableHook(target);
    if (status != MH_OK && status != MH_ERROR_ENABLED)
    {
        LogLine(std::string("[D3DINLINE] MH_EnableHook failed for ") + name +
                " target=" + HexPointer(target) +
                " status=" + std::to_string(static_cast<int>(status)));
        return;
    }

    LogLine(std::string("[D3DINLINE] hooked ") + name + " target=" + HexPointer(target));
}

// ============================================================================
// [DISPQ] Which OS call does LE1 clamp its resolution to?
//
// Proven 2026-07-10: GamerSettings.ini asked for 5120x2880 and the game handed DXGI 3840x2160 - exactly the
// desktop mode - so the clamp happens INSIDE the game, before any DXGI call. Both EnumDisplaySettings and
// GetSystemMetrics(SM_CXSCREEN) return that same number, so the [SCREQ] line alone cannot say which the game
// actually read. This logs every candidate call with its result and whether it landed BEFORE the swapchain
// was created (g_screqSeen). Only the calls that precede swapchain creation can possibly be the clamp source.
//
// LOGGING ONLY. Nothing is spoofed yet - spoofing three APIs at once and then trying to work out which one
// mattered from a broken window is exactly the guess-first loop this project is supposed to avoid.
// ============================================================================
std::atomic_bool g_screqSeen{false};   // set once CreateSwapChainForHwnd has been observed

// ---- [DISPQ] the spoof ------------------------------------------------------
// Proven by the DISPQ logging run: LE1 asks ONLY for ENUM_CURRENT_SETTINGS (mode == -1) and
// GetSystemMetrics(SM_CXSCREEN/CYSCREEN). It never enumerates the mode list, so there is no mode list to
// inject into - the clamp is "the desktop is 3840x2160, therefore render 3840x2160". Both APIs agree, so
// which one is load-bearing is unknowable from outside; they're made to agree on a bigger number instead.
//
// Two safety properties:
//  1. Target comes from GamerSettings.ini (read at install time - MELEVR.ini isn't loaded yet). If the user
//     asks for <= their real desktop size, the spoof never arms and every hook is a pure pass-through.
//  2. Only results matching the REAL PRIMARY desktop size are rewritten. The test machine's second monitor reports
//     2560x1440 through the same API; rewriting that would be a lie about a display nothing is rendering to.
UINT g_realPrimaryW = 0;
UINT g_realPrimaryH = 0;
UINT g_spoofW = 0;   // 0 = spoof disabled
UINT g_spoofH = 0;

bool SpoofActive() noexcept { return g_spoofW != 0 && g_spoofH != 0; }

// Rewrite only if this result IS the primary desktop mode.
bool ShouldRewrite(UINT w, UINT h) noexcept
{
    return SpoofActive() && w == g_realPrimaryW && h == g_realPrimaryH;
}

using EnumDisplaySettingsWFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DEVMODEW*);
using EnumDisplaySettingsAFn = BOOL(WINAPI*)(LPCSTR, DWORD, DEVMODEA*);
using GetSystemMetricsFn = int(WINAPI*)(int);

EnumDisplaySettingsWFn g_origEnumDisplaySettingsW = nullptr;
EnumDisplaySettingsAFn g_origEnumDisplaySettingsA = nullptr;
GetSystemMetricsFn g_origGetSystemMetrics = nullptr;

std::atomic<int> g_dispqEnumWLogs{0};
std::atomic<int> g_dispqEnumALogs{0};
std::atomic<int> g_dispqMetricLogs{0};

BOOL WINAPI EnumDisplaySettingsWHook(LPCWSTR device, DWORD modeNum, DEVMODEW* dm) noexcept
{
    const BOOL ok = (g_origEnumDisplaySettingsW != nullptr) ? g_origEnumDisplaySettingsW(device, modeNum, dm) : FALSE;
    bool rewrote = false;
    if (ok && dm != nullptr && ShouldRewrite(dm->dmPelsWidth, dm->dmPelsHeight))
    {
        dm->dmPelsWidth = g_spoofW;
        dm->dmPelsHeight = g_spoofH;
        rewrote = true;
    }
    const int n = g_dispqEnumWLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 24 && dm != nullptr)
    {
        char line[192];
        std::snprintf(line, sizeof(line),
                      "[DISPQ] EnumDisplaySettingsW mode=%lu -> ok=%d %ux%u @%uHz preSwapchain=%d spoofed=%d",
                      modeNum, ok ? 1 : 0, ok ? dm->dmPelsWidth : 0u, ok ? dm->dmPelsHeight : 0u,
                      ok ? dm->dmDisplayFrequency : 0u,
                      g_screqSeen.load(std::memory_order_acquire) ? 0 : 1, rewrote ? 1 : 0);
        LogLine(line);
    }
    return ok;
}

BOOL WINAPI EnumDisplaySettingsAHook(LPCSTR device, DWORD modeNum, DEVMODEA* dm) noexcept
{
    const BOOL ok = (g_origEnumDisplaySettingsA != nullptr) ? g_origEnumDisplaySettingsA(device, modeNum, dm) : FALSE;
    bool rewrote = false;
    if (ok && dm != nullptr && ShouldRewrite(dm->dmPelsWidth, dm->dmPelsHeight))
    {
        dm->dmPelsWidth = g_spoofW;
        dm->dmPelsHeight = g_spoofH;
        rewrote = true;
    }
    const int n = g_dispqEnumALogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 24 && dm != nullptr)
    {
        char line[192];
        std::snprintf(line, sizeof(line),
                      "[DISPQ] EnumDisplaySettingsA mode=%lu -> ok=%d %ux%u @%uHz preSwapchain=%d spoofed=%d",
                      modeNum, ok ? 1 : 0, ok ? dm->dmPelsWidth : 0u, ok ? dm->dmPelsHeight : 0u,
                      ok ? dm->dmDisplayFrequency : 0u,
                      g_screqSeen.load(std::memory_order_acquire) ? 0 : 1, rewrote ? 1 : 0);
        LogLine(line);
    }
    return ok;
}

int WINAPI GetSystemMetricsHook(int index) noexcept
{
    int value = (g_origGetSystemMetrics != nullptr) ? g_origGetSystemMetrics(index) : 0;
    bool rewrote = false;

    // Only the primary-screen extents. SM_CXVIRTUALSCREEN spans ALL monitors, so rewriting it would corrupt
    // multi-monitor coordinate space; left alone deliberately.
    if (SpoofActive())
    {
        if ((index == SM_CXSCREEN || index == SM_CXFULLSCREEN) && value == static_cast<int>(g_realPrimaryW))
        {
            value = static_cast<int>(g_spoofW);
            rewrote = true;
        }
        else if ((index == SM_CYSCREEN || index == SM_CYFULLSCREEN) && value == static_cast<int>(g_realPrimaryH))
        {
            value = static_cast<int>(g_spoofH);
            rewrote = true;
        }
    }

    if (index == SM_CXSCREEN || index == SM_CYSCREEN ||
        index == SM_CXFULLSCREEN || index == SM_CYFULLSCREEN)
    {
        const int n = g_dispqMetricLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 32)
        {
            char line[160];
            std::snprintf(line, sizeof(line), "[DISPQ] GetSystemMetrics(%d) -> %d preSwapchain=%d spoofed=%d",
                          index, value, g_screqSeen.load(std::memory_order_acquire) ? 0 : 1, rewrote ? 1 : 0);
            LogLine(line);
        }
    }
    return value;
}

// ============================================================================
// [DISPQ2] The fourth source.
//
// Proven 2026-07-10: with EnumDisplaySettings AND GetSystemMetrics both reporting 5120x2880 (spoofed=1 on
// every primary-display call, hooks installed 8s before the queries), LE1 still handed DXGI 3840x2160 - the
// REAL desktop size. Something else is telling it the truth. Remaining candidates, all logged here:
//   IDXGIOutput::GetDesc            -> DesktopCoordinates is the real rect
//   IDXGIOutput::GetDisplayModeList -> game may snap 5120x2880 down to the largest real mode
//   IDXGIOutput::FindClosestMatchingMode -> literally a "snap to a real mode" API
//   GetMonitorInfo / GetDeviceCaps  -> the classic GDI paths
// LOGGING ONLY. Identify first, then lie to exactly one thing.
// ============================================================================
using GetDescFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput*, DXGI_OUTPUT_DESC*);
using GetDisplayModeListFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput*, DXGI_FORMAT, UINT, UINT*, DXGI_MODE_DESC*);
using FindClosestMatchingModeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIOutput*, const DXGI_MODE_DESC*, DXGI_MODE_DESC*, IUnknown*);
using GetMonitorInfoWFn = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using GetMonitorInfoAFn = BOOL(WINAPI*)(HMONITOR, LPMONITORINFO);
using GetDeviceCapsFn = int(WINAPI*)(HDC, int);

GetDescFn g_origOutputGetDesc = nullptr;
GetDisplayModeListFn g_origGetDisplayModeList = nullptr;
FindClosestMatchingModeFn g_origFindClosestMatchingMode = nullptr;
GetMonitorInfoWFn g_origGetMonitorInfoW = nullptr;
GetMonitorInfoAFn g_origGetMonitorInfoA = nullptr;
GetDeviceCapsFn g_origGetDeviceCaps = nullptr;

// HookSlot dereferences its slotStore unconditionally (idempotence guard) - it cannot take nullptr.
void** g_outputGetDescSlot = nullptr;
void** g_getDisplayModeListSlot = nullptr;
void** g_findClosestModeSlot = nullptr;

std::atomic<int> g_dq2GetDescLogs{0};
std::atomic<int> g_dq2ModeListLogs{0};
std::atomic<int> g_dq2ClosestLogs{0};
std::atomic<int> g_dq2MonInfoLogs{0};
std::atomic<int> g_dq2CapsLogs{0};

// The primary output/monitor is the one at the desktop origin whose extent matches the real primary size.
// Anchoring on (0,0) as well as the size keeps the capture off the second monitor, which sits at x=3840.
bool IsPrimaryRect(const RECT& r) noexcept
{
    return r.left == 0 && r.top == 0 &&
           static_cast<UINT>(r.right - r.left) == g_realPrimaryW &&
           static_cast<UINT>(r.bottom - r.top) == g_realPrimaryH;
}

HRESULT STDMETHODCALLTYPE OutputGetDescHook(IDXGIOutput* self, DXGI_OUTPUT_DESC* desc) noexcept
{
    // NO REWRITE HERE, deliberately. DXGI's own GetDesc calls GetMonitorInfoW internally, so it already
    // returns the spoofed rect (proven: this hook logged desktop=5120x2880 with spoofed=0). Rewriting here
    // too would be dead code pretending to be load-bearing. Kept purely as an observer.
    const HRESULT hr = (g_origOutputGetDesc != nullptr) ? g_origOutputGetDesc(self, desc) : E_FAIL;
    const bool rewrote = false;
    const int n = g_dq2GetDescLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 16 && SUCCEEDED(hr) && desc != nullptr)
    {
        const RECT& r = desc->DesktopCoordinates;
        char line[192];
        std::snprintf(line, sizeof(line),
                      "[DISPQ2] IDXGIOutput::GetDesc -> desktop=%ldx%ld (%ld,%ld)-(%ld,%ld) preSwapchain=%d spoofed=%d",
                      r.right - r.left, r.bottom - r.top, r.left, r.top, r.right, r.bottom,
                      g_screqSeen.load(std::memory_order_acquire) ? 0 : 1, rewrote ? 1 : 0);
        LogLine(line);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE GetDisplayModeListHook(IDXGIOutput* self, DXGI_FORMAT fmt, UINT flags,
                                                 UINT* numModes, DXGI_MODE_DESC* modes) noexcept
{
    const HRESULT hr = (g_origGetDisplayModeList != nullptr)
                           ? g_origGetDisplayModeList(self, fmt, flags, numModes, modes)
                           : E_FAIL;
    const int n = g_dq2ModeListLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 8 && SUCCEEDED(hr) && numModes != nullptr)
    {
        UINT maxW = 0, maxH = 0;
        if (modes != nullptr)
        {
            for (UINT i = 0; i < *numModes; ++i)
            {
                if (modes[i].Width > maxW) { maxW = modes[i].Width; maxH = modes[i].Height; }
            }
        }
        char line[192];
        std::snprintf(line, sizeof(line),
                      "[DISPQ2] GetDisplayModeList fmt=%d count=%u listFilled=%d maxMode=%ux%u preSwapchain=%d",
                      static_cast<int>(fmt), *numModes, (modes != nullptr) ? 1 : 0, maxW, maxH,
                      g_screqSeen.load(std::memory_order_acquire) ? 0 : 1);
        LogLine(line);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE FindClosestMatchingModeHook(IDXGIOutput* self, const DXGI_MODE_DESC* want,
                                                      DXGI_MODE_DESC* got, IUnknown* device) noexcept
{
    const HRESULT hr = (g_origFindClosestMatchingMode != nullptr)
                           ? g_origFindClosestMatchingMode(self, want, got, device)
                           : E_FAIL;
    const int n = g_dq2ClosestLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 16)
    {
        char line[192];
        std::snprintf(line, sizeof(line),
                      "[DISPQ2] FindClosestMatchingMode want=%ux%u -> got=%ux%u hr=0x%08X preSwapchain=%d",
                      (want != nullptr) ? want->Width : 0u, (want != nullptr) ? want->Height : 0u,
                      (SUCCEEDED(hr) && got != nullptr) ? got->Width : 0u,
                      (SUCCEEDED(hr) && got != nullptr) ? got->Height : 0u,
                      static_cast<unsigned>(hr), g_screqSeen.load(std::memory_order_acquire) ? 0 : 1);
        LogLine(line);
    }
    return hr;
}

BOOL WINAPI GetMonitorInfoWHook(HMONITOR mon, LPMONITORINFO mi) noexcept
{
    const BOOL ok = (g_origGetMonitorInfoW != nullptr) ? g_origGetMonitorInfoW(mon, mi) : FALSE;
    bool rewrote = false;
    if (ok && mi != nullptr && SpoofActive() &&
        (mi->dwFlags & MONITORINFOF_PRIMARY) != 0 && IsPrimaryRect(mi->rcMonitor))
    {
        mi->rcMonitor.right = mi->rcMonitor.left + static_cast<LONG>(g_spoofW);
        mi->rcMonitor.bottom = mi->rcMonitor.top + static_cast<LONG>(g_spoofH);
        // rcWork must stay consistent with rcMonitor or window placement maths goes strange.
        mi->rcWork.right = mi->rcWork.left + static_cast<LONG>(g_spoofW);
        mi->rcWork.bottom = mi->rcWork.top + static_cast<LONG>(g_spoofH);
        rewrote = true;
    }
    const int n = g_dq2MonInfoLogs.fetch_add(1, std::memory_order_relaxed);
    if (n < 16 && ok && mi != nullptr)
    {
        const RECT& r = mi->rcMonitor;
        char line[192];
        std::snprintf(line, sizeof(line),
                      "[DISPQ2] GetMonitorInfoW -> rcMonitor=%ldx%ld primary=%d preSwapchain=%d spoofed=%d",
                      r.right - r.left, r.bottom - r.top, (mi->dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0,
                      g_screqSeen.load(std::memory_order_acquire) ? 0 : 1, rewrote ? 1 : 0);
        LogLine(line);
    }
    return ok;
}

// LE1 itself uses the W variant, but an ASI/overlay in another process could take the A path and
// re-introduce the real size. Same rewrite, same primary-only guard.
BOOL WINAPI GetMonitorInfoAHook(HMONITOR mon, LPMONITORINFO mi) noexcept
{
    const BOOL ok = (g_origGetMonitorInfoA != nullptr) ? g_origGetMonitorInfoA(mon, mi) : FALSE;
    if (ok && mi != nullptr && SpoofActive() &&
        (mi->dwFlags & MONITORINFOF_PRIMARY) != 0 && IsPrimaryRect(mi->rcMonitor))
    {
        mi->rcMonitor.right = mi->rcMonitor.left + static_cast<LONG>(g_spoofW);
        mi->rcMonitor.bottom = mi->rcMonitor.top + static_cast<LONG>(g_spoofH);
        mi->rcWork.right = mi->rcWork.left + static_cast<LONG>(g_spoofW);
        mi->rcWork.bottom = mi->rcWork.top + static_cast<LONG>(g_spoofH);
    }
    return ok;
}

int WINAPI GetDeviceCapsHook(HDC hdc, int index) noexcept
{
    const int value = (g_origGetDeviceCaps != nullptr) ? g_origGetDeviceCaps(hdc, index) : 0;
    if (index == HORZRES || index == VERTRES || index == DESKTOPHORZRES || index == DESKTOPVERTRES)
    {
        const int n = g_dq2CapsLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 16)
        {
            char line[160];
            std::snprintf(line, sizeof(line), "[DISPQ2] GetDeviceCaps(%d) -> %d preSwapchain=%d",
                          index, value, g_screqSeen.load(std::memory_order_acquire) ? 0 : 1);
            LogLine(line);
        }
    }
    return value;
}

std::wstring GameConfigPath() noexcept
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return L"";
    // ...\Game\ME1\Binaries\Win64\MassEffect1.exe -> ...\Game\ME1\BioGame\Config\GamerSettings.ini
    std::wstring path(exePath);
    const size_t cut = path.rfind(L"\\Binaries\\");
    if (cut == std::wstring::npos) return L"";
    return path.substr(0, cut) + L"\\BioGame\\Config\\GamerSettings.ini";
}

std::wstring AutoResPath() noexcept
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return L"";
    std::wstring path(exePath);
    const size_t cut = path.rfind(L'\\');
    if (cut == std::wstring::npos) return L"";
    return path.substr(0, cut) + L"\\MELEVR_autores.ini";
}

// Read ResX/ResY out of GamerSettings.ini by hand. MELEVR.ini's loader isn't up yet at DLL attach, and the
// game's own config path is the right source of truth anyway - this is the number the user typed.
bool ReadRequestedResFromGamerSettings(UINT* outW, UINT* outH) noexcept
{
    const std::wstring path = GameConfigPath();
    if (path.empty()) return false;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || f == nullptr) return false;

    UINT w = 0, h = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), f) != nullptr)
    {
        unsigned v = 0;
        if (sscanf_s(buf, " ResX = %u", &v) == 1 || sscanf_s(buf, " ResX=%u", &v) == 1) w = v;
        else if (sscanf_s(buf, " ResY = %u", &v) == 1 || sscanf_s(buf, " ResY=%u", &v) == 1) h = v;
    }
    fclose(f);

    if (w == 0 || h == 0) return false;
    *outW = w;
    *outH = h;
    return true;
}

// [HDRCHK] Read-only. Logs the game's OWN persisted HDR setting (GamerSettings.ini [HDR] DesiredRange) at
// startup. A blue/doubled headset image was reported even after turning HDR off in Mass Effect AND
// Windows - proof either way needs two data points, not a guess: this is one (does the game's own setting
// say SDR?), [SCREQ]'s existing fmt= field is the other (did the game actually REQUEST an SDR swapchain
// format from DXGI - fmt=28/R8G8B8A8_UNORM vs fmt=10/R16G16B16A16_FLOAT?). If a future report shows
// DesiredRange=SDR AND fmt=28 but the backbuffer still comes back HDR, that is real evidence the cause is
// Windows/DWM composition overriding a well-behaved request - not a guess, and only then does hooking a
// DXGI HDR-capability query (the way resolution already spoofs GetMonitorInfoW) become justified rather
// than a blind vtable guess that could crash the exact thing being fixed. MELE-VR.bat now force-writes
// DesiredRange=DynamicRange_SDR at install/re-run time (the ini channel proven to actually stick - see the
// dormant auto-res sidecar note above for why a DLL-side write of THIS SAME FILE was already proven to lose
// a race against the Steam launcher and NOT reach the game).
void LogHdrSettingFromGamerSettings() noexcept
{
    const std::wstring path = GameConfigPath();
    if (path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || f == nullptr) return;

    char buf[256];
    char range[128] = "<not found>";
    while (fgets(buf, sizeof(buf), f) != nullptr)
    {
        char v[120] = {};
        if (sscanf_s(buf, " DesiredRange = %119s", v, (unsigned)sizeof(v)) == 1 ||
            sscanf_s(buf, " DesiredRange=%119s", v, (unsigned)sizeof(v)) == 1)
        {
            strcpy_s(range, sizeof(range), v);
        }
    }
    fclose(f);

    LogLine(std::string("[HDRCHK] GamerSettings.ini [HDR] DesiredRange=") + range);
}

// Rewrite ResX/ResY in place, preserving every other line. The game re-saves this file on exit anyway, but it
// must already hold the target at STARTUP or the game renders min(ini, monitor) and the spoof buys nothing.
bool WriteResToGamerSettings(UINT w, UINT h) noexcept
{
    const std::wstring path = GameConfigPath();
    if (path.empty() || w == 0 || h == 0) return false;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || f == nullptr) return false;
    std::string body;
    char buf[512];
    while (fgets(buf, sizeof(buf), f) != nullptr)
    {
        unsigned v = 0;
        if (sscanf_s(buf, " ResX = %u", &v) == 1 || sscanf_s(buf, " ResX=%u", &v) == 1)
        {
            char line[64];
            std::snprintf(line, sizeof(line), "ResX=%u\n", w);
            body += line;
        }
        else if (sscanf_s(buf, " ResY = %u", &v) == 1 || sscanf_s(buf, " ResY=%u", &v) == 1)
        {
            char line[64];
            std::snprintf(line, sizeof(line), "ResY=%u\n", h);
            body += line;
        }
        else
        {
            body += buf;
        }
    }
    fclose(f);

    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || f == nullptr) return false;
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    return true;
}

// [DOF] Ensure GamerSettings.ini [SystemSettings] has DepthOfField set to the requested value. The engine
// reads SystemSettings at load, so this takes effect on the NEXT launch. Preserving read-modify-write
// (mirrors WriteResToGamerSettings): every other line is copied byte-for-byte; only the DepthOfField line
// inside [SystemSettings] is rewritten, and if the key is missing it is inserted at the top of that section.
// Fails safe: on any file error the original file is left untouched (only overwritten after a full read).
// Internal (anon-namespace) worker; the public MELEVR::D3DCapture::EnsureDepthOfFieldSetting forwards here.
bool WriteDofToGamerSettings(bool disable) noexcept
{
    const std::wstring path = GameConfigPath();
    if (path.empty()) return false;

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || f == nullptr) return false;

    const char* desired = disable ? "DepthOfField=False\n" : "DepthOfField=True\n";
    std::string body;
    char buf[512];
    bool inSystemSettings = false;
    bool wrote = false;                 // replaced an existing DepthOfField line
    bool insertedAtHeader = false;      // inserted right after the [SystemSettings] header (missing key)
    while (fgets(buf, sizeof(buf), f) != nullptr)
    {
        // Section header? first non-space char is '['.
        const char* p = buf;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '[')
        {
            // Leaving [SystemSettings] without having written the key -> insert it before this new header.
            if (inSystemSettings && !wrote && !insertedAtHeader)
            {
                body += desired;
                insertedAtHeader = true;
            }
            inSystemSettings = (_strnicmp(p, "[SystemSettings]", 16) == 0);
            body += buf;
            continue;
        }

        // A DepthOfField line inside [SystemSettings] -> replace it (case-insensitive key match).
        if (inSystemSettings && !wrote)
        {
            const char* q = buf;
            while (*q == ' ' || *q == '\t') ++q;
            if (_strnicmp(q, "DepthOfField", 12) == 0)
            {
                const char* r = q + 12;
                while (*r == ' ' || *r == '\t') ++r;
                if (*r == '=')
                {
                    body += desired;
                    wrote = true;
                    continue;
                }
            }
        }
        body += buf;
    }
    // File ended while still inside [SystemSettings] and the key was never present -> append it.
    if (inSystemSettings && !wrote && !insertedAtHeader)
    {
        body += desired;
        wrote = true;
    }
    fclose(f);

    if (!wrote && !insertedAtHeader)
    {
        // No [SystemSettings] section at all - don't invent structure; report no-op.
        LogLine("[DOF] GamerSettings.ini has no [SystemSettings] section; DepthOfField not written.");
        return false;
    }

    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || f == nullptr) return false;
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    LogLine(std::string("[DOF] GamerSettings.ini DepthOfField set to ") + (disable ? "False" : "True") +
            " (applies on next game launch).");
    return true;
}

// [ENGSMOOTH 2026-08-12] Assert SmoothFrameRate=TRUE + MaxSmoothedFrameRate=120 in BIOEngine.ini at
// every DLL attach, BEFORE the engine reads its config. SmoothFrameRate is UE3's tick-DELTA
// SMOOTHING, not just an fps cap: cine/convo cameras are animation-driven (they integrate DeltaTime
// every tick), so raw deltas make the cutscene CONTENT judder at any present rate - immune to every
// delivery/tag/pacing fix, while the head-driven gameplay camera never notices. Cutscenes were
// confirmed smooth in-headset while this was TRUE (2026-08-11) and the judder returned "100%" the
// day the installer wrote FALSE for gameplay fps (e3fc464). The gameplay cap was the NUMBER
// (MaxSmoothedFrameRate=60), not the smoothing - 120 keeps full gameplay rate. ME2, whose cine is
// the reference for smooth, never touches this key. A manual ini edit does NOT survive: the game
// re-syncs BIOEngine.ini (measured 2026-08-12: hand-set TRUE/120 came back FALSE/60 by the next
// session), so this must re-assert from DllMain each launch, ahead of the engine's config read -
// the same reasoning as the DISPQ attach-time hooks, and why EnsureDepthOfFieldSetting's
// next-launch write cannot serve here.
bool EnsureBioEngineSmoothingWorker() noexcept
{
    wchar_t exePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return false;
    std::wstring path(exePath);
    const size_t cut = path.rfind(L"\\Binaries\\");
    if (cut == std::wstring::npos) return false;
    path = path.substr(0, cut) + L"\\BioGame\\Config\\BIOEngine.ini";

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || f == nullptr) return false;
    std::string body;
    char buf[512];
    bool wroteSmooth = false, wroteMax = false;
    while (fgets(buf, sizeof(buf), f) != nullptr)
    {
        const char* q = buf;
        while (*q == ' ' || *q == '\t') ++q;
        if (!wroteSmooth && _strnicmp(q, "SmoothFrameRate", 15) == 0)
        {
            const char* r = q + 15;
            while (*r == ' ' || *r == '\t') ++r;
            if (*r == '=') { body += "SmoothFrameRate=TRUE\n"; wroteSmooth = true; continue; }
        }
        if (!wroteMax && _strnicmp(q, "MaxSmoothedFrameRate", 20) == 0)
        {
            const char* r = q + 20;
            while (*r == ' ' || *r == '\t') ++r;
            if (*r == '=') { body += "MaxSmoothedFrameRate=120\n"; wroteMax = true; continue; }
        }
        body += buf;
    }
    fclose(f);
    if (!wroteSmooth && !wroteMax)
    {
        LogLine("[ENGSMOOTH] BIOEngine.ini has no SmoothFrameRate/MaxSmoothedFrameRate lines; not written.");
        return false;
    }
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || f == nullptr) return false;
    fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    LogLine(std::string("[ENGSMOOTH] BIOEngine.ini asserted SmoothFrameRate=TRUE MaxSmoothedFrameRate=120") +
            (wroteSmooth && wroteMax ? "" : " (one key was missing)"));
    return true;
}

// ---- auto-res sidecar --------------------------------------------------------
// Its own file, NOT MELEVR.ini: that one has four [profile:*] sections and is rewritten by the game's own
// serializer, and this must be read at DLL attach where no config loader exists yet. Flat key=value.
struct AutoResState
{
    unsigned eyeW = 0;      // headset recommended per-eye image (cached after the first XR session)
    unsigned eyeH = 0;
    unsigned targetW = 0;   // current backbuffer target; calibrated down until the frame budget is met
    unsigned targetH = 0;
    int vrMode = -1;        // which mode the cached target was calibrated for (2 = Stereo)

    // DORMANT (2026-07-10). The auto-target has to reach the game through GamerSettings.ini, and this cannot
    // win that file: the Steam launcher rewrites it from another process after the DllMain write, and the
    // game's authoritative read happens before any hook exists yet. Writing at the game's own read (the
    // CreateFileW injection below) proved it too - the game had already cached 5120 and simply re-saved it.
    //
    // The RESOLUTION SPOOF itself works and is what matters: the monitor size reported is what lifts the
    // ceiling. The resolution number now comes from MELE-VR.bat, which writes GamerSettings.ini at install
    // time while nothing else is running. Set enabled=1 in MELEVR_autores.ini to experiment.
    int enabled = 0;
};

AutoResState g_autoRes;

void ReadAutoResFile(AutoResState* s) noexcept
{
    const std::wstring path = AutoResPath();
    if (path.empty() || s == nullptr) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") != 0 || f == nullptr) return;
    char buf[256];
    while (fgets(buf, sizeof(buf), f) != nullptr)
    {
        unsigned u = 0;
        int i = 0;
        if (sscanf_s(buf, " eyeW = %u", &u) == 1) s->eyeW = u;
        else if (sscanf_s(buf, " eyeH = %u", &u) == 1) s->eyeH = u;
        else if (sscanf_s(buf, " targetW = %u", &u) == 1) s->targetW = u;
        else if (sscanf_s(buf, " targetH = %u", &u) == 1) s->targetH = u;
        else if (sscanf_s(buf, " vrMode = %d", &i) == 1) s->vrMode = i;
        else if (sscanf_s(buf, " enabled = %d", &i) == 1) s->enabled = i;
    }
    fclose(f);
}

void WriteAutoResFile(const AutoResState& s) noexcept
{
    const std::wstring path = AutoResPath();
    if (path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"w") != 0 || f == nullptr) return;
    fprintf(f, "; MELE VR auto-resolution state - written by the mod, safe to delete (it re-learns).\n");
    fprintf(f, "; targetW/H is calibrated DOWN from the headset ideal until the frame budget is met.\n");
    fprintf(f, "enabled = %d\n", s.enabled);
    fprintf(f, "eyeW = %u\n", s.eyeW);
    fprintf(f, "eyeH = %u\n", s.eyeH);
    fprintf(f, "targetW = %u\n", s.targetW);
    fprintf(f, "targetH = %u\n", s.targetH);
    fprintf(f, "vrMode = %d\n", s.vrMode);
    fclose(f);
}

// Ideal target for the headset, used only by the DORMANT auto-res sidecar (see AutoResState: that lane
// is proven unwinnable, the resolution comes from MELE-VR.bat). Kept correct anyway so enabling it for an
// experiment cannot silently reintroduce a shape bug.
//
// CORRECTED 2026-08-11. This used to hardcode 16:9 and double the width for an SBS split. Both are wrong
// for Stereo 2 (SFR), which renders a FULL frame per eye: the width is the eye width, and the ASPECT must
// track the headset's own window or every UI element arrives mis-proportioned (square target = 3.7% too
// tall, 16:9 target = 1.85x too tall - both measured in headset). eyeH is the reference, not a constant.
// Even dimensions, clamped to something a driver will actually allocate.
void ComputeIdealTarget(unsigned eyeW, unsigned eyeH, unsigned* outW, unsigned* outH) noexcept
{
    unsigned w = eyeW;
    if (w < 1280u) w = 1280u;
    if (w > 10240u) w = 10240u;
    w &= ~1u;
    // Aspect from the headset's own recommended eye image; fall back to the common near-square window.
    const double aspect = (eyeW > 0u && eyeH > 0u) ? (static_cast<double>(eyeW) / static_cast<double>(eyeH))
                                                   : 0.9639;
    unsigned h = static_cast<unsigned>((static_cast<double>(w) / aspect) + 0.5);
    if (h < 1280u) h = 1280u;
    if (h > 10752u) h = 10752u;
    h &= ~1u;
    *outW = w;
    *outH = h;
}

// [RELIEF M0.6] forward decls (definitions live below, near the clear hook).
void NoteDepthDraw(ID3D11DeviceContext* ctx) noexcept;
void RunDepthCapturePipeline(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d) noexcept;
void CaptureLeaderDepthNow(ID3D11DeviceContext* ctx) noexcept;

// Find (or claim a free slot for) the per-DSV draw counter. Render-thread only, so no lock.
uint32_t* DepthDrawCounter(void* dsv) noexcept
{
    if (dsv == nullptr) return nullptr;
    for (auto& e : g_depthDraws) if (e.dsv == dsv) return &e.draws;
    for (auto& e : g_depthDraws) if (e.dsv == nullptr) { e.dsv = dsv; e.draws = 0; return &e.draws; }
    return nullptr;   // table full (shouldn't happen - a scene has a handful of DSVs)
}

// [RELIEF M0.6] attribute the draw + track the MOST-DRAWN depth (the true scene depth climbs fastest).
// KEY CHANGE: in stereo, CAPTURE THE DEPTH MID-SCENE - every 256 draws into the leader, while geometry
// is actively writing it. The burst-then-flat log proved the depth is trampled BEFORE any post-scene
// sample point (clear/UI), so it's copied out WHILE it's alive; whatever wipes it after is irrelevant.
void NoteDepthDraw(ID3D11DeviceContext* ctx) noexcept
{
    g_dbgDrawsTotal.fetch_add(1, std::memory_order_relaxed);
    if (g_boundDepthRes == nullptr) return;
    g_dbgDrawsAttrib.fetch_add(1, std::memory_order_relaxed);
    uint32_t* c = DepthDrawCounter(g_boundDepthRes);
    if (c == nullptr) return;
    ++*c;
    const uint32_t cnt = *c;
    if (cnt > g_busyDepthDraws && g_boundDepthTex != nullptr)
    {
        g_busyDepthDraws = cnt;
        if (g_busyDepthTex != g_boundDepthTex)
        {
            g_boundDepthTex->AddRef();
            ID3D11Texture2D* old = g_busyDepthTex;
            g_busyDepthTex = g_boundDepthTex;
            if (old != nullptr) old->Release();
        }
    }
    // Mid-scene live capture (stereo only; AER uses the clear-time path which isn't trampled). Copy the
    // currently-bound depth every 256 draws - the LATER copies each frame hold the fullest pre-trample depth.
    if (cnt >= 256u && (cnt % 256u) == 0u &&
        g_stereoUiActive.load(std::memory_order_acquire) && g_boundDepthTex != nullptr)
        CaptureLeaderDepthNow(ctx);
}

// Remember which depth buffer the game just bound, so the draw hooks can attribute geometry to it.
// ============================ [REFLCEN] ============================
// Reflection-capture census (2026-07-14, "head tracking moves the floor reflection slightly"):
// LE1's floor reflections are SceneCaptureReflectActor planar captures (BIOEngine.ini) - a second
// scene render from a MIRRORED game camera that never sees the render-side head transform, so it
// slides against the head-tracked floor. Step 1 of the fix = fingerprint the capture pass: tally
// world draws per render target per present; the capture = a non-backbuffer-sized RT taking
// hundreds of world draws. Passive, logs 3 presents every ~30s while Stereo 2 is active.
constexpr int kReflCenSlots = 24;
struct ReflCenEntry
{
    void* tex = nullptr;
    UINT w = 0, h = 0;
    int fmt = 0;
    unsigned draws = 0;
};
ReflCenEntry g_reflCen[kReflCenSlots] = {};      // render-thread only, reset each present
int g_reflCenCount = 0;
std::atomic<unsigned> g_reflCenLogs{0};

// [REFLCEN2] identity dump: the census found a constant-38-draw full-size LDR pass (suspect =
// reflection capture OR Scaleform HUD compositor). Photograph every distinct LDR census target
// mid-pass once (~present 1200+), so the suspect's CONTENT settles it: mirrored room vs HUD panels.
std::atomic_bool g_reflCenDumpDone{false};
std::atomic_bool g_reflCenDumpArmed{false};      // marker-gated (see ReflCenMaybeDumpCurrentRt)
void* g_reflCenDumped[6] = {};                   // render-thread only
int g_reflCenDumpedCount = 0;
void SfrDumpTextureBmp(ID3D11Texture2D* tex, const char* name) noexcept;   // fwd (defined below)

void ReflCenMaybeDumpCurrentRt(unsigned drawsOnRt) noexcept
{
    // [REFLCEN] BMP dumps MARKER-GATED 2026-07-15: they re-armed EVERY session and wrote up to six
    // full-RT BMPs (108MB each at 6144 square) synchronously on the render thread ~seconds after
    // load-in - the reported "freeze after 3-4 seconds on every load". The reflection bug they served is
    // FIXED (REFLRATE); dumps now need MELEVR_ENABLE_REFLCEN_DUMP.txt (polled in PresentHook).
    if (!g_reflCenDumpArmed.load(std::memory_order_relaxed)) return;
    if (g_reflCenDumpDone.load(std::memory_order_relaxed)) return;
    if (drawsOnRt != 5) return;                  // mid-pass: content exists, pass not over
    if (g_currentRtvFormat != DXGI_FORMAT_R8G8B8A8_UNORM &&
        g_currentRtvFormat != DXGI_FORMAT_R8G8B8A8_TYPELESS &&
        g_currentRtvFormat != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
    {
        return;                                   // LDR suspects only (scene HDR is known)
    }
    for (int i = 0; i < g_reflCenDumpedCount; ++i)
    {
        if (g_reflCenDumped[i] == g_currentRtvTexPtr) return;
    }
    if (g_reflCenDumpedCount >= 6)
    {
        g_reflCenDumpDone.store(true, std::memory_order_relaxed);
        return;
    }
    g_reflCenDumped[g_reflCenDumpedCount++] = g_currentRtvTexPtr;
    if (g_gameContext == nullptr) return;
    ID3D11RenderTargetView* rtv = nullptr;
    g_gameContext->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv == nullptr) return;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    if (res != nullptr)
    {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) &&
            tex != nullptr)
        {
            char name[48] = {};
            std::snprintf(name, sizeof(name), "reflcen_rt%d_%p", g_reflCenDumpedCount, g_currentRtvTexPtr);
            SfrDumpTextureBmp(tex, name);
            tex->Release();
        }
        res->Release();
    }
    rtv->Release();
}

void ReflCbMaybeXray(unsigned drawsOnRt) noexcept;   // fwd ([REFLCB] block below)

void ReflCenNoteDraw() noexcept                  // called per world draw (render thread)
{
    if (g_currentRtvTexPtr == nullptr) return;
    for (int i = 0; i < g_reflCenCount; ++i)
    {
        if (g_reflCen[i].tex == g_currentRtvTexPtr)
        {
            ++g_reflCen[i].draws;
            ReflCenMaybeDumpCurrentRt(g_reflCen[i].draws);
            ReflCbMaybeXray(g_reflCen[i].draws);
            return;
        }
    }
    if (g_reflCenCount < kReflCenSlots)
    {
        ReflCenEntry& e = g_reflCen[g_reflCenCount++];
        e.tex = g_currentRtvTexPtr;
        e.w = g_currentRtvWidth;
        e.h = g_currentRtvHeight;
        e.fmt = static_cast<int>(g_currentRtvFormat);
        e.draws = 1;
    }
}

// [REFLCB] X-ray: once per session, dump the VS constant buffers of one draw on the REFLECTION
// target and one draw on the MAIN HDR scene target (same present when possible) as floats - the
// two camera records + their relation define the mirror transform the fix must preserve.
void* g_reflSuspectTex = nullptr;                // render-thread only: last present's 38-draw fmt28 RT
void* g_reflSceneTex = nullptr;                  // last present's HDR fmt10 RT (main scene)
std::atomic<int> g_reflCbDumpsRefl{0};           // per-tag budgets: the shared cap let the main scene
std::atomic<int> g_reflCbDumpsMain{0};           // eat every slot before the reflection pass drew #3

void ReflCbDumpBoundVsCbs(const char* tag) noexcept
{
    if (g_gameContext == nullptr || g_gameDevice == nullptr) return;
    ID3D11Buffer* cbs[4] = {};
    g_gameContext->VSGetConstantBuffers(0, 4, cbs);
    for (UINT slot = 0; slot < 4; ++slot)
    {
        ID3D11Buffer* cb = cbs[slot];
        if (cb == nullptr) continue;
        D3D11_BUFFER_DESC bd = {};
        cb->GetDesc(&bd);
        D3D11_BUFFER_DESC sd = bd;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        sd.MiscFlags = 0;
        ID3D11Buffer* staging = nullptr;
        if (SUCCEEDED(g_gameDevice->CreateBuffer(&sd, nullptr, &staging)) && staging != nullptr)
        {
            g_gameContext->CopyResource(staging, cb);
            D3D11_MAPPED_SUBRESOURCE m = {};
            if (SUCCEEDED(g_gameContext->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
            {
                const float* f = static_cast<const float*>(m.pData);
                const UINT nf = (std::min)(bd.ByteWidth / 4u, 32u);
                char line[512] = {};
                int off = std::snprintf(line, sizeof(line), "[REFLCB] %s slot=%u bytes=%u f:",
                                        tag, slot, bd.ByteWidth);
                for (UINT k = 0; k < nf && off > 0 && off < static_cast<int>(sizeof(line)) - 16; ++k)
                {
                    off += std::snprintf(line + off, sizeof(line) - off, " %.4g", f[k]);
                }
                LogLine(line);
                g_gameContext->Unmap(staging, 0);
            }
            staging->Release();
        }
        cb->Release();
    }
}

void ReflCbMaybeXray(unsigned drawsOnRt) noexcept
{
    // Dump at several depths of the reflection pass (its 38 draws may interleave camera sources),
    // one depth for the main scene reference.
    if (g_currentRtvTexPtr == g_reflSuspectTex && g_reflSuspectTex != nullptr)
    {
        if ((drawsOnRt == 3 || drawsOnRt == 15 || drawsOnRt == 30) &&
            g_reflCbDumpsRefl.fetch_add(1, std::memory_order_relaxed) < 9)
        {
            char tag[48] = {};
            std::snprintf(tag, sizeof(tag), "REFLECTION d%u rt=%p", drawsOnRt, g_currentRtvTexPtr);
            ReflCbDumpBoundVsCbs(tag);
        }
    }
    else if (g_currentRtvTexPtr == g_reflSceneTex && g_reflSceneTex != nullptr)
    {
        if (drawsOnRt == 3 && g_reflCbDumpsMain.fetch_add(1, std::memory_order_relaxed) < 3)
        {
            char tag[48] = {};
            std::snprintf(tag, sizeof(tag), "MAINSCENE d%u rt=%p", drawsOnRt, g_currentRtvTexPtr);
            ReflCbDumpBoundVsCbs(tag);
        }
    }
}

// ============================ [REFLFIX] ============================
// Planar-reflection head-tracking fix (2026-07-14, v7 FREEZE rewrite). v1-v6 tried to COMPENSATE
// the reflection camera's rotation with a computed yaw delta - wrong model. The reflcb-9 X-ray logs
// (captured before ANY rewrite code existed) show the reflection camera's own VP matrix ALREADY
// changing frame-to-frame while the tracked camera's position barely moves - the base ENGINE
// already re-derives this camera's orientation from something entangled with the render-side head
// rotation, and the exact relationship (mirror-plane reflection math, which axis, what order) is
// not safely inferable from telemetry alone; two field attempts (opposite signs) both made it
// worse or unchanged, proving "add a compensating rotation" is the wrong tool regardless of sign.
// Requirement, stated directly: the reflection must not react to head tracking AT ALL.
// New approach needs no model of the coupling: FREEZE the record. The first time a matching 96-byte
// camera write is seen, cache its full VP matrix (f[0..15]) and position (f[20..22]). Every later
// matching write is FORCED to the frozen VP, regardless of what the game computed that frame - zero
// dependency on head yaw, pitch, or any other per-frame variable, by construction. Re-freeze only
// when the position drifts far enough to mean the player actually moved (not held constant by a
// gate on incoming writes - determined by comparing to the frozen position each time).
// Kill switch: MELEVR_DISABLE_REFLFIX.txt.
std::atomic_bool g_reflFixDisabled{false};
std::atomic_bool g_reflFixHaveInputs{false};
std::atomic<float> g_reflFixYawRad{0.0f};
std::atomic<float> g_reflFixPitchRad{0.0f};
std::atomic<float> g_reflFixCamX{0.0f};
std::atomic<float> g_reflFixCamY{0.0f};
std::atomic<float> g_reflFixCamZ{0.0f};
std::atomic<unsigned long long> g_reflFixApplied{0};      // freeze/re-freeze count
std::atomic<unsigned long long> g_reflFixHeld{0};          // frames forced to the frozen reference
std::atomic<unsigned> g_reflFixLogs{0};

// resource -> is-96-byte-CB cache (open-addressed; Map fires constantly, GetDesc only on first sight)
constexpr UINT kCb96Slots = 4096;
std::atomic<void*> g_cb96Keys[kCb96Slots] = {};
std::atomic<int> g_cb96Vals[kCb96Slots] = {};     // 0 unknown, 1 = 96-byte CB, -1 = anything else

int Cb96Lookup(void* res) noexcept
{
    const UINT mask = kCb96Slots - 1;
    UINT i = static_cast<UINT>((reinterpret_cast<uintptr_t>(res) >> 4) & mask);
    for (UINT probe = 0; probe < 64; ++probe, i = (i + 1) & mask)
    {
        void* cur = g_cb96Keys[i].load(std::memory_order_acquire);
        if (cur == res) return g_cb96Vals[i].load(std::memory_order_acquire);
        if (cur == nullptr) return 0;
    }
    return -1;   // probe overflow: treat as not-a-camera-CB
}

void Cb96Store(void* res, int val) noexcept
{
    const UINT mask = kCb96Slots - 1;
    UINT i = static_cast<UINT>((reinterpret_cast<uintptr_t>(res) >> 4) & mask);
    for (UINT probe = 0; probe < 64; ++probe, i = (i + 1) & mask)
    {
        void* cur = g_cb96Keys[i].load(std::memory_order_acquire);
        if (cur == res)
        {
            g_cb96Vals[i].store(val, std::memory_order_release);
            return;
        }
        if (cur == nullptr)
        {
            void* expected = nullptr;
            if (g_cb96Keys[i].compare_exchange_strong(expected, res, std::memory_order_acq_rel) ||
                expected == res)
            {
                g_cb96Vals[i].store(val, std::memory_order_release);
                return;
            }
        }
    }
}

// live 96-byte maps awaiting Unmap (tiny; camera CBs in flight at once are few)
std::mutex g_refl96MapMutex;
struct Refl96Live { ID3D11Resource* res; void* ptr; };
Refl96Live g_refl96Live[16] = {};

void Refl96Remember(ID3D11Resource* res, void* ptr) noexcept
{
    std::lock_guard<std::mutex> lock(g_refl96MapMutex);
    for (auto& e : g_refl96Live) { if (e.res == res) { e.ptr = ptr; return; } }
    for (auto& e : g_refl96Live) { if (e.res == nullptr) { e.res = res; e.ptr = ptr; return; } }
}

void* Refl96Take(ID3D11Resource* res) noexcept
{
    std::lock_guard<std::mutex> lock(g_refl96MapMutex);
    for (auto& e : g_refl96Live)
    {
        if (e.res == res)
        {
            void* p = e.ptr;
            e.res = nullptr;
            e.ptr = nullptr;
            return p;
        }
    }
    return nullptr;
}

// [REFLFIX] gate telemetry: distinguishes "no 96B writes seen" from "seen but which gate rejected".
std::atomic<unsigned long long> g_reflFixSeen{0};
std::atomic<unsigned long long> g_reflFixGateStruct{0};
std::atomic<unsigned long long> g_reflFixGateDz{0};
std::atomic<unsigned long long> g_reflFixGateXy{0};

// [REFLFIX v7] the frozen reference: captured once, reused verbatim every matching write until the
// player moves far enough that a re-freeze is warranted. Render-thread only (no lock needed - every
// caller of MaybeRewriteMirrorCameraRecord already runs on the render thread).
bool g_reflFixFrozenValid = false;
float g_reflFixFrozenVp[16] = {};
float g_reflFixFrozenPos[3] = {};

// Rewrite a staged 96-byte camera record IN PLACE if it is the reflection/capture camera. Returns
// true if edited (forced to the frozen reference).
bool MaybeRewriteMirrorCameraRecord(float* f) noexcept
{
    if (f == nullptr) return false;
    if (g_reflFixDisabled.load(std::memory_order_relaxed)) return false;
    if (!g_reflFixHaveInputs.load(std::memory_order_acquire)) return false;
    g_reflFixSeen.fetch_add(1, std::memory_order_relaxed);
    // structure: pad row (0,0,0,1) and position w == 1
    if (f[16] != 0.0f || f[17] != 0.0f || f[18] != 0.0f || f[19] != 1.0f || f[23] != 1.0f)
    {
        g_reflFixGateStruct.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // capture-camera signature: same room, meaningfully ABOVE or BELOW the tracked camera (the
    // engineering-deck capture sits ~500uu above at near-constant height - a fixed capture actor,
    // not a live mirror). Low bound 20uu keeps eye-shift/camera-bob from ever matching the MAIN
    // camera; this gate only picks the RIGHT record, it plays no role in what gets written to it.
    const float dzRaw = g_reflFixCamZ.load(std::memory_order_relaxed) - f[22];
    const float dz = dzRaw < 0.0f ? -dzRaw : dzRaw;
    if (!(dz > 20.0f && dz < 600.0f))
    {
        const unsigned long long rej = g_reflFixGateDz.fetch_add(1, std::memory_order_relaxed);
        if (rej < 10 || (rej % 15000) == 0)
        {
            char line[224] = {};
            std::snprintf(line, sizeof(line),
                          "[REFLFIX] dz-reject sample: rec=(%.0f,%.0f,%.0f) cam=(%.0f,%.0f,%.0f) dz=%.1f",
                          f[20], f[21], f[22],
                          g_reflFixCamX.load(std::memory_order_relaxed),
                          g_reflFixCamY.load(std::memory_order_relaxed),
                          g_reflFixCamZ.load(std::memory_order_relaxed), dzRaw);
            LogLine(line);
        }
        return false;
    }
    const float dx = g_reflFixCamX.load(std::memory_order_relaxed) - f[20];
    const float dy = g_reflFixCamY.load(std::memory_order_relaxed) - f[21];
    if (dx > 400.0f || dx < -400.0f || dy > 400.0f || dy < -400.0f)
    {
        g_reflFixGateXy.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Re-freeze if this is the first sighting, or the RECORD's own position (not the tracked
    // camera's) has moved far enough from the frozen baseline to mean the player actually walked
    // somewhere - not held head-still-but-looking-around, which keeps the record position ~fixed.
    bool needFreeze = !g_reflFixFrozenValid;
    if (!needFreeze)
    {
        const float mx = f[20] - g_reflFixFrozenPos[0];
        const float my = f[21] - g_reflFixFrozenPos[1];
        const float mz = f[22] - g_reflFixFrozenPos[2];
        if (mx * mx + my * my + mz * mz > 150.0f * 150.0f) needFreeze = true;
    }
    if (needFreeze)
    {
        std::memcpy(g_reflFixFrozenVp, f, sizeof(g_reflFixFrozenVp));
        g_reflFixFrozenPos[0] = f[20];
        g_reflFixFrozenPos[1] = f[21];
        g_reflFixFrozenPos[2] = f[22];
        g_reflFixFrozenValid = true;
        const unsigned long long n = g_reflFixApplied.fetch_add(1, std::memory_order_relaxed) + 1;
        char line[224] = {};
        std::snprintf(line, sizeof(line),
                      "[REFLFIX] (re)froze reflection camera #%llu pos=(%.0f,%.0f,%.0f)",
                      n, f[20], f[21], f[22]);
        LogLine(line);
        return false;   // this frame's write passes through unmodified; frozen from the NEXT write
    }

    std::memcpy(f, g_reflFixFrozenVp, sizeof(g_reflFixFrozenVp));
    const unsigned long long held = g_reflFixHeld.fetch_add(1, std::memory_order_relaxed) + 1;
    if (held <= 12 || (held % 4000) == 0)
    {
        char line[160] = {};
        std::snprintf(line, sizeof(line), "[REFLFIX] held frozen reflection camera #%llu", held);
        LogLine(line);
    }
    return true;
}

void ReflCenOnPresent(unsigned long long present) noexcept
{
    if (g_reflCenCount > 0 && (present % 1800) < 3 && g_reflCenLogs.load(std::memory_order_relaxed) < 3000)
    {
        for (int i = 0; i < g_reflCenCount; ++i)
        {
            const ReflCenEntry& e = g_reflCen[i];
            if (e.draws < 20) continue;          // world-ish targets only, skip one-off quads
            g_reflCenLogs.fetch_add(1, std::memory_order_relaxed);
            char line[224] = {};
            std::snprintf(line, sizeof(line), "[REFLCEN] present=%llu rt=%p %ux%u fmt=%d draws=%u",
                          present, e.tex, e.w, e.h, e.fmt, e.draws);
            LogLine(line);
        }
    }
    // [REFLCB] carry this present's pass identities into the next present for the X-ray:
    // reflection = the constant-38-draw LDR pass; main scene = the HDR pass.
    g_reflSuspectTex = nullptr;
    g_reflSceneTex = nullptr;
    for (int i = 0; i < g_reflCenCount; ++i)
    {
        const ReflCenEntry& e = g_reflCen[i];
        if (e.fmt == static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT) && e.draws > 100)
        {
            g_reflSceneTex = e.tex;
        }
        else if (e.fmt == static_cast<int>(DXGI_FORMAT_R8G8B8A8_UNORM) && e.draws >= 25 && e.draws <= 60)
        {
            g_reflSuspectTex = e.tex;
        }
    }
    g_reflCenCount = 0;
}

void STDMETHODCALLTYPE OMSetRenderTargetsHook(ID3D11DeviceContext* ctx, UINT num,
                                              ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv) noexcept
{
    g_boundDsv = dsv;
    // [RELIEF M0.1] resolve the DSV's resource once per bind - draws are attributed to the RESOURCE, so
    // a clear coming through a different DSV object onto the same texture still finds the counter.
    // [M0.5] also keep an AddRef'd TEXTURE interface for the bound depth (QI only when the resource
    // changes - hot path) so the UI-latch capture can snapshot the scene depth mid-frame.
    if (dsv != nullptr)
    {
        ID3D11Resource* dres = nullptr;
        dsv->GetResource(&dres);
        g_boundDepthRes = dres;
        if (dres != nullptr && dres != g_lastDepthResRaw)
        {
            ID3D11Texture2D* t = nullptr;
            dres->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&t));
            ID3D11Texture2D* old = g_boundDepthTex;
            g_boundDepthTex = t;                 // may be null if QI failed; capture null-guards
            g_lastDepthResRaw = dres;
            if (old != nullptr) old->Release();
        }
        if (dres != nullptr) dres->Release();   // identity only for g_boundDepthRes; tex holds its own ref
    }
    else
    {
        g_boundDepthRes = nullptr;
    }
    // Track the first bound RTV's format + width so the draw hooks can discriminate the LDR backbuffer
    // (UI target) from the HDR world intermediates cheaply. Read-only; release everything that gets AddRef'd.
    if (num > 0 && rtvs != nullptr && rtvs[0] != nullptr)
    {
        ID3D11Resource* res = nullptr;
        rtvs[0]->GetResource(&res);
        if (res != nullptr)
        {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex != nullptr)
            {
                D3D11_TEXTURE2D_DESC d = {};
                tex->GetDesc(&d);
                g_currentRtvWidth = d.Width;
                g_currentRtvHeight = d.Height;
                g_currentRtvTexPtr = tex;      // identity only ([QPOST] dup keys); never dereferenced
                g_currentRtvFormat = d.Format;
                tex->Release();
            }
            res->Release();
        }
    }
    else
    {
        g_currentRtvWidth = 0;
        g_currentRtvHeight = 0;
        g_currentRtvTexPtr = nullptr;
        g_currentRtvFormat = DXGI_FORMAT_UNKNOWN;
    }
    if (g_originalOMSetRenderTargets != nullptr) g_originalOMSetRenderTargets(ctx, num, rtvs, dsv);
}

// A UI draw = full-viewport + LDR backbuffer (R8G8B8A8_UNORM, full width) + alpha-blended. Scaleform
// HUD/menus/text render this way; the world renders half-width into HDR intermediates and the world's
// full-screen composite is opaque (fails the blend gate). Fills vpOut with the full viewport when true.
bool IsUiDrawNow(ID3D11DeviceContext* ctx, D3D11_VIEWPORT& vpOut) noexcept
{
    vpOut = {};
    const UINT bbW = g_sceneRenderW.load(std::memory_order_acquire);   // == backbuffer width on LE1
    if (!(bbW > 0 && g_currentRtvFormat == DXGI_FORMAT_R8G8B8A8_UNORM && g_currentRtvWidth == bbW))
        return false;
    UINT num = 1;
    ctx->RSGetViewports(&num, &vpOut);
    if (!(num == 1 && vpOut.Width > static_cast<float>(bbW) * 0.75f)) return false;
    ID3D11BlendState* bs = nullptr; float bf[4] = {}; UINT sampleMask = 0;
    ctx->OMGetBlendState(&bs, bf, &sampleMask);
    bool blended = false;
    if (bs != nullptr) { D3D11_BLEND_DESC bd = {}; bs->GetDesc(&bd); blended = bd.RenderTarget[0].BlendEnable != FALSE; bs->Release(); }
    return blended;
}

// Per-eye duplicate of a UI draw, preserving aspect (halve W and H, center vertically) so it fuses at
// screen depth without a 2:1 vertical stretch. Caller passes a thunk that re-issues the real draw.
// ME1 does NOT hook RSSetViewports, so calling ctx->RSSetViewports directly here is safe (no recursion).
//
// [CINERIP 2026-08-09] The [CINEUI] band-crop compensation that used to live here is GONE. It only
// existed because a SQUARE render target forced the submit to stretch the cine band 1.78x vertically,
// which stretched the UI with it. Stereo 2 renders 16:9 now (ME2's shape), the engine's 16:9 cine
// constraint fills that buffer exactly, there is no band and no stretch, so there is nothing to
// pre-compress. Do not reintroduce a UI squash without first reintroducing a non-16:9 target.
template <typename DrawThunk>
void DupUiDraw(ID3D11DeviceContext* ctx, const D3D11_VIEWPORT& vp, DrawThunk&& draw) noexcept
{
    g_cntUiDup.fetch_add(1, std::memory_order_relaxed);
    D3D11_VIEWPORT h = vp;
    h.Width = vp.Width * 0.5f;
    h.Height = vp.Height * 0.5f;
    // 0.25 centers the half-height UI; subtract the user shift to move it UP so the crosshair meets the shots.
    h.TopLeftY = vp.TopLeftY +
                 vp.Height * (0.25f - g_stereoUiYShift.load(std::memory_order_relaxed));
    h.TopLeftX = vp.TopLeftX;                                  // left eye
    ctx->RSSetViewports(1, &h);
    draw();
    h.TopLeftX = vp.TopLeftX + vp.Width * 0.5f;                // right eye
    ctx->RSSetViewports(1, &h);
    draw();
    ctx->RSSetViewports(1, &vp);                              // restore full viewport
}

// Create/resize the sampleable R24 depth copy to match the scene depth.
bool EnsureDepthCopy(const D3D11_TEXTURE2D_DESC& srcDesc) noexcept
{
    if (g_gameDevice == nullptr) return false;
    if (g_depthCopy != nullptr && g_depthCopyW == srcDesc.Width && g_depthCopyH == srcDesc.Height &&
        g_depthCopyFmt == srcDesc.Format) return true;
    // [RELIEF M0.2] copy + SRV formats FOLLOW the source. A hardcoded R24G8 copy of a D32 source is an
    // illegal CopyResource that D3D silently drops -> uninitialized reads (the flat-1.0 probe bug).
    DXGI_FORMAT copyFmt, srvFmt;
    int decode;
    switch (srcDesc.Format)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        copyFmt = DXGI_FORMAT_R24G8_TYPELESS; srvFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; decode = 0; break;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        copyFmt = DXGI_FORMAT_R32_TYPELESS; srvFmt = DXGI_FORMAT_R32_FLOAT; decode = 1; break;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        copyFmt = DXGI_FORMAT_R32G8X24_TYPELESS; srvFmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; decode = 2; break;
    default:
        {
            static int s_fmtLogs = 0;
            if (s_fmtLogs++ < 3) LogLine("[DIBR_DEPTH] unsupported depth format " + std::to_string(static_cast<int>(srcDesc.Format)));
            return false;
        }
    }
    SafeRelease(g_depthSrv);
    SafeRelease(g_depthCopy);
    SafeRelease(g_depthPublished);
    g_depthReady.store(false, std::memory_order_release);   // [M0.4] stays false until the first VERIFIED-real promote
    D3D11_TEXTURE2D_DESC cd = srcDesc;
    cd.Format = copyFmt;
    cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cd.Usage = D3D11_USAGE_DEFAULT;
    cd.CPUAccessFlags = 0;
    cd.MiscFlags = 0;
    cd.MipLevels = 1;
    cd.ArraySize = 1;
    HRESULT hr = g_gameDevice->CreateTexture2D(&cd, nullptr, &g_depthCopy);
    if (FAILED(hr) || g_depthCopy == nullptr) { LogLine("[DIBR_DEPTH] copy tex create failed hr=" + HexHRESULT(hr)); return false; }
    hr = g_gameDevice->CreateTexture2D(&cd, nullptr, &g_depthPublished);   // [M0.4] the good-depth target the SRV views
    if (FAILED(hr) || g_depthPublished == nullptr) { LogLine("[DIBR_DEPTH] published tex create failed hr=" + HexHRESULT(hr)); SafeRelease(g_depthCopy); return false; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = srvFmt;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    hr = g_gameDevice->CreateShaderResourceView(g_depthPublished, &sd, &g_depthSrv);   // [M0.4] SRV on PUBLISHED
    if (FAILED(hr) || g_depthSrv == nullptr) { LogLine("[DIBR_DEPTH] srv create failed hr=" + HexHRESULT(hr)); SafeRelease(g_depthCopy); SafeRelease(g_depthPublished); return false; }
    SafeRelease(g_depthStaging[0]); SafeRelease(g_depthStaging[1]);
    SafeRelease(g_depthGpuSnap[0]); SafeRelease(g_depthGpuSnap[1]);
    g_depthStagingInFlight[0] = g_depthStagingInFlight[1] = false; g_depthStagingWrite = 0;
    D3D11_TEXTURE2D_DESC stg = cd; stg.BindFlags = 0; stg.Usage = D3D11_USAGE_STAGING; stg.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g_gameDevice->CreateTexture2D(&stg, nullptr, &g_depthStaging[0]);   // depth-probe readback ring (non-blocking)
    g_gameDevice->CreateTexture2D(&stg, nullptr, &g_depthStaging[1]);
    g_gameDevice->CreateTexture2D(&cd, nullptr, &g_depthGpuSnap[0]);    // [M0.4b] GPU snapshot ring (promote source)
    g_gameDevice->CreateTexture2D(&cd, nullptr, &g_depthGpuSnap[1]);
    g_depthCopyW = srcDesc.Width; g_depthCopyH = srcDesc.Height;
    g_depthCopyFmt = srcDesc.Format;
    g_depthDecodeKind = decode;
    char fbuf[144];
    std::snprintf(fbuf, sizeof(fbuf), "[DIBR_DEPTH] sampleable copy created %ux%u srcFmt=%d copyFmt=%d decode=%d",
                  srcDesc.Width, srcDesc.Height, static_cast<int>(srcDesc.Format), static_cast<int>(copyFmt), decode);
    LogLine(fbuf);
    return true;
}

// [RELIEF M0.6] PROBE + PROMOTE the CURRENT g_depthCopy: verify real depth (spread), auto-fit the viz
// window, and promote the good frame to g_depthPublished (hold-last-good otherwise). Split out from the
// capture so stereo can capture many times mid-scene but probe/promote ONCE per frame (from the fullest
// copy) - promoting every mid-scene copy flashed white as early sparse copies briefly published.
void ProbeAndPromote(ID3D11DeviceContext* ctx) noexcept
{
    const UINT dW = g_depthCopyW, dH = g_depthCopyH;
    if (ctx == nullptr || dW == 0 || dH == 0) return;
    const uint32_t pc = g_depthProbeLogs.fetch_add(1, std::memory_order_relaxed);
    if (g_depthStaging[0] == nullptr || g_depthStaging[1] == nullptr) return;
    const int w = g_depthStagingWrite;
    const int r = 1 - w;
    ctx->CopyResource(g_depthStaging[w], g_depthCopy);         // CPU-readable twin (probe)
    if (g_depthGpuSnap[w] != nullptr) ctx->CopyResource(g_depthGpuSnap[w], g_depthCopy);   // [M0.4b] GPU twin (promote src)
    g_depthStagingInFlight[w] = true;
    g_depthStagingWrite = r;   // next copy goes into the slot about to be read
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (!g_depthStagingInFlight[r] ||
        FAILED(ctx->Map(g_depthStaging[r], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return;
    g_depthStagingInFlight[r] = false;
    // [RELIEF M0.2] decode follows the copy format (0=24-bit uint, 1=float32, 2=float32 in 8-byte texel).
    const int decodeKind = g_depthDecodeKind;
    auto sample = [&](UINT x, UINT y) -> float {
        const uint8_t* row = static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch;
        if (decodeKind == 1) return *reinterpret_cast<const float*>(row + static_cast<size_t>(x) * 4u);
        if (decodeKind == 2) return *reinterpret_cast<const float*>(row + static_cast<size_t>(x) * 8u);
        const uint32_t v = *reinterpret_cast<const uint32_t*>(row + static_cast<size_t>(x) * 4u);
        return static_cast<float>(v & 0x00FFFFFFu) / 16777215.0f;
    };
    const float center = sample(dW / 2, dH / 2);
    const float tl = sample(24, 24);
    const float br = sample(dW - 24, dH - 24);
    const float tr = sample(dW - 24, 24);
    const float lc = sample(dW / 4, dH / 2);        // [RELIEF M0] left-eye center in SBS
    const float rc = sample((dW * 3) / 4, dH / 2);  // [RELIEF M0] right-eye center in SBS
    g_probeCenter.store(center, std::memory_order_relaxed);
    g_probeTL.store(tl, std::memory_order_relaxed);
    g_probeBR.store(br, std::memory_order_relaxed);
    g_probeTR.store(tr, std::memory_order_relaxed);
    g_probeLC.store(lc, std::memory_order_relaxed);
    g_probeRC.store(rc, std::memory_order_relaxed);
    // [RELIEF M0.5c] content gate. Two failure modes to reject: (a) the pooled corpse = ALL taps at the
    // exact clear value (1.0 or 0.0); (b) a uniform fill (all taps identical but not 1.0/0.0). Real scene
    // depth is NOT at the clear extreme AND has at least a hair of variation (this engine packs depth into
    // ~0.95-1.0, so the spread is small - 0.005 was too strict and rejected real frames). good = below the
    // far-clear AND some spread.
    float mn = center, mx = center;
    const float taps[5] = { tl, br, tr, lc, rc };
    for (float t : taps) { if (t < mn) mn = t; if (t > mx) mx = t; }
    const float spread = mx - mn;
    const bool notCorpse = (mn < 0.99990f) && (mx > 0.00010f);   // not all-1.0 and not all-0.0
    const bool good = notCorpse && (spread > 0.00050f);          // real geometry varies at least this much
    if (good && g_depthVizShow.load(std::memory_order_relaxed) && g_depthVizAutoFit.load(std::memory_order_relaxed))
    {
        // [M0.3b] auto-fit the greyscale window to the live range (kills "all white"). Skipped when the
        // user takes manual control of the window (to read where the subject sits vs the environment).
        const float pad = (mx - mn) * 0.05f;
        g_depthFar.store(mn - pad, std::memory_order_relaxed);   // low d -> black
        g_depthNear.store(mx + pad, std::memory_order_relaxed);  // high d -> white
        g_depthFlip.store(0.0f, std::memory_order_relaxed);
        g_depthGamma.store(1.0f, std::memory_order_relaxed);
    }
    if (pc < 45u || (pc % 240u) == 0u)   // first 45, then ~1 per few seconds so ongoing promotes stay visible
    {
        char buf[224];
        std::snprintf(buf, sizeof(buf),
                      "[DIBR_PROBE] depth center=%.5f TL=%.5f BR=%.5f TR=%.5f L=%.5f R=%.5f spread=%.5f %s",
                      center, tl, br, tr, lc, rc, mx - mn, good ? "PROMOTE" : "hold");
        LogLine(buf);
    }
    ctx->Unmap(g_depthStaging[r], 0);
    // [M0.4b] Promote from the GPU snapshot twin (GPU->GPU, ~free), NOT the staging texture.
    if (good && g_depthPublished != nullptr && g_depthGpuSnap[r] != nullptr)
    {
        ctx->CopyResource(g_depthPublished, g_depthGpuSnap[r]);
        g_depthReady.store(true, std::memory_order_release);
    }
}

// [RELIEF M0.6] copy 'tex' into the scratch g_depthCopy. Capture only - no stage, no promote. For stereo
// this is called many times mid-scene; g_depthCopy ends the frame holding the fullest (last) copy.
void CaptureDepthToScratch(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d) noexcept
{
    if (ctx == nullptr || tex == nullptr || !EnsureDepthCopy(d)) return;
    ctx->CopyResource(g_depthCopy, tex);
    g_depthCopyCount.fetch_add(1, std::memory_order_relaxed);
}

// [RELIEF M0.6] AER clear-time vantage: capture + probe/promote together (AER depth isn't trampled, one
// capture per clear is enough). Stereo does NOT use this - it captures mid-scene and promotes at present.
void RunDepthCapturePipeline(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const D3D11_TEXTURE2D_DESC& d) noexcept
{
    // [RELIEF_PERF] Big-res throttle (perf report 2026-07-15, log-proven at 6144x6144): each pipeline
    // run moves several full-res depth copies (scratch + staging + GPU snap + promote) - ~600MB per run at
    // 6144, measured ~28 runs/s = the dominant depth-pop bandwidth cost. The warp happily tolerates depth a
    // frame or two stale (it already lags one frame by design), so at >=4096 capture every OTHER qualifying
    // clear. At the 3072 design point nothing changes.
    if (d.Width >= 4096)
    {
        static std::atomic<unsigned> s_bigResSkip{0};
        if ((s_bigResSkip.fetch_add(1, std::memory_order_relaxed) & 1u) != 0u) return;
    }
    CaptureDepthToScratch(ctx, tex, d);
    ProbeAndPromote(ctx);
}

// [RELIEF M0.6] STEREO vantage: copy the CURRENTLY-BOUND depth mid-scene (called from NoteDepthDraw at
// draw-count milestones), while geometry is still actively writing it - so the later trample can't reach
// the captured copy. Capture ONLY; the fullest copy survives to present, where ProbeAndPromote runs once/frame.
void CaptureLeaderDepthNow(ID3D11DeviceContext* ctx) noexcept
{
    if (ctx == nullptr) return;
    ID3D11Texture2D* tex = g_boundDepthTex;   // the depth being drawn RIGHT NOW (alive)
    if (tex == nullptr) return;
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    const UINT renderW = g_sceneRenderW.load(std::memory_order_acquire);
    const UINT renderH = g_sceneRenderH.load(std::memory_order_acquire);
    if (renderW == 0 || d.Width != renderW || d.Height != renderH || d.SampleDesc.Count != 1) return;
    static int s_uiCapLogs = 0;
    if (s_uiCapLogs < 3)
    {
        ++s_uiCapLogs;
        char b[128];
        std::snprintf(b, sizeof(b), "[DIBR_DEPTH] mid-scene live capture %ux%u fmt=%d",
                      d.Width, d.Height, static_cast<int>(d.Format));
        LogLine(b);
    }
    CaptureDepthToScratch(ctx, tex, d);   // capture only; ProbeAndPromoteStereoAtPresent does the rest
}

// [RELIEF M0.6] called once per present (stereo): probe + promote the fullest mid-scene capture that
// survived in g_depthCopy. One promote per frame = no white flash from early sparse mid-scene copies.
void ProbeAndPromoteStereoAtPresent() noexcept
{
    if (!g_stereoUiActive.load(std::memory_order_acquire) ||
        !g_depthMapEnabled.load(std::memory_order_relaxed) || g_gameContext == nullptr) return;
    ProbeAndPromote(g_gameContext);
}

// Capture the scene depth into the sampleable copy BEFORE the game wipes it. Menu-gated.
// [DYNRES_RT] The observable for the supersample test. "It was set to 1.5 and didn't crash" proves nothing;
// this has to prove whether the SCENE render target actually got bigger.
//
// v1 of this tracked max-width and max-height in two independent atomics and printed them as one size. That
// was wrong: it welded the width of one surface to the height of another and reported a 3840x3264 target
// that does not exist. Sizes are tracked as PAIRS now, and the whole distinct set prints per second rather
// than a single "max", because inferring one number from a population is what produced the phantom in the
// first place. Non-MSAA depth surfaces only; runs in every VR mode, outside the depth-capture gates.
// The RT size alone can't distinguish "field is inert" from "field is live but clamped above 1.0", because
// UE3 dynamic res renders into a SUB-RECT of a fixed-size target rather than reallocating it. The bound
// viewport at scene-depth-clear time is that sub-rect. Read it (RSGetViewports - a pure getter, no vtable
// hook, no recursion) and tally it next to the depth size. Below 1.0 the viewport must shrink while the
// depth surface stays put; that is the signature of a live-but-clamped field.
struct DepthSizeTally
{
    UINT w = 0;
    UINT h = 0;
    UINT vpW = 0;
    UINT vpH = 0;
    UINT clears = 0;
};

void DynResRtObserve(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv) noexcept
{
    constexpr int kMaxDistinct = 8;
    static CRITICAL_SECTION s_cs;
    static bool s_csInit = false;
    static DepthSizeTally s_seen[kMaxDistinct] = {};
    static int s_seenCount = 0;
    static ULONGLONG s_lastTick = 0;

    if (dsv == nullptr) return;

    if (!s_csInit) { InitializeCriticalSection(&s_cs); s_csInit = true; }

    UINT w = 0, h = 0, vpW = 0, vpH = 0;
    bool usable = false;
    {
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        if (res == nullptr) return;
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex != nullptr)
        {
            D3D11_TEXTURE2D_DESC d = {};
            tex->GetDesc(&d);
            if (d.SampleDesc.Count == 1 && d.Width >= 512)
            {
                w = d.Width;
                h = d.Height;
                usable = true;
            }
            tex->Release();
        }
        res->Release();
    }

    if (usable && ctx != nullptr)
    {
        D3D11_VIEWPORT vps[8] = {};
        UINT n = 8;
        ctx->RSGetViewports(&n, vps);   // getter only; ME1 does not hook RSSetViewports (see note at ~2613)
        if (n > 0)
        {
            vpW = static_cast<UINT>(vps[0].Width);
            vpH = static_cast<UINT>(vps[0].Height);
        }
    }

    std::string report;
    bool emit = false;

    EnterCriticalSection(&s_cs);
    if (usable)
    {
        int slot = -1;
        for (int i = 0; i < s_seenCount; ++i)
        {
            if (s_seen[i].w == w && s_seen[i].h == h &&
                s_seen[i].vpW == vpW && s_seen[i].vpH == vpH) { slot = i; break; }
        }
        if (slot < 0 && s_seenCount < kMaxDistinct)
        {
            slot = s_seenCount++;
            s_seen[slot].w = w;
            s_seen[slot].h = h;
            s_seen[slot].vpW = vpW;
            s_seen[slot].vpH = vpH;
            s_seen[slot].clears = 0;
        }
        if (slot >= 0) ++s_seen[slot].clears;
    }

    const ULONGLONG now = GetTickCount64();
    if (s_lastTick == 0 || now - s_lastTick >= 1000)
    {
        s_lastTick = now;
        if (s_seenCount > 0)
        {
            emit = true;
            for (int i = 0; i < s_seenCount; ++i)
            {
                char one[96];
                std::snprintf(one, sizeof(one), " rt=%ux%u/vp=%ux%u(x%u)",
                              s_seen[i].w, s_seen[i].h, s_seen[i].vpW, s_seen[i].vpH, s_seen[i].clears);
                report += one;
            }
            s_seenCount = 0;
        }
    }
    LeaveCriticalSection(&s_cs);

    if (!emit) return;

    const UINT bbW = g_sceneRenderW.load(std::memory_order_acquire);
    const UINT bbH = g_sceneRenderH.load(std::memory_order_acquire);
    char head[160];
    std::snprintf(head, sizeof(head), "[DYNRES_RT] frac=%.3f backbuffer=%ux%u surfaces:",
                  static_cast<double>(::MELEVR::Config::Get().dynResFraction), bbW, bbH);
    LogLine(std::string(head) + report);
}

// ============================ [SFR-CAP] ============================
// Render-side pass capture (2026-07-14 rework): the FVC Draw hook lives on the GAME thread
// (threadcheck 16:45: fvcDraw != present) and only ENQUEUES render work (its second Draw returns in
// ~0.4ms), so immediate-context copies from there raced the render thread - four good frames, then
// an uncatchable death. All capture now happens ON the render thread, self-aligned by markers in
// the command stream itself:
//   pass 0 = backbuffer at the first scene-sized depth CLEAR that follows >=1 uber-composite draw
//            this present (= the replay pass's scene starting; pass 0's backbuffer is final, UI in);
//   pass 1 = backbuffer at Present.
// Armed a few presents at a time by the game thread each time it enqueues a replay, so the machine
// only runs while SFR is actually producing second passes. SfrNoteQuadDraw (the composite counter
// feed) lives further down, after the [UBER] shader-hash table it depends on.
std::atomic<int> g_sfrCapturePresentsLeft{0};
std::atomic<unsigned> g_sfrComposThisPresent{0};
// [ME2PORT v2] backbuffer-dirty flag: set when a draw lands on the scene-sized RGBA8 RTV (a composite
// or UI actually wrote the backbuffer), consumed by the pass-0 snapshot. Two jobs: (1) the reflection
// captures ME1 boosts to per-frame do ~9 extra scene-sized depth clears per present with NOTHING new
// on the backbuffer - without this gate the every-clear snapshot copied 5120^2 x11 per present
// (~43GB/s, the 300-850ms GPU spikes of the 16:09 run); (2) only clears that FOLLOW a backbuffer
// write can change the snapshot, so skipping clean ones is lossless by construction.
std::atomic_bool g_sfrBbDirty{false};
std::atomic_bool g_sfrPass0Captured{false};
std::atomic<unsigned> g_sfrCapLogs{0};
// [CLEANVRCINE] cine pass-boundary mode (published per frame by xr_session = Stereo 2 active in a
// convo/cutscene). Conversations run a DIFFERENT post chain - the gameplay composite hash never
// appears there, so the composite-gated boundary never fires, no pairs publish, and the submit
// would hold the last gameplay pair = frozen convo image. Cine boundary = composite-INDEPENDENT
// backbuffer-batch trigger, see SfrCineNoteDraw.
std::atomic_bool g_sfrCineBoundary{false};
std::atomic<unsigned> g_sfrCineDiagLogs{0};
// backbuffer-batch trigger state (render-thread only; definition/comment at SfrCineNoteDraw)
void* g_sfrCineInBbBatch = nullptr;
unsigned g_sfrCineBbBatches = 0;

bool SfrCaptureArmed() noexcept
{
    return g_sfrCapturePresentsLeft.load(std::memory_order_acquire) > 0;
}

// ============================ [REFLLOG] ============================
// Log-ONLY evidence pass for the reflection/head-tracking coupling (2026-07-15, mandated after
// reflfix v1-v7 all failed in-headset: derive the transform OFFLINE from data, then build ONCE).
// Armed by MELEVR_ENABLE_REFLLOG.txt; edits NOTHING. Emits:
//   [REFLLOG] p=<present> HEAD yaw=<rad> pitch=<rad> cam=(x,y,z) sfrArmed=<0|1>   (per present)
//   [REFLLOG] p=.. s=<seq> src=.. MAIN|CAPT pass0done=.. pos=(..) vp=[16 floats]  (per 96B write)
// tag=MAIN: record position ~= tracked camera (the per-pass main camera; SFR writes TWO per
// present at +/-halfEye). tag=CAPT: the reflection capture camera (the PROVEN v5-v7 gate:
// 20<dz<600, dxy<400). Anything else logs a position-only OTHER line (capped) so an unexpected
// third camera still shows up. What each hypothesis looks like in this data:
//   (a) per-eye capture records  -> two CAPT lines per present with distinct pos/VP;
//   (c) mirror-conjugated head   -> CAPT VP varies with HEAD yaw during head-only motion;
//   (b) sampling-side coupling   -> CAPT VP STABLE during head-only motion yet the image shakes.
std::atomic_bool g_reflLogArmed{false};
std::atomic<unsigned> g_reflLogLines{0};
std::atomic<unsigned> g_reflLogOtherLines{0};
std::atomic<int> g_reflLogSeq{0};                 // per-present write sequence (reset at Present)
constexpr unsigned kReflLogMaxLines = 60000;      // ~3.5 min at typical rates, then silent

// Write-path 96B tracking is wanted when the (marker-gated) rewrite could run OR the log is armed.
bool ReflWriteTrackingWanted() noexcept
{
    if (!g_reflFixHaveInputs.load(std::memory_order_acquire)) return false;
    if (!g_reflFixDisabled.load(std::memory_order_relaxed)) return true;
    return g_reflLogArmed.load(std::memory_order_relaxed);
}

void ReflLogRecord(const float* f, const char* src) noexcept
{
    if (f == nullptr || !g_reflLogArmed.load(std::memory_order_relaxed)) return;
    if (!g_reflFixHaveInputs.load(std::memory_order_acquire)) return;
    if (g_reflLogLines.load(std::memory_order_relaxed) >= kReflLogMaxLines) return;
    // camera-record structure gate (same as the rewrite: pad row 0,0,0,1 + position w == 1)
    if (f[16] != 0.0f || f[17] != 0.0f || f[18] != 0.0f || f[19] != 1.0f || f[23] != 1.0f) return;
    const float dx = g_reflFixCamX.load(std::memory_order_relaxed) - f[20];
    const float dy = g_reflFixCamY.load(std::memory_order_relaxed) - f[21];
    const float dzRaw = g_reflFixCamZ.load(std::memory_order_relaxed) - f[22];
    const float dz = dzRaw < 0.0f ? -dzRaw : dzRaw;
    const float dxy = std::sqrt(dx * dx + dy * dy);
    const char* tag = nullptr;
    if (dz <= 20.0f && dxy <= 50.0f) tag = "MAIN";
    else if (dz > 20.0f && dz < 600.0f && dxy < 400.0f) tag = "CAPT";
    const int seq = g_reflLogSeq.fetch_add(1, std::memory_order_relaxed);
    const unsigned long long present = g_presentCount.load(std::memory_order_relaxed);
    if (tag == nullptr)
    {
        const unsigned o = g_reflLogOtherLines.fetch_add(1, std::memory_order_relaxed);
        if (o >= 200 && (o % 500) != 0) return;
        g_reflLogLines.fetch_add(1, std::memory_order_relaxed);
        char line[224] = {};
        std::snprintf(line, sizeof(line),
                      "[REFLLOG] p=%llu s=%d src=%s OTHER pos=(%.1f,%.1f,%.1f) dz=%.1f dxy=%.1f",
                      present, seq, src, f[20], f[21], f[22], dzRaw, dxy);
        LogLine(line);
        return;
    }
    g_reflLogLines.fetch_add(1, std::memory_order_relaxed);
    char line[512] = {};
    std::snprintf(line, sizeof(line),
                  "[REFLLOG] p=%llu s=%d src=%s %s pass0done=%d pos=(%.2f,%.2f,%.2f) "
                  "vp=[%.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g]",
                  present, seq, src, tag,
                  g_sfrPass0Captured.load(std::memory_order_acquire) ? 1 : 0,
                  f[20], f[21], f[22],
                  f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7],
                  f[8], f[9], f[10], f[11], f[12], f[13], f[14], f[15]);
    LogLine(line);
}

void ReflLogOnPresent() noexcept   // render thread, from PresentHook (after the marker poll)
{
    g_reflLogSeq.store(0, std::memory_order_relaxed);
    if (!g_reflLogArmed.load(std::memory_order_relaxed)) return;
    if (!g_reflFixHaveInputs.load(std::memory_order_acquire)) return;
    if (g_reflLogLines.load(std::memory_order_relaxed) >= kReflLogMaxLines) return;
    g_reflLogLines.fetch_add(1, std::memory_order_relaxed);
    char line[224] = {};
    std::snprintf(line, sizeof(line),
                  "[REFLLOG] p=%llu HEAD yaw=%.6f pitch=%.6f cam=(%.2f,%.2f,%.2f) sfrArmed=%d",
                  g_presentCount.load(std::memory_order_relaxed),
                  g_reflFixYawRad.load(std::memory_order_relaxed),
                  g_reflFixPitchRad.load(std::memory_order_relaxed),
                  g_reflFixCamX.load(std::memory_order_relaxed),
                  g_reflFixCamY.load(std::memory_order_relaxed),
                  g_reflFixCamZ.load(std::memory_order_relaxed),
                  SfrCaptureArmed() ? 1 : 0);
    LogLine(line);
}

// [SFR-DUMP] state + writer live further down (needs nothing from here); declared early for the
// capture callbacks below.
std::atomic_bool g_sfrDumpArmed{false};
std::atomic_bool g_sfrDumpDone{false};
void SfrDumpTextureBmp(ID3D11Texture2D* tex, const char* name) noexcept;

void SfrOnSceneDepthClear() noexcept   // render thread, from ClearDepthStencilViewHook
{
    if (MELEVR::D3DCapture::CaptureStereoPass(0))
    {
        g_sfrPass0Captured.store(true, std::memory_order_release);
        const unsigned n = g_sfrCapLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 8 || (n % 600) == 0)
        {
            LogLine("[SFR-CAP] pass0 captured at pass-boundary clear (composites=" +
                    std::to_string(g_sfrComposThisPresent.load(std::memory_order_relaxed)) + ")");
        }
        // [SFR-DUMP] MARKER-GATED 2026-07-15 (was: auto-arm at the 300th capture EVERY session = four
        // synchronous 108MB BMP writes ~5s after every load at 6144 square - the other half of the
        // load-in freeze). The red-eye bug it diagnosed is fixed; opt back in with the marker.
        if (n == 300 && MarkerEnabled(L"MELEVR_ENABLE_SFR_DUMP.txt") &&
            !g_sfrDumpDone.load(std::memory_order_acquire))
        {
            g_sfrDumpArmed.store(true, std::memory_order_release);
            ID3D11Texture2D* pass0 = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_stereoPassMutex);
                pass0 = g_stereoPassTex[g_stereoWriteBank][0];
                if (pass0 != nullptr) pass0->AddRef();
            }
            if (pass0 != nullptr)
            {
                SfrDumpTextureBmp(pass0, "pass0_boundary");
                pass0->Release();
            }
        }
    }
}

void SfrOnPresent() noexcept           // render thread, from PresentHook (before the real present)
{
    if (!SfrCaptureArmed()) return;
    const bool hadPass0 = g_sfrPass0Captured.load(std::memory_order_acquire);
    const bool dumpNow = hadPass0 && g_sfrDumpArmed.exchange(false, std::memory_order_acq_rel);
    if (dumpNow && g_gameSwapChain != nullptr)
    {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(g_gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) &&
            bb != nullptr)
        {
            SfrDumpTextureBmp(bb, "backbuffer_present");
            bb->Release();
        }
    }
    if (hadPass0)
    {
        MELEVR::D3DCapture::CaptureStereoPass(1);   // second capture publishes the stereo pair
    }
    if (dumpNow)
    {
        ID3D11Texture2D* p0 = nullptr;
        ID3D11Texture2D* p1 = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_stereoPassMutex);
            const int bank = g_stereoPublishedBank;
            if (bank >= 0 && bank < kStereoPassBanks)
            {
                p0 = g_stereoPassTex[bank][0];
                p1 = g_stereoPassTex[bank][1];
                if (p0 != nullptr) p0->AddRef();
                if (p1 != nullptr) p1->AddRef();
            }
        }
        if (p0 != nullptr) { SfrDumpTextureBmp(p0, "pass0_present"); p0->Release(); }
        if (p1 != nullptr) { SfrDumpTextureBmp(p1, "pass1"); p1->Release(); }
        g_sfrDumpDone.store(true, std::memory_order_release);
        LogLine("[SFR-DUMP] one-shot pixel dump complete (boundary/backbuffer/pass0/pass1).");
    }
    // [CLEANVRCINE] cine-boundary visibility for the first presents of each convo/cutscene: whether
    // pass 0 landed and how many backbuffer batches were seen - a frozen cine names its own layer.
    if (g_sfrCineBoundary.load(std::memory_order_relaxed))
    {
        const unsigned d = g_sfrCineDiagLogs.fetch_add(1, std::memory_order_relaxed);
        if (d < 30 || (d % 600) == 0)
        {
            LogLine("[SFRCINE] present done: bbBatches=" + std::to_string(g_sfrCineBbBatches) +
                    " composites=" + std::to_string(g_sfrComposThisPresent.load(std::memory_order_relaxed)) +
                    " hadPass0=" + std::to_string(hadPass0 ? 1 : 0));
        }
    }
    g_sfrCineBbBatches = 0;   // render-thread only, reset per present
    g_sfrCineInBbBatch = nullptr;
    g_sfrPass0Captured.store(false, std::memory_order_release);
    g_sfrComposThisPresent.store(0, std::memory_order_release);
    g_sfrCapturePresentsLeft.fetch_sub(1, std::memory_order_acq_rel);
}

// [SFR-DUMP] one-shot pixel forensics for the 2026-07-14 "left eye renders red" report: at pair
// ~300, write the backbuffer + pass 0 (at boundary AND at present, bracketing the UI mirror) +
// pass 1 as BMPs next to the log. Minimal 24bpp writer (BMP is BGR bottom-up; source is RGBA8).
void SfrDumpTextureBmp(ID3D11Texture2D* tex, const char* name) noexcept
{
    if (tex == nullptr || g_gameDevice == nullptr || g_gameContext == nullptr) return;
    D3D11_TEXTURE2D_DESC d = {};
    tex->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
        d.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS)
    {
        LogLine(std::string("[SFR-DUMP] ") + name + " skipped: fmt=" + std::to_string(static_cast<int>(d.Format)));
        return;
    }
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* staging = nullptr;
    if (FAILED(g_gameDevice->CreateTexture2D(&sd, nullptr, &staging)) || staging == nullptr) return;
    g_gameContext->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE m = {};
    if (SUCCEEDED(g_gameContext->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
    {
        wchar_t exePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) != 0)
        {
            wchar_t* slash = wcsrchr(exePath, L'\\');
            if (slash != nullptr)
            {
                *(slash + 1) = L'\0';
                wchar_t path[MAX_PATH] = {};
                swprintf_s(path, L"%sMELEVR_SFR_%hs.bmp", exePath, name);
                FILE* f = nullptr;
                if (_wfopen_s(&f, path, L"wb") == 0 && f != nullptr)
                {
                    const UINT w = d.Width, h = d.Height;
                    const UINT rowBytes = ((w * 3 + 3) / 4) * 4;
                    const UINT imgBytes = rowBytes * h;
                    unsigned char hdr[54] = {};
                    hdr[0] = 'B'; hdr[1] = 'M';
                    const UINT fileBytes = 54 + imgBytes;
                    memcpy(hdr + 2, &fileBytes, 4);
                    const UINT dataOff = 54; memcpy(hdr + 10, &dataOff, 4);
                    const UINT dibSize = 40; memcpy(hdr + 14, &dibSize, 4);
                    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
                    hdr[26] = 1; hdr[28] = 24;
                    memcpy(hdr + 34, &imgBytes, 4);
                    fwrite(hdr, 1, 54, f);
                    std::vector<unsigned char> row(rowBytes, 0);
                    for (UINT y = 0; y < h; ++y)
                    {
                        const unsigned char* src = static_cast<const unsigned char*>(m.pData) +
                                                   static_cast<size_t>(h - 1 - y) * m.RowPitch;
                        for (UINT x = 0; x < w; ++x)
                        {
                            row[x * 3 + 0] = src[x * 4 + 2];   // B <- src B (RGBA8: R,G,B,A)
                            row[x * 3 + 1] = src[x * 4 + 1];   // G
                            row[x * 3 + 2] = src[x * 4 + 0];   // R
                        }
                        fwrite(row.data(), 1, rowBytes, f);
                    }
                    fclose(f);
                    LogLine(std::string("[SFR-DUMP] wrote MELEVR_SFR_") + name + ".bmp " +
                            std::to_string(w) + "x" + std::to_string(h));
                }
            }
        }
        g_gameContext->Unmap(staging, 0);
    }
    staging->Release();
}

// [SFR-UI] Scaleform/HUD draws once per PRESENT, after BOTH passes - so the boundary-captured pass 0
// (left eye) contains the world but no UI ("ui only renders on the right eye", 2026-07-14). Mirror
// each UI draw into the held pass-0 texture: same viewport/blend/shader state, only the RTV differs.
// Render thread; pass 0 was captured earlier in this same present, so the mirror composites onto it
// exactly like the game composites onto the backbuffer.
ID3D11RenderTargetView* SfrCurrentPass0Rtv() noexcept
{
    std::lock_guard<std::mutex> lock(g_stereoPassMutex);
    const int bank = g_stereoWriteBank;
    if (bank < 0 || bank >= kStereoPassBanks) return nullptr;
    ID3D11Texture2D* tex = g_stereoPassTex[bank][0];
    if (tex == nullptr || g_gameDevice == nullptr) return nullptr;
    if (g_sfrPass0Rtv[bank] == nullptr)
    {
        if (FAILED(g_gameDevice->CreateRenderTargetView(tex, nullptr, &g_sfrPass0Rtv[bank])))
        {
            return nullptr;
        }
    }
    return g_sfrPass0Rtv[bank];
}

bool SfrUiMirrorArmed() noexcept
{
    return SfrCaptureArmed() && g_sfrPass0Captured.load(std::memory_order_acquire);
}

// [UIMIR] diagnostic (2026-07-18, the "flickering transparent black square, LEFT eye only" on the
// endgame Citadel): every draw mirrored into the held pass-0 (left-eye) image logs its pixel-shader
// hash + viewport + blend ONCE per session. IsUiDrawNow is just "alpha-blended + full-width
// viewport" - a level-specific fullscreen FX quad qualifies and gets mirrored with non-Scaleform
// state (the 2026-07-14 "red overlay on the left eye" was exactly this family). The hash that
// appears only on the affected level = the culprit; the fix is then a targeted exclusion.
UINT LookupPsHash(void* ps) noexcept;   // defined below with the [UBER] shader-hash table

// [UIMIR] fix (2026-07-18, evidence-first): the endgame-Citadel "flickering transparent black
// square, LEFT eye only" = MULTIPLY-blend fullscreen FX quads (ps DE32BFC0 + 11B5823A, blend
// src=DEST_COLOR dst=ZERO - scene darkening pulses) slipping through IsUiDrawNow ("alpha-blended
// + full-width viewport") and getting mirrored into the held pass-0 (left-eye) image with the
// wrong state. Every REAL Scaleform UI draw observed blends over with dst=INV_SRC_ALPHA (straight
// or premultiplied alpha), so the mirror accepts ONLY that family - any darken/multiply/additive
// effect quad on ANY level is excluded structurally, not by per-level hash. Returns true = mirror;
// logs each distinct hash once either way ([UIMIR] mirrored / EXCLUDED).
bool UiMirrorTraceAndClassify(ID3D11DeviceContext* ctx) noexcept
{
    D3D11_BLEND_DESC bd = {};
    ID3D11BlendState* bs = nullptr;
    float bf[4] = {};
    UINT sm = 0;
    ctx->OMGetBlendState(&bs, bf, &sm);
    if (bs != nullptr) { bs->GetDesc(&bd); bs->Release(); }
    const bool mirrorable = bd.RenderTarget[0].BlendEnable != FALSE &&
                            bd.RenderTarget[0].DestBlend == D3D11_BLEND_INV_SRC_ALPHA;

    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    if (ps != nullptr) ps->Release();   // identity only (LookupPsHash keys on the pointer)
    const unsigned hash = LookupPsHash(ps);
    static unsigned s_seen[64] = {};    // render-thread only, like the rest of the mirror state
    static int s_seenN = 0;
    bool logged = false;
    for (int i = 0; i < s_seenN; ++i)
        if (s_seen[i] == hash) { logged = true; break; }
    if (!logged && s_seenN < 64)
    {
        s_seen[s_seenN++] = hash;
        D3D11_VIEWPORT vp = {};
        UINT nvp = 1;
        ctx->RSGetViewports(&nvp, &vp);
        char line[192];
        std::snprintf(line, sizeof(line),
                      "[UIMIR] %s ps=%08X vp=%.0fx%.0f+%.0f,%.0f blend=%d src=%d dst=%d op=%d",
                      mirrorable ? "mirrored" : "EXCLUDED (non-alpha blend)",
                      hash, vp.Width, vp.Height, vp.TopLeftX, vp.TopLeftY,
                      bd.RenderTarget[0].BlendEnable ? 1 : 0,
                      static_cast<int>(bd.RenderTarget[0].SrcBlend),
                      static_cast<int>(bd.RenderTarget[0].DestBlend),
                      static_cast<int>(bd.RenderTarget[0].BlendOp));
        LogLine(line);
    }
    return mirrorable;
}

template <typename DrawThunk>
void SfrMirrorUiDrawIntoPass0(ID3D11DeviceContext* ctx, DrawThunk&& draw) noexcept
{
    ID3D11RenderTargetView* rtv = SfrCurrentPass0Rtv();
    if (rtv == nullptr) return;
    if (!UiMirrorTraceAndClassify(ctx)) return;   // [UIMIR] FX quads never land in the left-eye image
    ID3D11RenderTargetView* savedRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* savedDsv = nullptr;
    ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtvs, &savedDsv);
    // Keep the game's OWN depth-stencil bound (sizes match the backbuffer): Scaleform clips HUD
    // elements with STENCIL masks, and mirroring with a null DSV drew every masked element in full
    // (the 2026-07-14 "red overlay on the left eye" = a masked vignette quad, unclipped; radar
    // unmasked and misplaced). Only the color target changes.
    ctx->OMSetRenderTargets(1, &rtv, savedDsv);
    draw();
    ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtvs, savedDsv);
    for (auto*& r : savedRtvs)
    {
        if (r != nullptr) { r->Release(); r = nullptr; }
    }
    if (savedDsv != nullptr) savedDsv->Release();
}

void STDMETHODCALLTYPE ClearDepthStencilViewHook(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv,
                                                 UINT flags, FLOAT depthVal, UINT8 stencil) noexcept
{
    DynResRtObserve(ctx, dsv);

    // [ME2PORT] pass boundary = MASS EFFECT 2's SFR capture design, ported 2026-07-20: snapshot the
    // backbuffer at EVERY scene-sized depth clear while the replay is armed. Each pass clears depth
    // at its start, so at a clear the backbuffer holds the PREVIOUS pass's finished image; pass 1
    // (the replay) clears last, so the LAST snapshot before present = pass 0's final left-eye image.
    // CaptureStereoPass(0) overwrites in place and the bank only advances on pass 1, so re-snapshot
    // is free of pair corruption by construction.
    //
    // WHY (the 2026-07-20 release bug): the OLD boundary ("first scene-sized clear after >=1
    // uber-composite draw") identified the composite by an exact FNV hash of its shader BYTECODE
    // (kUberCompositeHash). The game's own video-options screen rewrites GamerSettings.ini and drops
    // the hand-tuned keys (AntiAliasing/QualityBloom/AmbientOcclusion/...), which changes the
    // post-process permutation -> different bytecode -> hash never matches -> composites stay 0 ->
    // the boundary NEVER fires -> zero stereo pairs -> BOTH eyes get the same mono image (separation
    // slider does nothing, swap-eyes does nothing). ME2's clear-snapshot trigger has no shader
    // knowledge at all: no settings rewrite, driver change, or overlay can starve it.
    // [ME2PORT v2] two gates on top of the raw ME2 trigger, both shader-independent, both learned
    // from the 16:09 live run (pairs published continuously but the LEFT eye went mono seconds in):
    // (1) DEPTH-flag clears only - Scaleform's UI stencil-only clears happen AFTER pass 1's
    //     composite, so an unfiltered "every clear" snapshot re-captured the backbuffer holding the
    //     RIGHT eye's finished image = both eyes identical ("shepard small for a second, then flat").
    //     Scene passes always clear DEPTH; UI mask maintenance clears STENCIL only.
    // (2) backbuffer-dirty since the last snapshot - kills the reflection-clear copy flood (11 -> 2
    //     snapshots per present) with zero information loss (a clear with no backbuffer write in
    //     between cannot change what the snapshot would hold).
    if (dsv != nullptr && (flags & D3D11_CLEAR_DEPTH) != 0 && SfrCaptureArmed() &&
        g_sfrBbDirty.load(std::memory_order_relaxed))
    {
        ID3D11Resource* sfrRes = nullptr;
        dsv->GetResource(&sfrRes);
        if (sfrRes != nullptr)
        {
            ID3D11Texture2D* sfrTex = nullptr;
            if (SUCCEEDED(sfrRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&sfrTex))) &&
                sfrTex != nullptr)
            {
                D3D11_TEXTURE2D_DESC sfrDesc = {};
                sfrTex->GetDesc(&sfrDesc);
                if (sfrDesc.SampleDesc.Count == 1 &&
                    sfrDesc.Width == g_sceneRenderW.load(std::memory_order_acquire) &&
                    sfrDesc.Height == g_sceneRenderH.load(std::memory_order_acquire))
                {
                    static std::atomic<int> s_me2PortLogged{0};
                    if (s_me2PortLogged.exchange(1, std::memory_order_relaxed) == 0)
                        LogLine("[ME2PORT] SFR pass-0 capture = ME2 clear-snapshot trigger v2 (depth-flag + bb-dirty gates) ACTIVE");
                    g_sfrBbDirty.store(false, std::memory_order_relaxed);
                    SfrOnSceneDepthClear();
                }
                sfrTex->Release();
            }
            sfrRes->Release();
        }
    }

    // [RELIEF M0.5b] In STEREO the pooled depth is already trampled to 1.0 by clear time, so every
    // clear-time capture is a flat corpse - AND it fires ~120/s, flooding the pipeline with wasted 4K
    // copies (the freeze). Stereo is served entirely by the mid-frame UI-latch vantage; skip clear-time
    // capture here. AER/DIBR/mono keep the clear-time path (works, no trample).
    const bool stereoUi = g_stereoUiActive.load(std::memory_order_acquire);
    if (ctx != nullptr && dsv != nullptr && !stereoUi &&
        g_depthMapEnabled.load(std::memory_order_relaxed))
    {
        ID3D11Resource* res = nullptr;
        dsv->GetResource(&res);
        if (res != nullptr)
        {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex != nullptr)
            {
                D3D11_TEXTURE2D_DESC d = {};
                tex->GetDesc(&d);
                // Scene depth == the LIVE render resolution (LE1 locks scene-depth to the display mode == the
                // backbuffer), non-MSAA, AND it received real geometry draws since its last clear. Matching the
                // live size (NOT a hardcoded 2096 - that silently broke DIBR at every other resolution) is the fix.
                const UINT renderW = g_sceneRenderW.load(std::memory_order_acquire);
                const UINT renderH = g_sceneRenderH.load(std::memory_order_acquire);
                const bool sceneSized = (renderW != 0) && (d.Width == renderW) && (d.Height == renderH);
                if (!sceneSized && d.SampleDesc.Count == 1 && d.Width >= 512)
                {
                    const int dn = g_depthMissLogs.fetch_add(1, std::memory_order_relaxed);
                    if (dn < 20)
                        LogLine("[DIBR_DEPTH] non-scene depth clear " + std::to_string(d.Width) + "x" +
                                std::to_string(d.Height) + " (render=" + std::to_string(renderW) + "x" +
                                std::to_string(renderH) + ")");
                }
                if (sceneSized && d.SampleDesc.Count == 1)
                {
                    // [RELIEF M0.1] counter keyed by RESOURCE (matches the draw hooks); res is the clear
                    // target's resource resolved above. [M0.4] no blacklist - capture every populated pass,
                    // the content-gated promotion below decides which frame is real (hold-last-good).
                    uint32_t* counter = DepthDrawCounter(res);
                    const uint32_t drawsSince = (counter != nullptr) ? *counter : 0;
                    if (counter != nullptr) *counter = 0;   // reset for the next clear cycle
                    const bool populated = drawsSince >= kSceneDepthDrawThreshold;
                    const int n = g_depthCapLogs.fetch_add(1, std::memory_order_relaxed);
                    if (n < 60 || (n % 600) == 0)
                    {
                        char clbuf[192];
                        std::snprintf(clbuf, sizeof(clbuf),
                                      "[DIBR_DEPTH] scene clear %ux%u fmt=%d drawsSince=%u populated=%d res=%p clearVal=%.2f",
                                      d.Width, d.Height, static_cast<int>(d.Format), drawsSince, populated ? 1 : 0,
                                      res, depthVal);
                        LogLine(clbuf);
                    }
                    if (populated) RunDepthCapturePipeline(ctx, tex, d);   // [M0.5] shared with the UI-latch vantage
                }
                tex->Release();
            }
            res->Release();
        }
    }
    if (g_originalClearDSV != nullptr) g_originalClearDSV(ctx, dsv, flags, depthVal, stencil);
}

// ============================ [XSCISSOR] ============================
// Right-eye black character / missing bloom-DoF fix. Root-cause chain:
// the defect follows any view whose rect starts at x!=0 in the shared target; the light draws DO execute
// on the right half (draw census), and the per-half pixel-shader constants are correct (XOFFSET_DIFF found
// only the legitimate 0.25->0.75 screen-bias delta). The remaining way an issued draw can shade zero pixels
// is raster state: the engine's per-light/per-pass scissor rect, computed CPU-side from the view rect.
// Signature of the broken math: a draw whose viewport is the RIGHT half of its render target but whose
// scissor rect lies entirely inside the LEFT half. Such a draw can never touch its own viewport, so
// shifting the rect into the right half is safe by construction - it can only restore pixels the engine
// meant to shade. Applies only while SBS stereo is the active mode; left half is never modified.
std::atomic_bool g_xScissorFixEnabled{true};
std::atomic<unsigned long long> g_xScissorRightDraws{0};   // right-half draws seen with scissor test enabled
std::atomic<unsigned long long> g_xScissorShifted{0};      // rects shifted into the right half
std::atomic<unsigned long long> g_xScissorExpanded{0};     // empty rects expanded to the viewport
std::atomic<unsigned> g_xScissorLogs{0};

struct XScissorRestore
{
    bool active = false;
    UINT count = 0;
    D3D11_RECT rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
};

void BeginXScissorFix(ID3D11DeviceContext* context, XScissorRestore* restore) noexcept
{
    if (context == nullptr || restore == nullptr) return;
    if (!g_xScissorFixEnabled.load(std::memory_order_relaxed) ||
        !g_stereoUiActive.load(std::memory_order_acquire))
    {
        return;
    }

    // Right-half-of-target viewport? Compared against the BOUND RTV's width (not just the backbuffer) so the
    // quarter-size bloom/DoF filter chain - same left|right region geometry at 1/4 scale - is covered too.
    const UINT rtW = g_currentRtvWidth;
    if (rtW < 512) return;
    const float halfW = 0.5f * static_cast<float>(rtW);
    UINT vpCount = 1;
    D3D11_VIEWPORT vp = {};
    context->RSGetViewports(&vpCount, &vp);
    if (vpCount == 0 || vp.Width < 16.0f || vp.Width > halfW + 2.0f) return;
    if (std::fabs(vp.TopLeftX - halfW) > 2.0f) return;

    ID3D11RasterizerState* rasterizer = nullptr;
    context->RSGetState(&rasterizer);
    if (rasterizer == nullptr) return;
    D3D11_RASTERIZER_DESC rd = {};
    rasterizer->GetDesc(&rd);
    rasterizer->Release();
    if (rd.ScissorEnable == FALSE) return;

    UINT srCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT rects[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    context->RSGetScissorRects(&srCount, rects);
    if (srCount == 0) return;
    const unsigned long long seen = g_xScissorRightDraws.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seen == 1 || (seen % 8192) == 0)
    {
        LogLine("[XSCISSOR] stats rightScissorDraws=" + std::to_string(seen) +
                " shifted=" + std::to_string(g_xScissorShifted.load(std::memory_order_relaxed)) +
                " expanded=" + std::to_string(g_xScissorExpanded.load(std::memory_order_relaxed)));
    }

    const LONG halfL = static_cast<LONG>(halfW);
    const D3D11_RECT r = rects[0];
    const bool emptyRect = (r.right <= r.left || r.bottom <= r.top);
    if (!emptyRect && r.right > halfL + 2) return;   // rect already reaches the right half - engine got it right

    // Broken rect. Left-bound rects are the view-local math shifted whole into the right half; empty rects
    // are a global rect clamped against the wrong bound (left > right after clamp) - the true extent is
    // unrecoverable, so expand to the viewport (attenuation bounds the light, so extra pixels only cost fill).
    const LONG vpL = static_cast<LONG>(vp.TopLeftX);
    const LONG vpT = static_cast<LONG>(vp.TopLeftY);
    const LONG vpR = static_cast<LONG>(vp.TopLeftX + vp.Width);
    const LONG vpB = static_cast<LONG>(vp.TopLeftY + vp.Height);
    D3D11_RECT s = {};
    if (emptyRect)
    {
        s.left = vpL; s.top = vpT; s.right = vpR; s.bottom = vpB;
        g_xScissorExpanded.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        s = r;
        s.left += halfL;
        s.right += halfL;
        s.left = (std::max)(s.left, vpL);
        s.right = (std::min)(s.right, vpR);
        s.top = (std::max)(s.top, vpT);
        s.bottom = (std::min)(s.bottom, vpB);
        if (s.right <= s.left || s.bottom <= s.top)
        {
            s.left = vpL; s.top = vpT; s.right = vpR; s.bottom = vpB;
        }
        g_xScissorShifted.fetch_add(1, std::memory_order_relaxed);
    }

    restore->count = srCount;
    std::memcpy(restore->rects, rects, sizeof(D3D11_RECT) * srCount);
    D3D11_RECT patched[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
    std::memcpy(patched, rects, sizeof(D3D11_RECT) * srCount);
    patched[0] = s;
    context->RSSetScissorRects(srCount, patched);
    restore->active = true;

    const unsigned logIndex = g_xScissorLogs.fetch_add(1, std::memory_order_relaxed);
    if (logIndex < 48 || (logIndex % 2000) == 0)
    {
        char line[288] = {};
        std::snprintf(line, sizeof(line),
                      "[XSCISSOR] fix n=%u kind=%s rtW=%u vp=%.0fx%.0f+%.0f,%.0f rect=[%ld,%ld,%ld,%ld]->[%ld,%ld,%ld,%ld]",
                      logIndex + 1, emptyRect ? "empty" : "leftbound", rtW,
                      vp.Width, vp.Height, vp.TopLeftX, vp.TopLeftY,
                      static_cast<long>(r.left), static_cast<long>(r.top),
                      static_cast<long>(r.right), static_cast<long>(r.bottom),
                      static_cast<long>(s.left), static_cast<long>(s.top),
                      static_cast<long>(s.right), static_cast<long>(s.bottom));
        LogLine(line);
    }
}

void EndXScissorFix(ID3D11DeviceContext* context, XScissorRestore* restore) noexcept
{
    if (context == nullptr || restore == nullptr || !restore->active) return;
    context->RSSetScissorRects(restore->count, restore->rects);
    restore->active = false;
}

// ============================ [QPOST] ============================
// RETIRED from normal rendering: the failed Tali trace contains explicit L/R filter-buffer pairs and
// a legitimate second L/R pair with the same RT/PS. The old duplicate heuristic moved that second
// left pass over an already-rendered right pass, corrupting the right-eye post chain. The code remains
// available only behind MELEVR_ENABLE_QPOST_EXPERIMENT.txt for forensic reproduction.
// Normal stereo must not mutate these draws.
//
std::atomic_bool g_qpostFixEnabled{false};
std::atomic<unsigned long long> g_qpostShifted{0};
std::atomic<unsigned> g_qpostLogs{0};
std::atomic<unsigned> g_qpostTraceLogs{0};

constexpr UINT kQPostSeenMax = 96;
struct QPostSeen
{
    void* rt = nullptr;
    void* ps = nullptr;
    float vpX = 0, vpY = 0, vpW = 0, vpH = 0;
    UINT count = 0;
};
QPostSeen g_qpostSeen[kQPostSeenMax] = {};      // render-thread only, reset each frame
UINT g_qpostSeenCount = 0;
unsigned long long g_qpostSeenFrame = ~0ull;

struct QPostRestore
{
    bool active = false;
    UINT count = 0;
    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
};

void BeginQPostFix(ID3D11DeviceContext* context, UINT primIndexOrVertexCount, QPostRestore* restore) noexcept
{
    if (context == nullptr || restore == nullptr) return;
    if (!g_qpostFixEnabled.load(std::memory_order_relaxed) ||
        !g_stereoUiActive.load(std::memory_order_acquire))
    {
        return;
    }
    if (primIndexOrVertexCount > 6) return;      // full-region post quads only (3/4/6 verts or indices)

    const UINT bbW = g_sceneRenderW.load(std::memory_order_acquire);
    const UINT rtW = g_currentRtvWidth;
    if (bbW < 2048 || rtW == 0) return;
    if (rtW > bbW / 2 || rtW < bbW / 32) return; // the downsample/filter chain, not scene or tiny lookup targets

    const unsigned long long frame = g_presentCount.load(std::memory_order_relaxed);
    const bool traceFrame = (frame % 1800) < 3;  // self-arming: 3 consecutive frames every ~30s

    UINT vpCount = 1;
    D3D11_VIEWPORT vp = {};
    context->RSGetViewports(&vpCount, &vp);
    if (vpCount == 0 || vp.Width < 4.0f) return;

    ID3D11PixelShader* shader = nullptr;
    context->PSGetShader(&shader, nullptr, nullptr);
    if (shader != nullptr) shader->Release();    // identity only

    if (g_qpostSeenFrame != frame)
    {
        g_qpostSeenFrame = frame;
        g_qpostSeenCount = 0;
    }

    // Left-positioned, region-sized viewports are the dup-fix candidates; everything small-RT is traced.
    const bool fixCandidate = (vp.TopLeftX <= 4.0f && vp.Width <= 0.5f * rtW + 8.0f);
    UINT dupCount = 0;
    if (fixCandidate)
    {
        QPostSeen* entry = nullptr;
        for (UINT i = 0; i < g_qpostSeenCount; ++i)
        {
            QPostSeen& c = g_qpostSeen[i];
            if (c.rt == g_currentRtvTexPtr && c.ps == shader &&
                std::fabs(c.vpX - vp.TopLeftX) <= 1.0f && std::fabs(c.vpY - vp.TopLeftY) <= 1.0f &&
                std::fabs(c.vpW - vp.Width) <= 1.0f && std::fabs(c.vpH - vp.Height) <= 1.0f)
            {
                entry = &c;
                break;
            }
        }
        if (entry == nullptr && g_qpostSeenCount < kQPostSeenMax)
        {
            entry = &g_qpostSeen[g_qpostSeenCount++];
            entry->rt = g_currentRtvTexPtr;
            entry->ps = shader;
            entry->vpX = vp.TopLeftX; entry->vpY = vp.TopLeftY;
            entry->vpW = vp.Width;    entry->vpH = vp.Height;
            entry->count = 0;
        }
        if (entry != nullptr) dupCount = ++entry->count;
    }

    const bool shiftThis = (dupCount == 2);      // 2nd identical signature = view 2's misplaced pass
    if (shiftThis)
    {
        UINT allCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        D3D11_VIEWPORT all[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
        context->RSGetViewports(&allCount, all);
        restore->count = allCount;
        std::memcpy(restore->vps, all, sizeof(D3D11_VIEWPORT) * allCount);
        all[0].TopLeftX += 0.5f * static_cast<float>(rtW);
        context->RSSetViewports(allCount, all);
        restore->active = true;
        g_qpostShifted.fetch_add(1, std::memory_order_relaxed);
    }

    bool logIt = false;
    if (traceFrame) logIt = g_qpostTraceLogs.fetch_add(1, std::memory_order_relaxed) < 20000;
    if (shiftThis || dupCount > 2)
    {
        const unsigned fixIndex = g_qpostLogs.fetch_add(1, std::memory_order_relaxed);
        if (fixIndex < 48 || (fixIndex % 2000) == 0) logIt = true;
    }
    if (logIt)
    {
        char line[288] = {};
        std::snprintf(line, sizeof(line),
                      "[QPOST] frame=%llu rt=%p %ux%u ps=%p vp=%.0fx%.0f+%.0f,%.0f verts=%u dup=%u%s",
                      frame, g_currentRtvTexPtr, rtW, g_currentRtvHeight, shader,
                      vp.Width, vp.Height, vp.TopLeftX, vp.TopLeftY,
                      primIndexOrVertexCount, dupCount,
                      shiftThis ? " SHIFTED" : (dupCount > 2 ? " EXTRA-DUP" : ""));
        LogLine(line);
    }
}

void EndQPostFix(ID3D11DeviceContext* context, QPostRestore* restore) noexcept
{
    if (context == nullptr || restore == nullptr || !restore->active) return;
    context->RSSetViewports(restore->count, restore->vps);
    restore->active = false;
}

// ============================ [UBER] ============================
// Bloom-chain shader-hash trace (2026-07-14). FBCOPY falsified itself: its structural gate
// (<=6-vert draw, right-half viewport, full-width RT) matched ordinary world quads (it scrambled
// BC asset textures), and its trace proved the right view's full-width draws NEVER bind the
// quarter-res filter buffer in ps slots 0-7 - the composite lives on other targets. Structural
// draw-guessing is done (QPOST + FBCOPY both missed). This hooks CreatePixelShader, FNV-1a-hashes
// every PS, and logs the exact post chain by identity: every post-family quad draw with psHash,
// RT, viewport and SRV inventory, plus a raw vertex dump of composite-suspect quads (the
// filter-buffer UVs live in the quad VERTEX DATA).
// Cache-derived hashes: gather 0x9E583157, blur 0x9B6F6AA5, uber composite
// 0x9AA7F54A (may not match runtime FNV values - the trace works either way and reports the real
// runtime hashes). Pure trace - mutates nothing.
//
constexpr UINT kUberGatherHash = 0x9E583157u;
constexpr UINT kUberBlurHash = 0x9B6F6AA5u;
constexpr UINT kUberCompositeHash = 0x9AA7F54Au;

constexpr UINT kPsHashSlots = 8192;              // power of two; open-addressed ptr->hash map
std::atomic<void*> g_psHashKeys[kPsHashSlots] = {};
std::atomic<UINT> g_psHashVals[kPsHashSlots] = {};

void RecordPsHash(void* ps, UINT hash) noexcept
{
    if (ps == nullptr) return;
    const UINT mask = kPsHashSlots - 1;
    UINT i = static_cast<UINT>((reinterpret_cast<uintptr_t>(ps) >> 4) & mask);
    for (UINT probe = 0; probe < kPsHashSlots; ++probe, i = (i + 1) & mask)
    {
        void* cur = g_psHashKeys[i].load(std::memory_order_acquire);
        if (cur == ps)
        {
            g_psHashVals[i].store(hash, std::memory_order_release);   // pointer reuse: refresh
            return;
        }
        if (cur == nullptr)
        {
            void* expected = nullptr;
            if (g_psHashKeys[i].compare_exchange_strong(expected, ps, std::memory_order_acq_rel) ||
                expected == ps)
            {
                g_psHashVals[i].store(hash, std::memory_order_release);
                return;
            }
        }
    }
}

UINT LookupPsHash(void* ps) noexcept
{
    if (ps == nullptr) return 0;
    const UINT mask = kPsHashSlots - 1;
    UINT i = static_cast<UINT>((reinterpret_cast<uintptr_t>(ps) >> 4) & mask);
    for (UINT probe = 0; probe < kPsHashSlots; ++probe, i = (i + 1) & mask)
    {
        void* cur = g_psHashKeys[i].load(std::memory_order_acquire);
        if (cur == ps) return g_psHashVals[i].load(std::memory_order_acquire);
        if (cur == nullptr) return 0;
    }
    return 0;
}

std::atomic<unsigned> g_uberTraceLogs{0};
std::atomic<unsigned> g_uberKnownLogs{0};
std::atomic<unsigned> g_uberVbDumps{0};
ID3D11Buffer* g_uberVbStaging = nullptr;         // render-thread only; created once, 1KB

// ============================ [UBERFILL] ============================
// THE FIX (2026-07-14, from the uber-hash-trace-1 run): the uber composite (0x9AA7F54A) draws TWO
// full-vp quads per frame - left eye at clip x[-1,0], right eye at clip x[0,1] - and BOTH carry the
// identical left-half UV rect (~0.0006..0.5). The right view's gather/blur chain likewise mis-aims
// into the left region (matches the 2026-07-13 SBS BMP analysis), so the 1538x866 filter buffer's
// right half is never written and the right eye's bloom/DoF lookup reads black. Fill the read:
// right before each composite draw, copy the filter texture's written LEFT half over its unwritten
// RIGHT half (bloom is low-frequency; one eye's blur in the other is invisible). Aimed by SHADER
// IDENTITY, not draw shape: only the composite-hash draw, only its non-block-compressed
// quarter-res SRV (the 1538x866/11 filter buffer; BC asset formats >= 70 are excluded - the FBCOPY
// mistake is structurally impossible here). One copy per texture per frame, pure resource copy.
// Kill switch: MELEVR_DISABLE_UBERFILL.txt.
//
std::atomic_bool g_uberFillEnabled{true};
std::atomic<unsigned long long> g_uberFillTotal{0};
std::atomic<unsigned> g_uberFillLogs{0};
ID3D11Texture2D* g_uberFillScratch = nullptr;    // render-thread only; recreated on size/format change
UINT g_uberFillScratchW = 0, g_uberFillScratchH = 0;
DXGI_FORMAT g_uberFillScratchFmt = DXGI_FORMAT_UNKNOWN;
void* g_uberFillSeen[4] = {};                    // per-frame dedupe; identity only
UINT g_uberFillSeenCount = 0;
unsigned long long g_uberFillSeenFrame = ~0ull;

void FillFilterBufferRightHalf(ID3D11DeviceContext* context, unsigned long long frame) noexcept
{
    if (!g_uberFillEnabled.load(std::memory_order_relaxed)) return;
    const UINT bbW = g_sceneRenderW.load(std::memory_order_acquire);
    if (bbW < 2048) return;

    if (g_uberFillSeenFrame != frame)
    {
        g_uberFillSeenFrame = frame;
        g_uberFillSeenCount = 0;
    }

    ID3D11ShaderResourceView* srvs[8] = {};
    context->PSGetShaderResources(0, 8, srvs);
    for (UINT slot = 0; slot < 8; ++slot)
    {
        ID3D11ShaderResourceView* srv = srvs[slot];
        if (srv == nullptr) continue;
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        ID3D11Texture2D* tex = nullptr;
        if (res != nullptr)
        {
            res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex));
        }
        if (tex != nullptr)
        {
            D3D11_TEXTURE2D_DESC d = {};
            tex->GetDesc(&d);
            // The filter buffer and nothing else: quarter-res-ish width, renderable (non-BC) format.
            const bool isFilter = d.Width >= bbW / 8 && d.Width <= bbW / 3 &&
                                  d.Format < DXGI_FORMAT_BC1_TYPELESS &&
                                  d.SampleDesc.Count == 1;
            bool seen = false;
            if (isFilter)
            {
                for (UINT i = 0; i < g_uberFillSeenCount; ++i)
                {
                    if (g_uberFillSeen[i] == tex) { seen = true; break; }
                }
            }
            if (isFilter && !seen)
            {
                if (g_uberFillScratch == nullptr || g_uberFillScratchW != d.Width ||
                    g_uberFillScratchH != d.Height || g_uberFillScratchFmt != d.Format)
                {
                    if (g_uberFillScratch != nullptr)
                    {
                        g_uberFillScratch->Release();
                        g_uberFillScratch = nullptr;
                    }
                    if (g_gameDevice != nullptr)
                    {
                        D3D11_TEXTURE2D_DESC sd = {};
                        sd.Width = d.Width;
                        sd.Height = d.Height;
                        sd.MipLevels = 1;
                        sd.ArraySize = 1;
                        sd.Format = d.Format;
                        sd.SampleDesc.Count = 1;
                        sd.Usage = D3D11_USAGE_DEFAULT;
                        if (SUCCEEDED(g_gameDevice->CreateTexture2D(&sd, nullptr, &g_uberFillScratch)))
                        {
                            g_uberFillScratchW = d.Width;
                            g_uberFillScratchH = d.Height;
                            g_uberFillScratchFmt = d.Format;
                        }
                        else
                        {
                            g_uberFillScratch = nullptr;
                        }
                    }
                }
                if (g_uberFillScratch != nullptr)
                {
                    const UINT half = d.Width / 2;
                    D3D11_BOX box = { 0, 0, 0, half, d.Height, 1 };
                    context->CopySubresourceRegion(g_uberFillScratch, 0, 0, 0, 0, tex, 0, &box);
                    context->CopySubresourceRegion(tex, 0, half, 0, 0, g_uberFillScratch, 0, &box);
                    if (g_uberFillSeenCount < 4) g_uberFillSeen[g_uberFillSeenCount++] = tex;
                    g_uberFillTotal.fetch_add(1, std::memory_order_relaxed);
                    const unsigned n = g_uberFillLogs.fetch_add(1, std::memory_order_relaxed);
                    if (n < 48 || (n % 2000) == 0)
                    {
                        char line[224] = {};
                        std::snprintf(line, sizeof(line),
                                      "[UBERFILL] frame=%llu slot=%u tex=%p %ux%u fmt=%d left->right FILLED",
                                      frame, slot, static_cast<void*>(tex),
                                      d.Width, d.Height, static_cast<int>(d.Format));
                        LogLine(line);
                    }
                }
            }
            tex->Release();
        }
        if (res != nullptr) res->Release();
        srv->Release();
    }
}

void DumpUberQuadVb(ID3D11DeviceContext* context, unsigned long long frame, UINT psHash) noexcept
{
    if (context == nullptr || context != g_gameContext) return;   // Map(READ) needs the immediate context
    if (g_uberVbDumps.fetch_add(1, std::memory_order_relaxed) >= 60) return;
    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    context->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    if (vb == nullptr) return;
    if (g_uberVbStaging == nullptr && g_gameDevice != nullptr)
    {
        D3D11_BUFFER_DESC sd = {};
        sd.ByteWidth = 1024;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        g_gameDevice->CreateBuffer(&sd, nullptr, &g_uberVbStaging);
    }
    D3D11_BUFFER_DESC vd = {};
    vb->GetDesc(&vd);
    if (g_uberVbStaging != nullptr && offset < vd.ByteWidth)
    {
        const UINT len = (std::min)(1024u, vd.ByteWidth - offset);
        D3D11_BOX box = { offset, 0, 0, offset + len, 1, 1 };
        context->CopySubresourceRegion(g_uberVbStaging, 0, 0, 0, 0, vb, 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (SUCCEEDED(context->Map(g_uberVbStaging, 0, D3D11_MAP_READ, 0, &mapped)))
        {
            const UINT vstride = stride != 0 ? stride : 16;
            const UINT vcount = (std::min)(4u, vstride != 0 ? len / vstride : 0u);
            for (UINT v = 0; v < vcount; ++v)
            {
                const float* f = reinterpret_cast<const float*>(
                    static_cast<const unsigned char*>(mapped.pData) + v * vstride);
                const UINT nf = (std::min)(vstride / 4u, 8u);
                char line[288] = {};
                int off = std::snprintf(line, sizeof(line), "[UBERVB] frame=%llu hash=0x%08X stride=%u v%u:",
                                        frame, psHash, vstride, v);
                for (UINT k = 0; k < nf && off > 0 && off < static_cast<int>(sizeof(line)) - 16; ++k)
                {
                    off += std::snprintf(line + off, sizeof(line) - off, " %.6g", f[k]);
                }
                LogLine(line);
            }
            context->Unmap(g_uberVbStaging, 0);
        }
    }
    vb->Release();
}

void UberPostTrace(ID3D11DeviceContext* context, UINT primIndexOrVertexCount) noexcept
{
    if (context == nullptr) return;
    if (!g_stereoUiActive.load(std::memory_order_acquire)) return;
    if (primIndexOrVertexCount > 6) return;      // full-region post quads only
    const UINT bbW = g_sceneRenderW.load(std::memory_order_acquire);
    const UINT rtW = g_currentRtvWidth;
    if (bbW < 2048 || rtW == 0) return;

    ID3D11PixelShader* shader = nullptr;
    context->PSGetShader(&shader, nullptr, nullptr);
    if (shader != nullptr) shader->Release();    // identity only
    const UINT psHash = LookupPsHash(shader);
    const bool known = psHash == kUberGatherHash || psHash == kUberBlurHash || psHash == kUberCompositeHash;

    const unsigned long long frame = g_presentCount.load(std::memory_order_relaxed);
    const bool traceFrame = (frame % 1800) < 3;  // self-arming: 3 consecutive frames every ~30s

    // [UBERFILL] the fix rides the composite draw: fill the filter buffer's right half before the
    // quad that samples it.
    if (psHash == kUberCompositeHash) FillFilterBufferRightHalf(context, frame);

    if (!known && !traceFrame) return;

    // SRV inventory (identity + size only; everything released).
    ID3D11ShaderResourceView* srvs[8] = {};
    context->PSGetShaderResources(0, 8, srvs);
    char inv[176] = {};
    int invOff = 0;
    UINT slot0W = 0;
    for (UINT slot = 0; slot < 8; ++slot)
    {
        ID3D11ShaderResourceView* srv = srvs[slot];
        if (srv == nullptr) continue;
        ID3D11Resource* res = nullptr;
        srv->GetResource(&res);
        if (res != nullptr)
        {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) &&
                tex != nullptr)
            {
                D3D11_TEXTURE2D_DESC d = {};
                tex->GetDesc(&d);
                if (slot == 0) slot0W = d.Width;
                if (invOff >= 0 && invOff < static_cast<int>(sizeof(inv)) - 24)
                {
                    invOff += std::snprintf(inv + invOff, sizeof(inv) - invOff, " s%u=%ux%u/%d",
                                            slot, d.Width, d.Height, static_cast<int>(d.Format));
                }
                tex->Release();
            }
            res->Release();
        }
        srv->Release();
    }

    // Post-chain families only: small/view-sized targets, or full-width draws sampling a big SRV
    // (composite/blit family). Asset-textured world quads (FBCOPY's mistake) fail both gates.
    const bool interesting = known || rtW <= bbW / 2 + 8 || slot0W >= bbW / 2;
    if (!interesting) return;

    UINT vpCount = 1;
    D3D11_VIEWPORT vp = {};
    context->RSGetViewports(&vpCount, &vp);

    bool logIt;
    if (known)
    {
        const unsigned n = g_uberKnownLogs.fetch_add(1, std::memory_order_relaxed);
        logIt = n < 400 || (n % 5000) == 0;
    }
    else
    {
        logIt = g_uberTraceLogs.fetch_add(1, std::memory_order_relaxed) < 20000;
    }
    if (logIt)
    {
        char line[400] = {};
        std::snprintf(line, sizeof(line),
                      "[UBER] frame=%llu ps=%p hash=0x%08X%s rt=%p %ux%u/%d vp=%.0fx%.0f+%.0f,%.0f verts=%u%s",
                      frame, shader, psHash, known ? " KNOWN" : "",
                      g_currentRtvTexPtr, rtW, g_currentRtvHeight, static_cast<int>(g_currentRtvFormat),
                      vp.Width, vp.Height, vp.TopLeftX, vp.TopLeftY,
                      primIndexOrVertexCount, inv);
        LogLine(line);
    }
    // Vertex dump for composite suspects: the known composite hash, or (trace frames) any quad that
    // samples a big SRV - that family carries the per-view UV rect needed to compare L vs R.
    if ((known && psHash == kUberCompositeHash) || (traceFrame && slot0W >= bbW / 2))
    {
        DumpUberQuadVb(context, frame, psHash);
    }
}

// [SFR-CAP] composite-draw counter feed (needs the [UBER] hash table above, hence it lives here;
// the rest of the [SFR-CAP] machinery sits above ClearDepthStencilViewHook).
void SfrNoteQuadDraw(ID3D11DeviceContext* context, UINT primIndexOrVertexCount) noexcept
{
    if (primIndexOrVertexCount > 6 || !SfrCaptureArmed()) return;
    ID3D11PixelShader* shader = nullptr;
    context->PSGetShader(&shader, nullptr, nullptr);
    if (shader != nullptr) shader->Release();    // identity only
    if (LookupPsHash(shader) == kUberCompositeHash)
    {
        // [CINECAP2 2026-08-12] Cine pass-0 capture, anchored to the event itself instead of a batch
        // index. The batch-2 trigger below (SfrCineNoteDraw) encoded "batch 1 = pass 0's composite,
        // batch 2 = pass 1's", tuned when cine had ~10 backbuffer batches; the 08-09 cine rip changed
        // the draw structure (5-7 batches now), so batch 2 can arrive before pass 0 is complete. The
        // symptom that found it: residual cutscene jitter with [CINEJIT] showing every pair fresh and
        // zero holds - the eyes disagreed in CONTENT, not in timing. This composite draw is exact by
        // shader hash: when one composite has already landed this present, the draw about to execute
        // is pass 1's composite, so the backbuffer right now holds pass 0 COMPLETE, post and all.
        // Re-capturing here overwrites any too-early batch-2 snapshot (last-wins into the unpublished
        // write bank, the same overwrite gameplay does ~3.5x per present). The batch-2 trigger stays
        // as the fallback for presents whose composites are invisible to this hook (deferred-recorded);
        // the g_gameContext check makes this a no-op there, so it can never make things worse.
        if (context == g_gameContext &&
            g_sfrCineBoundary.load(std::memory_order_relaxed) &&
            g_sfrComposThisPresent.load(std::memory_order_acquire) == 1)
        {
            SfrOnSceneDepthClear();               // overwrite pass 0 with the completed image
            static std::atomic<unsigned> s_cap2Logs{0};
            const unsigned n = s_cap2Logs.fetch_add(1, std::memory_order_relaxed);
            if (n < 8 || (n % 600) == 0)
                LogLine("[CINECAP2] pass0 re-captured at the 2nd composite (structure-independent)");
        }
        g_sfrComposThisPresent.fetch_add(1, std::memory_order_acq_rel);
    }
}

// [CLEANVRCINE] cine pass-0 capture trigger. Clear-based capture does NOT work in cine: convo
// scenes render on a DEFERRED context, so their depth clears are game-thread RECORDING events
// (wrong size, wrong thread, wrong time). What MUST happen on the immediate context is each pass
// compositing onto the real backbuffer. So: track BATCHES of consecutive immediate-context draws
// whose RTV is the backbuffer (scene-sized LDR), excluding UI-signature draws (alpha-blended;
// composites are opaque). The FIRST draw of the SECOND batch = pass 1's composite beginning = the
// backbuffer still holds pass 0 complete -> capture BEFORE that draw executes. One-shot per present
// (g_sfrPass0Captured); later batches (letterbox bars, UI) are ignored. Render-thread state only.
void SfrCineNoteDraw(ID3D11DeviceContext* context) noexcept
{
    if (!g_sfrCineBoundary.load(std::memory_order_relaxed) || !SfrCaptureArmed()) return;
    if (context != g_gameContext) return;         // immediate context only (deferred = recording)

    // Resolve the CURRENT bound RTV directly - the shared g_currentRtv* globals can be stomped by
    // the deferred thread's recording while a convo is active. Immediate direct draws during a
    // convo are few (composites/UI/bars), so the query cost is negligible.
    ID3D11RenderTargetView* rtv = nullptr;
    context->OMGetRenderTargets(1, &rtv, nullptr);
    bool isBb = false;
    if (rtv != nullptr)
    {
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        if (res != nullptr)
        {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) &&
                tex != nullptr)
            {
                D3D11_TEXTURE2D_DESC d = {};
                tex->GetDesc(&d);
                isBb = d.Width == g_sceneRenderW.load(std::memory_order_acquire) &&
                       d.Height == g_sceneRenderH.load(std::memory_order_acquire) &&
                       (d.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                        d.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                        d.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS);
                tex->Release();
            }
            res->Release();
        }
        rtv->Release();
    }
    if (!isBb)
    {
        g_sfrCineInBbBatch = nullptr;
        return;
    }
    D3D11_VIEWPORT vp = {};
    if (IsUiDrawNow(context, vp)) return;         // UI draws are batch-neutral
    if (g_sfrCineInBbBatch == nullptr)
    {
        g_sfrCineInBbBatch = context;
        const unsigned batch = ++g_sfrCineBbBatches;
        if (batch == 2 && !g_sfrPass0Captured.load(std::memory_order_acquire))
        {
            SfrOnSceneDepthClear();               // capture pass 0 NOW (backbuffer = pass 0 final)
        }
    }
}

// [ME2PORT v2] mark the backbuffer dirty for the pass-0 snapshot gate. RTV identity via the cached
// globals (the same first gate IsUiDrawNow uses): composites + UI land on the scene-sized RGBA8
// target; offscreen scene / reflection-capture draws do not. One relaxed store, render thread.
inline void SfrNoteBackbufferDraw() noexcept
{
    if (!SfrCaptureArmed()) return;
    const UINT w = g_currentRtvWidth;
    if (w != 0 && g_currentRtvFormat == DXGI_FORMAT_R8G8B8A8_UNORM &&
        w == g_sceneRenderW.load(std::memory_order_relaxed))
        g_sfrBbDirty.store(true, std::memory_order_relaxed);
}

void STDMETHODCALLTYPE DrawIndexedHook(ID3D11DeviceContext* context, UINT indexCount, UINT startIndexLocation,
                                       INT baseVertexLocation) noexcept
{
    if (g_depthMapEnabled.load(std::memory_order_relaxed)) NoteDepthDraw(context);   // DIBR/relief: attribute + mid-scene live capture ([RELIEF M0.6])
    SfrNoteBackbufferDraw();                    // [ME2PORT v2] pass-0 snapshot dirty gate
    if (SfrCaptureArmed()) ReflCenNoteDraw();   // [REFLCEN] per-RT draw census while Stereo 2 runs
    SfrCineNoteDraw(context);                   // [CLEANVRCINE] cine pass-0 capture (backbuffer-batch trigger)
    DrawIndexedFn original = g_originalDrawIndexed;
    if (original != nullptr)
    {
        // Per-eye UI duplication (stereo only): panels/backgrounds render via DrawIndexed. If this is a
        // UI draw and stereo is the active mode, dup it into each eye-half and DON'T draw it a third time.
        if (g_stereoUiActive.load(std::memory_order_acquire) && g_uiDupEnabled.load(std::memory_order_acquire))
        {
            D3D11_VIEWPORT vp = {};
            if (IsUiDrawNow(context, vp))
            {
                g_uiDI.fetch_add(1, std::memory_order_relaxed);
                DupUiDraw(context, vp, [&] { original(context, indexCount, startIndexLocation, baseVertexLocation); });
                return;
            }
        }
        // [SFR-UI] Stereo 2: mirror each UI draw into the held left-eye pass (UI draws once per
        // present, after both passes, so the boundary-captured pass 0 has no HUD without this).
        if (SfrUiMirrorArmed())
        {
            D3D11_VIEWPORT vp = {};
            if (IsUiDrawNow(context, vp))
            {
                original(context, indexCount, startIndexLocation, baseVertexLocation);
                SfrMirrorUiDrawIntoPass0(context, [&] { original(context, indexCount, startIndexLocation, baseVertexLocation); });
                return;
            }
        }
        XScissorRestore xsc = {};
        QPostRestore qp = {};
        UberPostTrace(context, indexCount);
        SfrNoteQuadDraw(context, indexCount);
        BeginXScissorFix(context, &xsc);
        BeginQPostFix(context, indexCount, &qp);
        original(context, indexCount, startIndexLocation, baseVertexLocation);
        EndQPostFix(context, &qp);
        EndXScissorFix(context, &xsc);
    }
}

void STDMETHODCALLTYPE DrawHook(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertexLocation) noexcept
{
    SfrNoteBackbufferDraw();                    // [ME2PORT v2] pass-0 snapshot dirty gate
    if (SfrCaptureArmed()) ReflCenNoteDraw();   // [REFLCEN]
    SfrCineNoteDraw(context);                   // [CLEANVRCINE] cine pass-0 capture (backbuffer-batch trigger)
    DrawFn original = g_originalDraw;
    if (original == nullptr) return;
    // Per-eye UI duplication (stereo only): Scaleform TEXT glyphs render via non-indexed Draw. Without
    // this dup the text lands cross-eyed (scrambled) while panels look right.
    if (g_stereoUiActive.load(std::memory_order_acquire) && g_uiDupEnabled.load(std::memory_order_acquire))
    {
        D3D11_VIEWPORT vp = {};
        if (IsUiDrawNow(context, vp))
        {
            g_uiDnon.fetch_add(1, std::memory_order_relaxed);
            DupUiDraw(context, vp, [&] { original(context, vertexCount, startVertexLocation); });
            return;
        }
    }
    // [SFR-UI] Stereo 2: mirror non-indexed UI draws (Scaleform TEXT glyphs) into the left-eye pass.
    if (SfrUiMirrorArmed())
    {
        D3D11_VIEWPORT vp = {};
        if (IsUiDrawNow(context, vp))
        {
            original(context, vertexCount, startVertexLocation);
            SfrMirrorUiDrawIntoPass0(context, [&] { original(context, vertexCount, startVertexLocation); });
            return;
        }
    }
    XScissorRestore xsc = {};
    QPostRestore qp = {};
    UberPostTrace(context, vertexCount);
    SfrNoteQuadDraw(context, vertexCount);
    BeginXScissorFix(context, &xsc);
    BeginQPostFix(context, vertexCount, &qp);
    original(context, vertexCount, startVertexLocation);
    EndQPostFix(context, &qp);
    EndXScissorFix(context, &xsc);
}

void STDMETHODCALLTYPE DrawIndexedInstancedHook(ID3D11DeviceContext* context, UINT indexCountPerInstance,
                                                UINT instanceCount, UINT startIndexLocation,
                                                INT baseVertexLocation, UINT startInstanceLocation) noexcept
{
    if (g_depthMapEnabled.load(std::memory_order_relaxed)) NoteDepthDraw(context);   // DIBR/relief: attribute + mid-scene live capture ([RELIEF M0.6])
    SfrNoteBackbufferDraw();                    // [ME2PORT v2] pass-0 snapshot dirty gate
    if (SfrCaptureArmed()) ReflCenNoteDraw();   // [REFLCEN]
    SfrCineNoteDraw(context);                   // [CLEANVRCINE] cine pass-0 capture (backbuffer-batch trigger)
    DrawIndexedInstancedFn original = g_originalDrawIndexedInstanced;
    if (original == nullptr) return;
    // Per-eye UI duplication (stereo only): some Scaleform batches use instanced draws. Dup for safety.
    if (g_stereoUiActive.load(std::memory_order_acquire) && g_uiDupEnabled.load(std::memory_order_acquire))
    {
        D3D11_VIEWPORT vp = {};
        if (IsUiDrawNow(context, vp))
        {
            g_uiDII.fetch_add(1, std::memory_order_relaxed);
            DupUiDraw(context, vp, [&] {
                original(context, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
            });
            return;
        }
    }
    // [SFR-UI] Stereo 2: mirror instanced UI batches into the left-eye pass too.
    if (SfrUiMirrorArmed())
    {
        D3D11_VIEWPORT vp = {};
        if (IsUiDrawNow(context, vp))
        {
            original(context, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
            SfrMirrorUiDrawIntoPass0(context, [&] {
                original(context, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
            });
            return;
        }
    }
    XScissorRestore xsc = {};
    BeginXScissorFix(context, &xsc);
    original(context, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
    EndXScissorFix(context, &xsc);
}

void STDMETHODCALLTYPE DrawInstancedHook(ID3D11DeviceContext* context, UINT vertexCountPerInstance,
                                         UINT instanceCount, UINT startVertexLocation,
                                         UINT startInstanceLocation) noexcept
{
    DrawInstancedFn original = g_originalDrawInstanced;
    if (original != nullptr)
    {
        XScissorRestore xsc = {};
        BeginXScissorFix(context, &xsc);
        original(context, vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
        EndXScissorFix(context, &xsc);
    }
}

void STDMETHODCALLTYPE DrawAutoHook(ID3D11DeviceContext* context) noexcept
{
    DrawAutoFn original = g_originalDrawAuto;
    if (original != nullptr) original(context);
}

HRESULT STDMETHODCALLTYPE MapHook(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource,
                                  D3D11_MAP mapType, UINT mapFlags, D3D11_MAPPED_SUBRESOURCE* mapped) noexcept
{
    MapFn original = g_originalMap;
    if (original == nullptr) return E_FAIL;
    const HRESULT hr = original(context, resource, subresource, mapType, mapFlags, mapped);
    // [REFLFIX] track live 96-byte CB maps so Unmap can rewrite the mirror-camera record in place.
    if (SUCCEEDED(hr) && subresource == 0 && resource != nullptr &&
        mapped != nullptr && mapped->pData != nullptr &&
        (mapType == D3D11_MAP_WRITE_DISCARD || mapType == D3D11_MAP_WRITE ||
         mapType == D3D11_MAP_WRITE_NO_OVERWRITE) &&
        ReflWriteTrackingWanted())
    {
        int is96 = Cb96Lookup(resource);
        if (is96 == 0)
        {
            is96 = -1;
            ID3D11Buffer* buffer = nullptr;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) &&
                buffer != nullptr)
            {
                D3D11_BUFFER_DESC bd = {};
                buffer->GetDesc(&bd);
                if (bd.ByteWidth == 96 && (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0) is96 = 1;
                buffer->Release();
            }
            Cb96Store(resource, is96);
        }
        if (is96 == 1) Refl96Remember(resource, mapped->pData);
    }
    return hr;
}

void STDMETHODCALLTYPE UnmapHook(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource) noexcept
{
    // [REFLFIX] last chance before the staged bytes reach the GPU: rewrite the mirror camera.
    if (subresource == 0 && resource != nullptr)
    {
        void* staged = Refl96Take(resource);
        if (staged != nullptr)
        {
            ReflLogRecord(static_cast<float*>(staged), "Map");   // [REFLLOG] evidence only
            MaybeRewriteMirrorCameraRecord(static_cast<float*>(staged));
        }
    }
    UnmapFn original = g_originalUnmap;
    if (original != nullptr) original(context, resource, subresource);
}

void STDMETHODCALLTYPE VSSetConstantBuffersHook(ID3D11DeviceContext* context, UINT startSlot, UINT numBuffers,
                                                ID3D11Buffer* const* buffers) noexcept
{
    VSSetConstantBuffersFn original = g_originalVSSetConstantBuffers;
    if (original != nullptr) original(context, startSlot, numBuffers, buffers);
}

void STDMETHODCALLTYPE UpdateSubresourceHook(ID3D11DeviceContext* context, ID3D11Resource* dstResource,
                                             UINT dstSubresource, const D3D11_BOX* dstBox,
                                             const void* srcData, UINT srcRowPitch, UINT srcDepthPitch) noexcept
{
    // [REFLFIX] cover the UpdateSubresource write path for 96-byte camera CBs too.
    if (dstSubresource == 0 && dstResource != nullptr && srcData != nullptr && dstBox == nullptr &&
        ReflWriteTrackingWanted())
    {
        int is96 = Cb96Lookup(dstResource);
        if (is96 == 0)
        {
            is96 = -1;
            ID3D11Buffer* buffer = nullptr;
            if (SUCCEEDED(dstResource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&buffer))) &&
                buffer != nullptr)
            {
                D3D11_BUFFER_DESC bd = {};
                buffer->GetDesc(&bd);
                if (bd.ByteWidth == 96 && (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0) is96 = 1;
                buffer->Release();
            }
            Cb96Store(dstResource, is96);
        }
        if (is96 == 1)
        {
            float local[24] = {};
            std::memcpy(local, srcData, sizeof(local));
            ReflLogRecord(local, "US");   // [REFLLOG] evidence only
            if (MaybeRewriteMirrorCameraRecord(local))
            {
                UpdateSubresourceFn originalEdit = g_originalUpdateSubresource;
                if (originalEdit != nullptr)
                {
                    originalEdit(context, dstResource, dstSubresource, dstBox, local, srcRowPitch, srcDepthPitch);
                }
                return;
            }
        }
    }
    UpdateSubresourceFn original = g_originalUpdateSubresource;
    if (original != nullptr) original(context, dstResource, dstSubresource, dstBox, srcData, srcRowPitch, srcDepthPitch);
}

HRESULT STDMETHODCALLTYPE DeferredMapHook(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource,
                                          D3D11_MAP mapType, UINT mapFlags, D3D11_MAPPED_SUBRESOURCE* mapped) noexcept
{
    MapFn original = g_originalDeferredMap;
    if (original == nullptr) return E_FAIL;
    const HRESULT hr = original(context, resource, subresource, mapType, mapFlags, mapped);
    // [REFLFIX] the game records world rendering on the DEFERRED context (old channel finding) -
    // the mirror-camera CB writes live HERE, not on the immediate context. Same track-and-rewrite.
    if (SUCCEEDED(hr) && subresource == 0 && resource != nullptr &&
        mapped != nullptr && mapped->pData != nullptr &&
        (mapType == D3D11_MAP_WRITE_DISCARD || mapType == D3D11_MAP_WRITE ||
         mapType == D3D11_MAP_WRITE_NO_OVERWRITE) &&
        ReflWriteTrackingWanted())
    {
        int is96 = Cb96Lookup(resource);
        if (is96 == 0)
        {
            is96 = -1;
            ID3D11Buffer* cb96 = nullptr;
            if (SUCCEEDED(resource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&cb96))) &&
                cb96 != nullptr)
            {
                D3D11_BUFFER_DESC bd = {};
                cb96->GetDesc(&bd);
                if (bd.ByteWidth == 96 && (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0) is96 = 1;
                cb96->Release();
            }
            Cb96Store(resource, is96);
        }
        if (is96 == 1) Refl96Remember(resource, mapped->pData);
    }
    return hr;
}

void STDMETHODCALLTYPE DeferredUnmapHook(ID3D11DeviceContext* context, ID3D11Resource* resource, UINT subresource) noexcept
{
    // [REFLFIX] rewrite the mirror camera in the staged bytes before the deferred Unmap seals them.
    if (subresource == 0 && resource != nullptr)
    {
        void* staged = Refl96Take(resource);
        if (staged != nullptr)
        {
            ReflLogRecord(static_cast<float*>(staged), "DMap");   // [REFLLOG] evidence only
            MaybeRewriteMirrorCameraRecord(static_cast<float*>(staged));
        }
    }
    UnmapFn original = g_originalDeferredUnmap;
    if (original != nullptr) original(context, resource, subresource);
}

void STDMETHODCALLTYPE DeferredVSSetConstantBuffersHook(ID3D11DeviceContext* context, UINT startSlot, UINT numBuffers,
                                                        ID3D11Buffer* const* buffers) noexcept
{
    VSSetConstantBuffersFn original = g_originalDeferredVSSetConstantBuffers;
    if (original != nullptr) original(context, startSlot, numBuffers, buffers);
}

void STDMETHODCALLTYPE DeferredUpdateSubresourceHook(ID3D11DeviceContext* context, ID3D11Resource* dstResource,
                                                     UINT dstSubresource, const D3D11_BOX* dstBox,
                                                     const void* srcData, UINT srcRowPitch, UINT srcDepthPitch) noexcept
{
    // [REFLFIX] the deferred UpdateSubresource path - the last uncovered camera-CB write route.
    if (dstSubresource == 0 && dstResource != nullptr && srcData != nullptr && dstBox == nullptr &&
        ReflWriteTrackingWanted())
    {
        int is96 = Cb96Lookup(dstResource);
        if (is96 == 0)
        {
            is96 = -1;
            ID3D11Buffer* cb96 = nullptr;
            if (SUCCEEDED(dstResource->QueryInterface(__uuidof(ID3D11Buffer), reinterpret_cast<void**>(&cb96))) &&
                cb96 != nullptr)
            {
                D3D11_BUFFER_DESC bd = {};
                cb96->GetDesc(&bd);
                if (bd.ByteWidth == 96 && (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0) is96 = 1;
                cb96->Release();
            }
            Cb96Store(dstResource, is96);
        }
        if (is96 == 1)
        {
            float local[24] = {};
            std::memcpy(local, srcData, sizeof(local));
            ReflLogRecord(local, "DUS");   // [REFLLOG] evidence only
            if (MaybeRewriteMirrorCameraRecord(local))
            {
                UpdateSubresourceFn originalEdit = g_originalDeferredUpdateSubresource;
                if (originalEdit != nullptr)
                {
                    originalEdit(context, dstResource, dstSubresource, dstBox, local, srcRowPitch, srcDepthPitch);
                }
                return;
            }
        }
    }
    UpdateSubresourceFn original = g_originalDeferredUpdateSubresource;
    if (original != nullptr) original(context, dstResource, dstSubresource, dstBox, srcData, srcRowPitch, srcDepthPitch);
}

void STDMETHODCALLTYPE ExecuteCommandListHook(ID3D11DeviceContext* context, ID3D11CommandList* commandList,
                                              BOOL restoreContextState) noexcept
{
    ExecuteCommandListFn original = g_originalExecuteCommandList;
    if (original != nullptr) original(context, commandList, restoreContextState);
}

void STDMETHODCALLTYPE DeferredDrawIndexedHook(ID3D11DeviceContext* context, UINT indexCount,
                                               UINT startIndexLocation, INT baseVertexLocation) noexcept
{
    DrawIndexedFn original = g_originalDeferredDrawIndexed;
    if (original != nullptr) original(context, indexCount, startIndexLocation, baseVertexLocation);
}

void STDMETHODCALLTYPE DeferredDrawHook(ID3D11DeviceContext* context, UINT vertexCount, UINT startVertexLocation) noexcept
{
    DrawFn original = g_originalDeferredDraw;
    if (original != nullptr) original(context, vertexCount, startVertexLocation);
}

void STDMETHODCALLTYPE DeferredDrawIndexedInstancedHook(ID3D11DeviceContext* context, UINT indexCountPerInstance,
                                                       UINT instanceCount, UINT startIndexLocation,
                                                       INT baseVertexLocation, UINT startInstanceLocation) noexcept
{
    DrawIndexedInstancedFn original = g_originalDeferredDrawIndexedInstanced;
    if (original != nullptr)
        original(context, indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation, startInstanceLocation);
}

void STDMETHODCALLTYPE DeferredDrawInstancedHook(ID3D11DeviceContext* context, UINT vertexCountPerInstance,
                                                 UINT instanceCount, UINT startVertexLocation,
                                                 UINT startInstanceLocation) noexcept
{
    DrawInstancedFn original = g_originalDeferredDrawInstanced;
    if (original != nullptr)
        original(context, vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
}

void STDMETHODCALLTYPE DeferredDrawAutoHook(ID3D11DeviceContext* context) noexcept
{
    DrawAutoFn original = g_originalDeferredDrawAuto;
    if (original != nullptr) original(context);
}

void InstallDeferredContextDrawHooks(ID3D11DeviceContext* context) noexcept
{
    if (context == nullptr || g_deferredDrawIndexedSlot != nullptr)
    {
        return;
    }

    void** vtable = *reinterpret_cast<void***>(context);
    if (vtable == nullptr)
    {
        return;
    }

    if (g_gameContext != nullptr && vtable == *reinterpret_cast<void***>(g_gameContext))
    {
        LogLine("[D3DDEF] deferred context shares immediate vtable; no separate patch needed.");
        return;
    }

    if (PatchPointerSlot(&vtable[kContextVSSetConstantBuffersVTableIndex],
                         reinterpret_cast<void*>(&DeferredVSSetConstantBuffersHook),
                         reinterpret_cast<void**>(&g_originalDeferredVSSetConstantBuffers),
                         "ID3D11DeferredContext::VSSetConstantBuffers"))
        g_deferredVSSetConstantBuffersSlot = &vtable[kContextVSSetConstantBuffersVTableIndex];

    if (PatchPointerSlot(&vtable[kContextMapVTableIndex], reinterpret_cast<void*>(&DeferredMapHook),
                         reinterpret_cast<void**>(&g_originalDeferredMap), "ID3D11DeferredContext::Map"))
        g_deferredMapSlot = &vtable[kContextMapVTableIndex];

    if (PatchPointerSlot(&vtable[kContextUnmapVTableIndex], reinterpret_cast<void*>(&DeferredUnmapHook),
                         reinterpret_cast<void**>(&g_originalDeferredUnmap), "ID3D11DeferredContext::Unmap"))
        g_deferredUnmapSlot = &vtable[kContextUnmapVTableIndex];

    if (PatchPointerSlot(&vtable[kContextDrawIndexedVTableIndex], reinterpret_cast<void*>(&DeferredDrawIndexedHook),
                         reinterpret_cast<void**>(&g_originalDeferredDrawIndexed), "ID3D11DeferredContext::DrawIndexed"))
        g_deferredDrawIndexedSlot = &vtable[kContextDrawIndexedVTableIndex];

    if (PatchPointerSlot(&vtable[kContextDrawVTableIndex], reinterpret_cast<void*>(&DeferredDrawHook),
                         reinterpret_cast<void**>(&g_originalDeferredDraw), "ID3D11DeferredContext::Draw"))
        g_deferredDrawSlot = &vtable[kContextDrawVTableIndex];

    if (PatchPointerSlot(&vtable[kContextDrawIndexedInstancedVTableIndex], reinterpret_cast<void*>(&DeferredDrawIndexedInstancedHook),
                         reinterpret_cast<void**>(&g_originalDeferredDrawIndexedInstanced), "ID3D11DeferredContext::DrawIndexedInstanced"))
        g_deferredDrawIndexedInstancedSlot = &vtable[kContextDrawIndexedInstancedVTableIndex];

    if (PatchPointerSlot(&vtable[kContextDrawInstancedVTableIndex], reinterpret_cast<void*>(&DeferredDrawInstancedHook),
                         reinterpret_cast<void**>(&g_originalDeferredDrawInstanced), "ID3D11DeferredContext::DrawInstanced"))
        g_deferredDrawInstancedSlot = &vtable[kContextDrawInstancedVTableIndex];

    if (PatchPointerSlot(&vtable[kContextDrawAutoVTableIndex], reinterpret_cast<void*>(&DeferredDrawAutoHook),
                         reinterpret_cast<void**>(&g_originalDeferredDrawAuto), "ID3D11DeferredContext::DrawAuto"))
        g_deferredDrawAutoSlot = &vtable[kContextDrawAutoVTableIndex];

    if (PatchPointerSlot(&vtable[kContextUpdateSubresourceVTableIndex],
                         reinterpret_cast<void*>(&DeferredUpdateSubresourceHook),
                         reinterpret_cast<void**>(&g_originalDeferredUpdateSubresource),
                         "ID3D11DeferredContext::UpdateSubresource"))
        g_deferredUpdateSubresourceSlot = &vtable[kContextUpdateSubresourceVTableIndex];

    LogLine("[D3DDEF] deferred draw/CB vtable probe installed (separate vtable).");
}

void InstallContextDrawHooks(ID3D11DeviceContext* context) noexcept
{
    if (context == nullptr || g_drawIndexedSlot != nullptr)
    {
        return;
    }

    void** vtable = *reinterpret_cast<void***>(context);
    if (vtable == nullptr)
    {
        return;
    }

    if (PatchPointerSlot(&vtable[kContextVSSetConstantBuffersVTableIndex],
                         reinterpret_cast<void*>(&VSSetConstantBuffersHook),
                         reinterpret_cast<void**>(&g_originalVSSetConstantBuffers),
                         "ID3D11DeviceContext::VSSetConstantBuffers"))
    {
        g_vsSetConstantBuffersSlot = &vtable[kContextVSSetConstantBuffersVTableIndex];
        InstallInlineHook(reinterpret_cast<void*>(g_originalVSSetConstantBuffers),
                          reinterpret_cast<void*>(&VSSetConstantBuffersHook),
                          reinterpret_cast<void**>(&g_originalVSSetConstantBuffers),
                          "ID3D11DeviceContext::VSSetConstantBuffers");
    }

    if (PatchPointerSlot(&vtable[kContextMapVTableIndex], reinterpret_cast<void*>(&MapHook),
                         reinterpret_cast<void**>(&g_originalMap), "ID3D11DeviceContext::Map"))
    {
        g_mapSlot = &vtable[kContextMapVTableIndex];
        InstallInlineHook(reinterpret_cast<void*>(g_originalMap),
                          reinterpret_cast<void*>(&MapHook),
                          reinterpret_cast<void**>(&g_originalMap),
                          "ID3D11DeviceContext::Map");
    }

    if (PatchPointerSlot(&vtable[kContextUnmapVTableIndex], reinterpret_cast<void*>(&UnmapHook),
                         reinterpret_cast<void**>(&g_originalUnmap), "ID3D11DeviceContext::Unmap"))
    {
        g_unmapSlot = &vtable[kContextUnmapVTableIndex];
        InstallInlineHook(reinterpret_cast<void*>(g_originalUnmap),
                          reinterpret_cast<void*>(&UnmapHook),
                          reinterpret_cast<void**>(&g_originalUnmap),
                          "ID3D11DeviceContext::Unmap");
    }

    if (PatchPointerSlot(&vtable[kContextDrawIndexedVTableIndex], reinterpret_cast<void*>(&DrawIndexedHook),
                         reinterpret_cast<void**>(&g_originalDrawIndexed), "ID3D11DeviceContext::DrawIndexed"))
    {
        g_drawIndexedSlot = &vtable[kContextDrawIndexedVTableIndex];
        InstallInlineHook(reinterpret_cast<void*>(g_originalDrawIndexed),
                          reinterpret_cast<void*>(&DrawIndexedHook),
                          reinterpret_cast<void**>(&g_originalDrawIndexed),
                          "ID3D11DeviceContext::DrawIndexed");
    }

    if (PatchPointerSlot(&vtable[kClearDSVSlot], reinterpret_cast<void*>(&ClearDepthStencilViewHook),
                         reinterpret_cast<void**>(&g_originalClearDSV), "ID3D11DeviceContext::ClearDepthStencilView"))
    {
        g_clearDSVSlot = &vtable[kClearDSVSlot];
        InstallInlineHook(reinterpret_cast<void*>(g_originalClearDSV),
                          reinterpret_cast<void*>(&ClearDepthStencilViewHook),
                          reinterpret_cast<void**>(&g_originalClearDSV),
                          "ID3D11DeviceContext::ClearDepthStencilView");
    }

    if (PatchPointerSlot(&vtable[kOMSetRenderTargetsSlot], reinterpret_cast<void*>(&OMSetRenderTargetsHook),
                         reinterpret_cast<void**>(&g_originalOMSetRenderTargets), "ID3D11DeviceContext::OMSetRenderTargets"))
    {
        g_omSetRtSlot = &vtable[kOMSetRenderTargetsSlot];
        InstallInlineHook(reinterpret_cast<void*>(g_originalOMSetRenderTargets),
                          reinterpret_cast<void*>(&OMSetRenderTargetsHook),
                          reinterpret_cast<void**>(&g_originalOMSetRenderTargets),
                          "ID3D11DeviceContext::OMSetRenderTargets");
    }

    if (PatchPointerSlot(&vtable[kContextDrawVTableIndex], reinterpret_cast<void*>(&DrawHook),
                         reinterpret_cast<void**>(&g_originalDraw), "ID3D11DeviceContext::Draw"))
    {
        g_drawSlot = &vtable[kContextDrawVTableIndex];
        InstallInlineHook(reinterpret_cast<void*>(g_originalDraw),
                          reinterpret_cast<void*>(&DrawHook),
                          reinterpret_cast<void**>(&g_originalDraw),
                          "ID3D11DeviceContext::Draw");
    }

    if (PatchPointerSlot(&vtable[kContextDrawIndexedInstancedVTableIndex], reinterpret_cast<void*>(&DrawIndexedInstancedHook),
                         reinterpret_cast<void**>(&g_originalDrawIndexedInstanced), "ID3D11DeviceContext::DrawIndexedInstanced"))
    {
        g_drawIndexedInstancedSlot = &vtable[kContextDrawIndexedInstancedVTableIndex];
        InstallInlineHook(reinterpret_cast<void*>(g_originalDrawIndexedInstanced),
                          reinterpret_cast<void*>(&DrawIndexedInstancedHook),
                          reinterpret_cast<void**>(&g_originalDrawIndexedInstanced),
                          "ID3D11DeviceContext::DrawIndexedInstanced");
    }

    if (PatchPointerSlot(&vtable[kContextDrawInstancedVTableIndex], reinterpret_cast<void*>(&DrawInstancedHook),
                         reinterpret_cast<void**>(&g_originalDrawInstanced), "ID3D11DeviceContext::DrawInstanced"))
    {
        g_drawInstancedSlot = &vtable[kContextDrawInstancedVTableIndex];
        InstallInlineHook(reinterpret_cast<void*>(g_originalDrawInstanced),
                          reinterpret_cast<void*>(&DrawInstancedHook),
                          reinterpret_cast<void**>(&g_originalDrawInstanced),
                          "ID3D11DeviceContext::DrawInstanced");
    }

    if (PatchPointerSlot(&vtable[kContextDrawAutoVTableIndex], reinterpret_cast<void*>(&DrawAutoHook),
                         reinterpret_cast<void**>(&g_originalDrawAuto), "ID3D11DeviceContext::DrawAuto"))
    {
        g_drawAutoSlot = &vtable[kContextDrawAutoVTableIndex];
        InstallInlineHook(reinterpret_cast<void*>(g_originalDrawAuto),
                          reinterpret_cast<void*>(&DrawAutoHook),
                          reinterpret_cast<void**>(&g_originalDrawAuto),
                          "ID3D11DeviceContext::DrawAuto");
    }

    if (PatchPointerSlot(&vtable[kContextUpdateSubresourceVTableIndex],
                         reinterpret_cast<void*>(&UpdateSubresourceHook),
                         reinterpret_cast<void**>(&g_originalUpdateSubresource),
                         "ID3D11DeviceContext::UpdateSubresource"))
    {
        g_updateSubresourceSlot = &vtable[kContextUpdateSubresourceVTableIndex];
        InstallInlineHook(reinterpret_cast<void*>(g_originalUpdateSubresource),
                          reinterpret_cast<void*>(&UpdateSubresourceHook),
                          reinterpret_cast<void**>(&g_originalUpdateSubresource),
                          "ID3D11DeviceContext::UpdateSubresource");
    }

    if (PatchPointerSlot(&vtable[kContextExecuteCommandListVTableIndex], reinterpret_cast<void*>(&ExecuteCommandListHook),
                         reinterpret_cast<void**>(&g_originalExecuteCommandList), "ID3D11DeviceContext::ExecuteCommandList"))
    {
        g_executeCommandListSlot = &vtable[kContextExecuteCommandListVTableIndex];
        InstallInlineHook(reinterpret_cast<void*>(g_originalExecuteCommandList),
                          reinterpret_cast<void*>(&ExecuteCommandListHook),
                          reinterpret_cast<void**>(&g_originalExecuteCommandList),
                          "ID3D11DeviceContext::ExecuteCommandList");
    }

    LogLine("[D3DDRAW] draw hooks installed; probes marker-gated; depth counters sleep when Depth map is off.");
}

HRESULT STDMETHODCALLTYPE CreateBufferHook(ID3D11Device* device, const D3D11_BUFFER_DESC* desc,
                                           const D3D11_SUBRESOURCE_DATA* initialData,
                                           ID3D11Buffer** buffer) noexcept
{
    CreateBufferFn original = g_originalCreateBuffer;
    if (original == nullptr) return E_FAIL;
    const HRESULT hr = original(device, desc, initialData, buffer);
    return hr;
}

HRESULT STDMETHODCALLTYPE CreatePixelShaderHook(ID3D11Device* device, const void* shaderBytecode,
                                                SIZE_T bytecodeLength, ID3D11ClassLinkage* classLinkage,
                                                ID3D11PixelShader** pixelShader) noexcept
{
    CreatePixelShaderFn original = g_originalCreatePixelShader;
    if (original == nullptr) return E_FAIL;
    const HRESULT hr = original(device, shaderBytecode, bytecodeLength, classLinkage, pixelShader);
    if (SUCCEEDED(hr) && pixelShader != nullptr && *pixelShader != nullptr &&
        shaderBytecode != nullptr && bytecodeLength != 0)
    {
        const SIZE_T hashBytes = (std::min)(bytecodeLength, static_cast<SIZE_T>(0xffffffffu));
        RecordPsHash(*pixelShader, HashBytes(shaderBytecode, static_cast<UINT>(hashBytes)));
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateDeferredContextHook(ID3D11Device* device, UINT contextFlags,
                                                    ID3D11DeviceContext** deferredContext) noexcept
{
    CreateDeferredContextFn original = g_originalCreateDeferredContext;
    if (original == nullptr)
    {
        return DXGI_ERROR_INVALID_CALL;
    }
    const HRESULT hr = original(device, contextFlags, deferredContext);
    LogLine("[D3DDEF] CreateDeferredContext returned " + HexHRESULT(hr) +
            " ctx=" + ((SUCCEEDED(hr) && deferredContext != nullptr) ? HexPointer(*deferredContext) : std::string("null")));
    if (SUCCEEDED(hr) && deferredContext != nullptr && *deferredContext != nullptr)
    {
        // This may share the same vtable as the immediate context or use a separate one; log the fact either way.
        void** vtable = *reinterpret_cast<void***>(*deferredContext);
        LogLine("[D3DDEF] deferred context vtable=" + HexPointer(vtable));
        InstallDeferredContextDrawHooks(*deferredContext);
    }
    return hr;
}

void InstallDeviceHooks(ID3D11Device* device) noexcept
{
    if (device == nullptr || g_createDeferredContextSlot != nullptr)
    {
        return;
    }

    void** vtable = *reinterpret_cast<void***>(device);
    if (vtable == nullptr)
    {
        return;
    }

    if (PatchPointerSlot(&vtable[kDeviceCreateBufferVTableIndex],
                         reinterpret_cast<void*>(&CreateBufferHook),
                         reinterpret_cast<void**>(&g_originalCreateBuffer),
                         "ID3D11Device::CreateBuffer"))
    {
        g_createBufferSlot = &vtable[kDeviceCreateBufferVTableIndex];
        LogLine("[CBCREATE] constant-buffer create probe installed.");
    }

    if (PatchPointerSlot(&vtable[kDeviceCreatePixelShaderVTableIndex],
                         reinterpret_cast<void*>(&CreatePixelShaderHook),
                         reinterpret_cast<void**>(&g_originalCreatePixelShader),
                         "ID3D11Device::CreatePixelShader"))
    {
        g_createPixelShaderSlot = &vtable[kDeviceCreatePixelShaderVTableIndex];
        LogLine("[UBER] pixel-shader hash probe installed.");
    }

    if (PatchPointerSlot(&vtable[kDeviceCreateDeferredContextVTableIndex],
                         reinterpret_cast<void*>(&CreateDeferredContextHook),
                         reinterpret_cast<void**>(&g_originalCreateDeferredContext),
                         "ID3D11Device::CreateDeferredContext"))
    {
        g_createDeferredContextSlot = &vtable[kDeviceCreateDeferredContextVTableIndex];
        LogLine("[D3DDEF] deferred-context probe installed.");

        ID3D11DeviceContext* probeContext = nullptr;
        const HRESULT hr = device->CreateDeferredContext(0, &probeContext);
        LogLine("[D3DDEF] one-shot deferred vtable probe CreateDeferredContext hr=" + HexHRESULT(hr) +
                " ctx=" + (SUCCEEDED(hr) && probeContext != nullptr ? HexPointer(probeContext) : std::string("null")));
        SafeRelease(probeContext);
    }
}

// ============================================================================
// [GPU] Real GPU milliseconds per frame, via D3D11 timestamp queries.
//
// Present rate is pinned at 60 on this game, so "fps=60" cannot distinguish "loads of headroom" from "one
// dropped frame away from the cliff". Timestamps can. Present-to-Present is bracketed (whole frame, including
// the XR submit work) with a TIMESTAMP pair inside a DISJOINT query, and results are read back a few frames
// later with DONOTFLUSH so the pipeline never stalls to collect these diagnostics.
//
// gpuMs vs the 16.67ms budget at 60Hz is the number that decides whether 6144x3456 is a safe default.
// ============================================================================
constexpr int kGpuQueryRing = 5;

struct GpuFrameQuery
{
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* tsStart = nullptr;
    ID3D11Query* tsEnd = nullptr;
    bool inFlight = false;
};

GpuFrameQuery g_gpuQ[kGpuQueryRing] = {};
int g_gpuQHead = 0;
bool g_gpuQInit = false;

// Rolling stats so one spiky frame doesn't set policy.
double g_gpuMsSum = 0.0;
double g_gpuMsMax = 0.0;
int g_gpuMsCount = 0;
ULONGLONG g_gpuLastLog = 0;

bool EnsureGpuQueries() noexcept
{
    if (g_gpuQInit) return true;
    if (g_gameDevice == nullptr) return false;

    D3D11_QUERY_DESC dd = {};
    dd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    D3D11_QUERY_DESC td = {};
    td.Query = D3D11_QUERY_TIMESTAMP;

    for (int i = 0; i < kGpuQueryRing; ++i)
    {
        if (FAILED(g_gameDevice->CreateQuery(&dd, &g_gpuQ[i].disjoint)) ||
            FAILED(g_gameDevice->CreateQuery(&td, &g_gpuQ[i].tsStart)) ||
            FAILED(g_gameDevice->CreateQuery(&td, &g_gpuQ[i].tsEnd)))
        {
            LogLine("[GPU] timestamp query creation failed - GPU timing disabled.");
            return false;
        }
    }
    g_gpuQInit = true;
    LogLine("[GPU] timestamp queries ready.");
    return true;
}

// Collect any finished query without blocking. Returns true if it consumed one.
void CollectGpuQuery(ID3D11DeviceContext* ctx, GpuFrameQuery& q) noexcept
{
    if (!q.inFlight) return;

    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = {};
    if (ctx->GetData(q.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;

    UINT64 t0 = 0, t1 = 0;
    if (ctx->GetData(q.tsStart, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;
    if (ctx->GetData(q.tsEnd, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return;

    q.inFlight = false;
    if (dj.Disjoint || dj.Frequency == 0 || t1 <= t0) return;   // clocks changed mid-frame; sample is garbage

    const double ms = static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(dj.Frequency);
    if (ms <= 0.0 || ms > 1000.0) return;

    g_gpuMsSum += ms;
    if (ms > g_gpuMsMax) g_gpuMsMax = ms;
    ++g_gpuMsCount;
}

// Step the target down when the frame budget is missed, and persist it for the next launch.
//
// Why next launch and not now: the backbuffer is created once, at startup. Resizing it live would mean
// tearing down the swapchain, the eye swapchains and every render target mid-session - the same class of
// problem as the live Pacing-Hz slider that can hang the game.
//
// Only ever steps DOWN, and only from sustained overrun (5 consecutive 2s windows), so a single loading
// hitch or a heavy cutscene can't ratchet the resolution into the floor. maxMs is deliberately NOT
// used for this: it catches streaming hitches that no resolution change will fix.
constexpr double kFrameBudgetMs = 16.67;      // the game's own 60fps limiter is the ceiling worth targeting
constexpr double kOverrunTripMs = 17.60;      // ~6% over: clear of "sitting exactly on the cap" (16.67)
constexpr int kOverrunWindowsToTrip = 5;

int g_overrunWindows = 0;
bool g_calibratedThisRun = false;

void CalibrateAutoRes(double avgMs) noexcept
{
    if (g_autoRes.enabled == 0 || g_autoRes.eyeW == 0) return;
    if (g_calibratedThisRun) return;                 // one step per launch; re-measure from the new target
    if (g_autoRes.targetW == 0 || g_autoRes.targetH == 0) return;

    if (avgMs < kOverrunTripMs) { g_overrunWindows = 0; return; }
    if (++g_overrunWindows < kOverrunWindowsToTrip) return;

    // Frame cost scales with pixel count, so scale linear dimensions by sqrt(budget/measured). The 0.98 is a
    // deliberate undershoot: converging from below beats oscillating around the budget.
    double scale = std::sqrt(kFrameBudgetMs / avgMs) * 0.98;
    if (scale > 0.97) scale = 0.97;                  // guarantee forward progress
    if (scale < 0.60) scale = 0.60;                  // never collapse in one step

    unsigned nw = static_cast<unsigned>(static_cast<double>(g_autoRes.targetW) * scale) & ~1u;
    if (nw < 1280u) nw = 1280u;
    unsigned nh = static_cast<unsigned>((static_cast<unsigned long long>(nw) * 9ull) / 16ull) & ~1u;

    if (nw >= g_autoRes.targetW) return;             // nothing to gain

    char line[224];
    std::snprintf(line, sizeof(line),
                  "[AUTORES] avgMs=%.2f over budget %.2f for %d windows -> target %ux%u => %ux%u "
                  "(applies next launch)",
                  avgMs, kFrameBudgetMs, g_overrunWindows, g_autoRes.targetW, g_autoRes.targetH, nw, nh);
    LogLine(line);

    g_autoRes.targetW = nw;
    g_autoRes.targetH = nh;
    WriteAutoResFile(g_autoRes);
    g_calibratedThisRun = true;
}

void TickGpuTiming() noexcept
{
    ID3D11DeviceContext* ctx = g_gameContext;
    if (ctx == nullptr || !EnsureGpuQueries()) return;

    // Close the frame opened at the previous Present.
    GpuFrameQuery& cur = g_gpuQ[g_gpuQHead];
    if (cur.inFlight)
    {
        ctx->End(cur.tsEnd);
        ctx->End(cur.disjoint);
    }

    // Harvest whatever has landed, oldest first. Never blocks.
    for (int i = 0; i < kGpuQueryRing; ++i) CollectGpuQuery(ctx, g_gpuQ[i]);

    // Open the next frame.
    g_gpuQHead = (g_gpuQHead + 1) % kGpuQueryRing;
    GpuFrameQuery& nxt = g_gpuQ[g_gpuQHead];
    if (!nxt.inFlight)
    {
        ctx->Begin(nxt.disjoint);
        ctx->End(nxt.tsStart);
        nxt.inFlight = true;
    }

    const ULONGLONG now = GetTickCount64();
    if (g_gpuLastLog == 0) { g_gpuLastLog = now; return; }
    if (now - g_gpuLastLog < 2000 || g_gpuMsCount == 0) return;

    const double avg = g_gpuMsSum / static_cast<double>(g_gpuMsCount);
    char line[192];
    std::snprintf(line, sizeof(line),
                  "[GPU] avgMs=%.2f maxMs=%.2f samples=%d budget60=16.67ms headroom=%.0f%% backbuffer=%ux%u",
                  avg, g_gpuMsMax, g_gpuMsCount, (1.0 - avg / 16.67) * 100.0,
                  g_sceneRenderW.load(std::memory_order_acquire),
                  g_sceneRenderH.load(std::memory_order_acquire));
    LogLine(line);

    // [LOADCRASH] Continuous VRAM/device-health trend alongside the frame-timing report (same 2s cadence, same
    // log line neighborhood), so a load-adjacent driver crash (2026-07-12, nvwgf2umx.dll) can be read back
    // against the whole session's VRAM trajectory, not just the one-shot samples at the movie/cine edges.
    MELEVR::D3DCapture::LogVideoMemory("periodic", false);
    MELEVR::D3DCapture::LogDeviceHealth("periodic");

    CalibrateAutoRes(avg);

    g_gpuLastLog = now;
    g_gpuMsSum = 0.0;
    g_gpuMsMax = 0.0;
    g_gpuMsCount = 0;
}

// (No present-rate logger here: [PRESENTHZ] further down already reports fps + originalSync + flags. A second
// one was added 2026-07-10 by mistake and removed the same day.)

HRESULT STDMETHODCALLTYPE PresentHook(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) noexcept
{
    TickGpuTiming();
    const unsigned long long n = g_presentCount.fetch_add(1, std::memory_order_relaxed) + 1;
    g_uiDepthCaptured = false;   // [RELIEF M0.5] re-arm the once-per-present UI-latch depth capture
    // [RELIEF M0.6] stereo probe+promote ONCE here from the fullest mid-scene capture (before the reset),
    // so only one good frame publishes per present - no white flash from early sparse mid-scene copies.
    ProbeAndPromoteStereoAtPresent();
    // [RELIEF M0.5c] reset per-frame draw tallies so each frame picks a fresh MOST-DRAWN depth leader.
    // (Stereo suppresses the clear-time counter reset, so without this the counts would grow unbounded.)
    if (g_depthMapEnabled.load(std::memory_order_relaxed))
    {
        g_busyDepthDraws = 0;
        for (auto& e : g_depthDraws) e.draws = 0;
    }
    g_lastOriginalSyncInterval = syncInterval;
    g_lastOriginalPresentFlags = flags;
    // [TEARING] decide once - used for BOTH the [PRESENTHZ] log below and the actual Present at the bottom.
    g_forceTearThisPresent = (MELEVR::Config::Get().forceAllowTearing &&
                              g_swapchainHasTearingFlag.load(std::memory_order_acquire));

    // XSCISSOR keeps its kill switch. QPOST is a disproven experiment and is explicit opt-in only;
    // normal stereo never arms it.
    // REFLFIX (v7 camera freeze) is ALSO a disproven experiment: REFLRATE (pchud.cpp) is the real
    // reflection fix. REFLFIX used to default ON behind a DISABLE marker; when that marker went
    // missing from the game dir (2026-07-16 reinstall shuffle) the freeze silently re-armed and
    // fought the reflection camera every frame - "the reflection issue is back" with zero code
    // change. Dead experiments must be explicit opt-in, never opt-out.
    g_xScissorFixEnabled.store(!MarkerEnabled(L"MELEVR_DISABLE_XSCISSOR.txt"), std::memory_order_relaxed);
    g_qpostFixEnabled.store(MarkerEnabled(L"MELEVR_ENABLE_QPOST_EXPERIMENT.txt"), std::memory_order_relaxed);
    g_uberFillEnabled.store(!MarkerEnabled(L"MELEVR_DISABLE_UBERFILL.txt"), std::memory_order_relaxed);
    g_reflFixDisabled.store(!MarkerEnabled(L"MELEVR_ENABLE_REFLFIX_EXPERIMENT.txt"), std::memory_order_relaxed);
    g_reflLogArmed.store(MarkerEnabled(L"MELEVR_ENABLE_REFLLOG.txt"), std::memory_order_relaxed);
    g_reflCenDumpArmed.store(MarkerEnabled(L"MELEVR_ENABLE_REFLCEN_DUMP.txt"), std::memory_order_relaxed);
    ReflLogOnPresent();   // [REFLLOG] per-present HEAD line + write-sequence reset
    if ((n % 1800) == 5 && g_reflFixHaveInputs.load(std::memory_order_acquire))
    {
        char rfLine[224] = {};
        std::snprintf(rfLine, sizeof(rfLine),
                      "[REFLFIX] stats seen96=%llu freezes=%llu held=%llu gates: struct=%llu dz=%llu xy=%llu",
                      g_reflFixSeen.load(std::memory_order_relaxed),
                      g_reflFixApplied.load(std::memory_order_relaxed),
                      g_reflFixHeld.load(std::memory_order_relaxed),
                      g_reflFixGateStruct.load(std::memory_order_relaxed),
                      g_reflFixGateDz.load(std::memory_order_relaxed),
                      g_reflFixGateXy.load(std::memory_order_relaxed));
        LogLine(rfLine);
    }

    // [SFR] one-shot pass-texture preallocation at a calm present (render thread, swapchain warm),
    // instead of the ~half-GB burst inside the first mid-frame CaptureStereoPass (16:40 crash suspect).
    if (n == 120 && MarkerEnabled(L"MELEVR_ENABLE_SFR_M0.txt"))
    {
        MELEVR::D3DCapture::LogVideoMemory("sfr-prealloc-before", true);
        const bool preallocOk = MELEVR::D3DCapture::PreallocateStereoPassTextures();
        MELEVR::D3DCapture::LogVideoMemory("sfr-prealloc-after", true);
        LogLine(std::string("[SFR] pass-texture preallocation ") + (preallocOk ? "OK" : "FAILED"));
    }

    // [SFR-CAP] pass 1 = the backbuffer as it goes to Present; publishes the pair, resets per-present state.
    SfrOnPresent();
    ReflCenOnPresent(n);   // [REFLCEN] reflection-capture census flush (trace presents only)

    if (n == 1)
    {
        LogLine("[CAPTURE] first Present seen.");
    }
    else if (n == 600)
    {
        LogLine("[CAPTURE] 600 frames presented; Present hook stable.");
    }

    // GameCamera::TickSfxCameraProbe (the per-present SFXCameraMode ownership probe) was REMOVED 2026-07-03
    // (strip-firstperson-livecode) - it scanned/wrote live game camera objects every present = the gameplay hitch.
    MELEVR::RenderHook::NotifyPresentTick();
    // Lazy Bink hook install: bink2w64.dll loads with the game, but retry until seen (cheap once-per-present
    // GetModuleHandle until it succeeds, then never again).
    if (!g_binkHooksTried)
    {
        if (HMODULE bink = GetModuleHandleW(L"bink2w64.dll"))
        {
            g_binkHooksTried = true;
            InstallInlineHook(reinterpret_cast<void*>(GetProcAddress(bink, "BinkDoFrame")),
                              reinterpret_cast<void*>(&HookBinkDoFrame),
                              reinterpret_cast<void**>(&g_origBinkDoFrame), "Bink.DoFrame");
            InstallInlineHook(reinterpret_cast<void*>(GetProcAddress(bink, "BinkDoFrameAsync")),
                              reinterpret_cast<void*>(&HookBinkDoFrameAsync),
                              reinterpret_cast<void**>(&g_origBinkDoFrameAsync), "Bink.DoFrameAsync");
            InstallInlineHook(reinterpret_cast<void*>(GetProcAddress(bink, "BinkDoFrameAsyncWait")),
                              reinterpret_cast<void*>(&HookBinkDoFrameAsyncWait),
                              reinterpret_cast<void**>(&g_origBinkDoFrameAsyncWait), "Bink.DoFrameAsyncWait");
        }
    }
    // Per-present diagnostic logging removed 2026-07-03: LogVideoMemory queried GPU adapter memory once a
    // second (a periodic gameplay hitch) and LogDeviceHealth polled the device every present. Nothing reads
    // their output - pure diagnostics - so this is a zero-risk removal that targets the once-a-second hitch.
    TryPresentDepthViz(swapChain);   // flat/no-headset DIBR lab only; never paints the XR backbuffer
    TryPresentFlatMenu(swapChain);   // no-headset lab mode: draw the tuning UI onto the monitor backbuffer

    // Drive the OpenXR frame loop on the game's Present thread and copy this frame into the eyes.
    const bool openXrDisabled = IsOpenXrDisabledForBoundary();
    if (!openXrDisabled)
    {
        MELEVR::XrSession::OnPresent(swapChain);
    }
    else
    {
        MELEVR::XrSession::FlatHudTick();   // no-headset flat test: run HUD/subtitle controls without XR
    }

    // Per-eye UI dup verification: how many UI-signature draws were duplicated this window, split by
    // draw path (panels=DI, text=Dnon, instanced=DII). Zero when not in stereo or when the toggle is off.
    if ((n % 180) == 0)
    {
        char ub[192] = {};
        sprintf_s(ub, "[UISTATE]@%llu stereoUI=%d dup=%d DI=%u Dnon=%u DII=%u total=%u",
                  static_cast<unsigned long long>(n),
                  g_stereoUiActive.load(std::memory_order_acquire) ? 1 : 0,
                  g_uiDupEnabled.load(std::memory_order_acquire) ? 1 : 0,
                  g_uiDI.exchange(0, std::memory_order_relaxed),
                  g_uiDnon.exchange(0, std::memory_order_relaxed),
                  g_uiDII.exchange(0, std::memory_order_relaxed),
                  g_cntUiDup.exchange(0, std::memory_order_relaxed));
        LogLine(ub);
    }

    const ULONGLONG now = GetTickCount64();
    if (g_presentWindowStartMs == 0)
    {
        g_presentWindowStartMs = now;
        g_presentWindowFrames = 0;
    }
    ++g_presentWindowFrames;
    const ULONGLONG elapsed = now - g_presentWindowStartMs;
    if (elapsed >= 2000)
    {
        const double fps = (static_cast<double>(g_presentWindowFrames) * 1000.0) / static_cast<double>(elapsed);
        char line[256] = {};
        sprintf_s(line, "[PRESENTHZ] fps=%.1f frames=%u windowMs=%llu originalSync=%u originalFlags=0x%X forcedSync=0 forcedTear=%d swapTearOK=%d",
                  fps,
                  g_presentWindowFrames,
                  static_cast<unsigned long long>(elapsed),
                  g_lastOriginalSyncInterval,
                  g_lastOriginalPresentFlags,
                  g_forceTearThisPresent ? 1 : 0,
                  g_swapchainHasTearingFlag.load(std::memory_order_acquire) ? 1 : 0);
        LogLine(line);
        g_presentWindowStartMs = now;
        g_presentWindowFrames = 0;
    }

    PresentFn original = g_originalPresent;
    if (original == nullptr)
    {
        return DXGI_ERROR_INVALID_CALL;
    }
    // [MIRRORTHROTTLE] ME2's fix, ported 2026-08-12. While the headset is being driven, the flat window
    // is only a mirror - XrSession::OnPresent above has ALREADY submitted this frame to the compositor,
    // so the real Present below is cosmetic. Presenting it every frame is what capped the game at 60fps:
    // measured here, the moment the frame pacer stopped holding, average frame time jumped 5.8ms -> 14.2ms
    // with identical engine work. That is presentation back-pressure - the runtime stalls the render
    // thread when it needs a backbuffer the compositor has not released - not the game running slower,
    // and it is why the pacer kept concluding 120 was unreachable and settling on a metronomic 60.
    // ME2 measured the same thing (focused 59.6-60.2 locked vs 76-112 unfocused) and fixed it exactly
    // this way, going 87 -> 119.7fps. The mirror becomes visibly choppy BY DESIGN; mirrorPresentEvery=1
    // restores the old present-every-frame behaviour if it is ever needed.
    {
        const int every = MELEVR::Config::Get().mirrorPresentEvery;
        if (every > 1 && MELEVR::XrSession::IsHeadsetDriven())
        {
            static unsigned long long s_mirrorTick = 0;
            if ((++s_mirrorTick % static_cast<unsigned long long>(every)) != 0)
            {
                static bool s_logged = false;
                if (!s_logged)
                {
                    s_logged = true;
                    LogLine("[MIRRORTHROTTLE] flat mirror present throttled to 1 frame in " +
                            std::to_string(every) + " (headset already submitted; mirror is cosmetic)");
                }
                return S_OK;
            }
        }
    }
    // [TEARING] force syncInterval=0 (as always) AND, when allowed+supported, DXGI_PRESENT_ALLOW_TEARING so the
    // present rate is not bound to the monitor's composition rate. g_forceTearThisPresent was decided at the top.
    const UINT presentFlags = g_forceTearThisPresent ? (flags | kDxgiPresentAllowTearing) : flags;
    return original(swapChain, 0, presentFlags);
}
// Caller must hold g_hookMutex.
void InstallPresentHook(IDXGISwapChain* swapChain) noexcept
{
    if (swapChain == nullptr || g_presentSlot != nullptr)
    {
        return;
    }
    void** vtable = *reinterpret_cast<void***>(swapChain);
    if (vtable == nullptr)
    {
        return;
    }
    void** slot = &vtable[kPresentVTableIndex];
    if (PatchPointerSlot(slot, reinterpret_cast<void*>(&PresentHook), reinterpret_cast<void**>(&g_originalPresent),
                         "IDXGISwapChain::Present"))
    {
        g_presentSlot = slot;
    }
}

void CaptureSwapChainInfoOnce(IDXGISwapChain* swapChain) noexcept
{
    bool expected = false;
    if (!g_captured.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return;
    }
    if (swapChain == nullptr)
    {
        return;
    }

    LogLine("[CAPTURE] game swapchain " + HexPointer(swapChain));

    // Backbuffer format first - the OpenXR eye swapchains are created to match it.
    int64_t backbufferFormat = 0;
    uint32_t backbufferWidth = 0;
    uint32_t backbufferHeight = 0;
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) && backBuffer != nullptr)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        backBuffer->GetDesc(&desc);
        backbufferFormat = static_cast<int64_t>(desc.Format);
        backbufferWidth = desc.Width;
        backbufferHeight = desc.Height;
        LogLine("[CAPTURE] backbuffer " + std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
                " " + DxgiFormatName(desc.Format));
        SafeRelease(backBuffer);
    }
    else
    {
        LogLine("[CAPTURE] GetBuffer(0) FAILED.");
    }

    ID3D11Device* device = nullptr;
    if (SUCCEEDED(swapChain->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&device))) && device != nullptr)
    {
        LogLine("[CAPTURE] ID3D11Device " + HexPointer(device));
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        if (context != nullptr)
        {
            LogLine("[CAPTURE] immediate context " + HexPointer(context));
            if (g_gameContext == nullptr)
            {
                g_gameContext = context;
                g_gameContext->AddRef();
            }
            InstallContextDrawHooks(context);
            SafeRelease(context);
        }
        if (g_gameDevice == nullptr)
        {
            g_gameDevice = device;
            g_gameDevice->AddRef();
        }
        InstallDeviceHooks(device);
        if (g_gameSwapChain == nullptr)
        {
            g_gameSwapChain = swapChain;
            g_gameSwapChain->AddRef();
        }
        // Install render-side probes independently of OpenXR so flat/no-headset boundary hunts still work.
        MELEVR::RenderHook::Install();

        if (IsOpenXrDisabledForBoundary())
        {
            bool loggedExpected = false;
            if (g_openXrDisableLogged.compare_exchange_strong(loggedExpected, true, std::memory_order_acq_rel))
            {
                LogLine("[BOUNDARY] OpenXR disabled by MELEVR_DISABLE_OPENXR; render probes remain active for flat testing.");
            }
        }
        else
        {
            // Hand the captured device + backbuffer format/size to OpenXR.
            MELEVR::XrSession::TryCreateSession(device, backbufferFormat, backbufferWidth, backbufferHeight);
        }
        SafeRelease(device);
    }
    else
    {
        LogLine("[CAPTURE] GetDevice(ID3D11Device) FAILED.");
    }
}

// Common tail: on a freshly created swapchain, capture device/backbuffer once + hook Present.
void OnSwapChainCreated(IDXGISwapChain* swapChain) noexcept
{
    if (swapChain == nullptr)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(g_hookMutex);
    CaptureSwapChainInfoOnce(swapChain);
    InstallPresentHook(swapChain);
}

HRESULT STDMETHODCALLTYPE CreateSwapChainHook(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
                                              IDXGISwapChain** swapChain) noexcept
{
    CreateSwapChainFn original = g_originalCreateSwapChain;
    if (original == nullptr)
    {
        return DXGI_ERROR_INVALID_CALL;
    }
    // [TEARING] legacy path (LE1 uses ForHwnd, but keep this in sync so a recreate here can't leave a stale flag).
    if (desc != nullptr)
        g_swapchainHasTearingFlag.store((desc->Flags & kDxgiSwapChainFlagAllowTearing) != 0,
                                        std::memory_order_release);
    const HRESULT hr = original(factory, device, desc, swapChain);  // swapchain force reverted (see ForHwnd note)
    LogLine("[CAPTURE] CreateSwapChain returned " + HexHRESULT(hr));
    if (SUCCEEDED(hr) && swapChain != nullptr && *swapChain != nullptr)
    {
        OnSwapChainCreated(*swapChain);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForHwndHook(IDXGIFactory2* factory, IUnknown* device, HWND hWnd,
                                                     const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fsDesc, IDXGIOutput* output,
                                                     IDXGISwapChain1** swapChain) noexcept
{
    CreateSwapChainForHwndFn original = g_originalCreateSwapChainForHwnd;
    if (original == nullptr)
    {
        return DXGI_ERROR_INVALID_CALL;
    }
    // NOTE: the old comment here named ULocalPlayer::DynamicResolutionFraction (0x5C4) as "the live lever".
    // That is FALSE, proven 2026-07-10: the offset is right and the write sticks (engine never stomps it),
    // but nothing in LE1's renderer reads it - at frac=0.5 the viewport stayed 3840x2160. The field is inert.
    //
    // [SCREQ] What the game ASKS for vs what it GETS vs what the OS reports. This discriminates the two very
    // different bugs behind "a 1080p monitor gets a soft image":
    //   asked 5120x2880, got 3840x2160  -> DXGI/window clamped the request; hook the display/window layer.
    //   asked 3840x2160                 -> the GAME clamped itself before ever calling DXGI; find its
    //                                      display-mode query (GetDisplayModeList / EnumDisplaySettings)
    //                                      and lie to it there.
    if (desc != nullptr)
    {
        g_screqSeen.store(true, std::memory_order_release);   // [DISPQ] everything after this is post-clamp

        // [TEARING] record whether this swapchain permits DXGI_PRESENT_ALLOW_TEARING (see globals note).
        g_swapchainHasTearingFlag.store((desc->Flags & kDxgiSwapChainFlagAllowTearing) != 0,
                                        std::memory_order_release);

        // [INICHK] What does GamerSettings.ini say RIGHT NOW, at the moment the game commits to a size?
        // The target was written in DllMain and read back verified. If it still reads the target here, the
        // game read the file and ignored it (its resolution comes from elsewhere). If it reads the OLD value,
        // something rewrote the file afterward. Different bugs, different fixes - don't guess.
        {
            UINT nowW = 0, nowH = 0;
            const bool ok = ReadRequestedResFromGamerSettings(&nowW, &nowH);
            char chk[192];
            std::snprintf(chk, sizeof(chk),
                          "[INICHK] at swapchain: GamerSettings=%ux%u(read=%d) autoResTarget=%ux%u requested=%ux%u",
                          nowW, nowH, ok ? 1 : 0, g_autoRes.targetW, g_autoRes.targetH,
                          desc->Width, desc->Height);
            LogLine(chk);
        }

        // [HDRCHK] Same idea, for HDR: what does the game's OWN persisted setting say right now, paired
        // with fmt= a few lines below (in the same [SCREQ] line) which is what the game actually asked
        // DXGI for. Read-only, zero risk - see LogHdrSettingFromGamerSettings for the full reasoning.
        LogHdrSettingFromGamerSettings();

        RECT rc = {};
        const bool haveRect = (hWnd != nullptr) && GetClientRect(hWnd, &rc);

        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        const bool haveMode = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) != 0;

        char line[320];
        std::snprintf(line, sizeof(line),
                      "[SCREQ] requested=%ux%u fmt=%d windowed=%d clientRect=%ldx%ld desktopMode=%ux%u "
                      "SM_CXSCREEN=%dx%d",
                      desc->Width, desc->Height, static_cast<int>(desc->Format),
                      (fsDesc == nullptr) ? 1 : (fsDesc->Windowed ? 1 : 0),
                      haveRect ? (rc.right - rc.left) : -1L, haveRect ? (rc.bottom - rc.top) : -1L,
                      haveMode ? dm.dmPelsWidth : 0u, haveMode ? dm.dmPelsHeight : 0u,
                      GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        LogLine(line);
    }

    const HRESULT hr = original(factory, device, hWnd, desc, fsDesc, output, swapChain);
    LogLine("[CAPTURE] CreateSwapChainForHwnd returned " + HexHRESULT(hr));
    if (SUCCEEDED(hr) && swapChain != nullptr && *swapChain != nullptr)
    {
        OnSwapChainCreated(*swapChain);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindowHook(IDXGIFactory2* factory, IUnknown* device, IUnknown* window,
                                                           const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output,
                                                           IDXGISwapChain1** swapChain) noexcept
{
    CreateSwapChainForCoreWindowFn original = g_originalCreateSwapChainForCoreWindow;
    if (original == nullptr)
    {
        return DXGI_ERROR_INVALID_CALL;
    }
    const HRESULT hr = original(factory, device, window, desc, output, swapChain);
    LogLine("[CAPTURE] CreateSwapChainForCoreWindow returned " + HexHRESULT(hr));
    if (SUCCEEDED(hr) && swapChain != nullptr && *swapChain != nullptr)
    {
        OnSwapChainCreated(*swapChain);
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE CreateSwapChainForCompositionHook(IDXGIFactory2* factory, IUnknown* device,
                                                            const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* output,
                                                            IDXGISwapChain1** swapChain) noexcept
{
    CreateSwapChainForCompositionFn original = g_originalCreateSwapChainForComposition;
    if (original == nullptr)
    {
        return DXGI_ERROR_INVALID_CALL;
    }
    const HRESULT hr = original(factory, device, desc, output, swapChain);
    LogLine("[CAPTURE] CreateSwapChainForComposition returned " + HexHRESULT(hr));
    if (SUCCEEDED(hr) && swapChain != nullptr && *swapChain != nullptr)
    {
        OnSwapChainCreated(*swapChain);
    }
    return hr;
}

void HookSlot(void** vtable, UINT index, void* replacement, void** original, void*** slotStore, const char* name) noexcept
{
    if (vtable == nullptr || *slotStore != nullptr)
    {
        return;
    }
    void** slot = &vtable[index];
    if (PatchPointerSlot(slot, replacement, original, name))
    {
        *slotStore = slot;
    }
}
}

namespace MELEVR::D3DCapture
{
// [INICHK] Who touches GamerSettings.ini, when, and for reading or writing? Installed from the worker thread
// (a few hundred ms after the DllMain write), so it will not see that write - by design: this wants to know
// about anyone ELSE touching the file before the swapchain is created.
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileWFn g_origCreateFileW = nullptr;
std::atomic<int> g_iniOpenLogs{0};

// [EXITKILL] Quest Link exit-hang fix (2026-07-19). On Meta's PC runtime the game WEDGES on exit and must be
// killed by hand. Root: the OpenXR frame loop runs on the game's Present thread, so once the game stops
// presenting, a clean xrEndSession/xrDestroyInstance is never driven, and Meta's runtime DLL then hangs inside
// its OWN DllMain(DETACH) during the normal ExitProcess unload chain (log ends abruptly mid-present, no
// STOPPING/EXITING - the clean path never ran). Fix: intercept ExitProcess at its ENTRY, before the DLL-detach
// chain executes, and hard-terminate. TerminateProcess runs NO detach handlers, no CRT atexit, no runtime
// teardown, so the hang cannot occur. No downside: the user was already force-killing via Task Manager, and
// ME1 persists its state during play, not on exit. VDXR never hung; this is runtime-agnostic and harmless there.
using ExitProcessFn = void(WINAPI*)(UINT);
ExitProcessFn g_origExitProcess = nullptr;
void WINAPI ExitProcessHook(UINT uExitCode) noexcept
{
    TerminateProcess(GetCurrentProcess(), uExitCode);   // never returns; skips the hanging detach chain
}

// Guard: WriteResToGamerSettings opens the file itself, which re-enters this hook.
thread_local bool t_inIniRewrite = false;

// Writing the target in DllMain is NOT enough. Proven 2026-07-10: 6144x3456 gets written and verified on disk,
// then ~8s later the file is rewritten to the launcher's value and the game reads THAT. There is no control over
// who writes the file or when. So don't race it - rewrite the target into the file at the exact moment the
// game opens it for reading, which is the last possible instant before it parses the number.
HANDLE WINAPI CreateFileWHook(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                              DWORD disp, DWORD flags, HANDLE tmpl) noexcept
{
    bool isGamerSettings = false;
    if (name != nullptr && !t_inIniRewrite)
    {
        isGamerSettings = (wcsstr(name, L"GamerSettings.ini") != nullptr) ||
                          (wcsstr(name, L"gamersettings.ini") != nullptr);
    }

    // Intercept the READ open, before the handle exists: put the target value in the file first.
    if (isGamerSettings && !g_screqSeen.load(std::memory_order_acquire) &&
        (access & GENERIC_READ) != 0 && (access & GENERIC_WRITE) == 0 &&
        g_autoRes.targetW != 0 && g_autoRes.targetH != 0)
    {
        t_inIniRewrite = true;
        const bool ok = WriteResToGamerSettings(g_autoRes.targetW, g_autoRes.targetH);
        t_inIniRewrite = false;

        char line[192];
        std::snprintf(line, sizeof(line),
                      "[INICHK] game opening GamerSettings.ini to READ - injected %ux%u first (ok=%d)",
                      g_autoRes.targetW, g_autoRes.targetH, ok ? 1 : 0);
        LogLine(line);
    }

    const HANDLE h = (g_origCreateFileW != nullptr)
                         ? g_origCreateFileW(name, access, share, sa, disp, flags, tmpl)
                         : INVALID_HANDLE_VALUE;

    if (isGamerSettings && !g_screqSeen.load(std::memory_order_acquire))
    {
        const int n = g_iniOpenLogs.fetch_add(1, std::memory_order_relaxed);
        if (n < 12)
        {
            char line[192];
            std::snprintf(line, sizeof(line),
                          "[INICHK] CreateFileW GamerSettings.ini access=0x%08X(%s%s) disp=%u ok=%d",
                          access,
                          (access & GENERIC_WRITE) ? "WRITE" : "",
                          (access & GENERIC_READ) ? "READ" : "",
                          disp, (h != INVALID_HANDLE_VALUE) ? 1 : 0);
            LogLine(line);
        }
    }
    return h;
}

// Runs in DllMain, before main(), before the engine has read a single config file. Everything here is file
// I/O plus GetSystemMetrics (user32 is already loaded by then). No LoadLibrary, no thread creation.
void ApplyAutoResEarly() noexcept
{
    // Snapshot the REAL primary size BEFORE any hook exists, or the hooks would later compare against
    // their own lie.
    g_realPrimaryW = static_cast<UINT>(GetSystemMetrics(SM_CXSCREEN));
    g_realPrimaryH = static_cast<UINT>(GetSystemMetrics(SM_CYSCREEN));

    UINT wantW = 0, wantH = 0;
    const bool haveWant = ReadRequestedResFromGamerSettings(&wantW, &wantH);

    // Auto-resolution only engages once a previous launch cached the headset's eye size. The very first
    // launch (or after deleting the sidecar) behaves exactly as before, off whatever is in GamerSettings.ini.
    ReadAutoResFile(&g_autoRes);
    const char* source = "GamerSettings.ini";

    // Stereo only (vrMode 2). AER and DIBR want a roughly SQUARE render - the opposite aspect - so a 16:9
    // target would silently break them. They keep whatever the bat/user put in GamerSettings.ini.
    const bool autoResApplies = (g_autoRes.enabled != 0) && (g_autoRes.eyeW > 0) && (g_autoRes.vrMode == 2);
    bool wroteIni = false;
    bool verified = false;

    if (autoResApplies)
    {
        unsigned tw = g_autoRes.targetW;
        unsigned th = g_autoRes.targetH;
        if (tw == 0 || th == 0)
        {
            ComputeIdealTarget(g_autoRes.eyeW, g_autoRes.eyeH, &tw, &th);   // never calibrated: start at ideal
            source = "auto(ideal)";
        }
        else
        {
            source = "auto(calibrated)";
        }

        // The game renders min(ini, monitor), so BOTH must name the target: the ini here, the monitor via the
        // hooks installed later. Read it back - a silently failed write is exactly how the 08:31 run shipped
        // a 5120 backbuffer while claiming 6144.
        if (tw != wantW || th != wantH)
        {
            wroteIni = WriteResToGamerSettings(tw, th);
            UINT vw = 0, vh = 0;
            verified = ReadRequestedResFromGamerSettings(&vw, &vh) && vw == tw && vh == th;
        }
        else
        {
            wroteIni = true;
            verified = true;   // already correct on disk
        }

        wantW = tw;
        wantH = th;

        // Set the in-memory target unconditionally: the CreateFileW hook re-injects it when the game opens
        // the ini to read, and that path is what actually decides the resolution. The disk write here is now
        // only a best-effort head start - something else rewrites the file before the game reads it.
        const bool changed = (g_autoRes.targetW != tw || g_autoRes.targetH != th);
        g_autoRes.targetW = tw;
        g_autoRes.targetH = th;
        if (changed) WriteAutoResFile(g_autoRes);
    }

    // Arm ONLY when the target exceeds the desktop. At or below native this is a no-op and every display
    // hook becomes a pure pass-through - a system whose monitor already covers the target sees no change.
    if (wantW != 0 && g_realPrimaryW != 0 && g_realPrimaryH != 0 &&
        (wantW > g_realPrimaryW || wantH > g_realPrimaryH))
    {
        g_spoofW = wantW;
        g_spoofH = wantH;
    }

    char line[288];
    std::snprintf(line, sizeof(line),
                  "[DISPQ] realPrimary=%ux%u target=%ux%u(read=%d src=%s) hmdEye=%ux%u iniWrite=%d verified=%d "
                  "spoof=%s -> %ux%u",
                  g_realPrimaryW, g_realPrimaryH, wantW, wantH, haveWant ? 1 : 0, source,
                  g_autoRes.eyeW, g_autoRes.eyeH, wroteIni ? 1 : 0, verified ? 1 : 0,
                  SpoofActive() ? "ARMED" : "off", g_spoofW, g_spoofH);
    LogLine(line);
}

// Installed from the worker thread. These intercept live API calls the game makes during D3D init, well
// after ApplyAutoResEarly has already put the target on disk - no race here.
// ============================================================================
// [SVRFIX] SteamVR scene-submit repair: strip D3D11_CREATE_DEVICE_SINGLETHREADED.
//
// Proven 2026-07-20 ([SVRDIAG]): LE1 creates its device with creationFlags=0x1 (SINGLETHREADED), and
// D3D11 refuses keyed-mutex shared textures on such a device (CreateTexture2D KEYEDMUTEX ->
// E_INVALIDARG while plain SHARED succeeds). SteamVR's in-process client must create exactly that
// keyed-mutex "sync texture" on the session's device before it will accept scene frames, so every
// ComposeLayerProjection failed with VRCompositorError_SharedTexturesNotSupported (xrclient log) while
// xrEndFrame still reported success - projection invisible, quad overlays fine, on SteamVR only
// (Meta/VDXR never create a keyed-mutex sync texture). Clearing the flag only makes D3D restore its
// own internal locking - a strict superset of singlethreaded semantics, and this mod already calls
// the device from the Present + XR threads, so the flag was a lie the moment it was injected.
// ============================================================================
using D3D11CreateDeviceFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                             const D3D_FEATURE_LEVEL*, UINT, UINT,
                                             ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using D3D11CreateDeviceAndSwapChainFn = HRESULT(WINAPI*)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                         const D3D_FEATURE_LEVEL*, UINT, UINT,
                                                         const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**,
                                                         ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
D3D11CreateDeviceFn g_origD3D11CreateDevice = nullptr;
D3D11CreateDeviceAndSwapChainFn g_origD3D11CreateDeviceAndSwapChain = nullptr;

UINT StripSingleThreadedFlag(UINT flags, const char* which) noexcept
{
    if ((flags & D3D11_CREATE_DEVICE_SINGLETHREADED) == 0) return flags;
    LogLine(std::string("[SVRFIX] ") + which + " requested SINGLETHREADED (flags=" +
            std::to_string(flags) + ") - stripping so SteamVR's keyed-mutex sync texture can be created.");
    return flags & ~static_cast<UINT>(D3D11_CREATE_DEVICE_SINGLETHREADED);
}

// [SVRFIX2] Is SteamVR the active OpenXR runtime? Read HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime
// (the JSON path the loader will use) BEFORE any XR instance exists, so device creation can gate on it.
// Cached: the hook fires several times and the registry value does not change mid-run.
bool IsSteamVrActiveRuntime() noexcept
{
    static int cached = -1;
    if (cached >= 0) return cached != 0;
    cached = 0;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ, &key) == ERROR_SUCCESS)
    {
        wchar_t path[1024] = {};
        DWORD cb = sizeof(path) - sizeof(wchar_t);
        DWORD type = 0;
        if (RegQueryValueExW(key, L"ActiveRuntime", nullptr, &type,
                             reinterpret_cast<LPBYTE>(path), &cb) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ))
        {
            for (wchar_t* p = path; *p; ++p) *p = towlower(*p);
            if (wcsstr(path, L"steamvr") != nullptr || wcsstr(path, L"steamxr") != nullptr) cached = 1;
        }
        RegCloseKey(key);
    }
    LogLine(std::string("[SVRFIX2] active OpenXR runtime is SteamVR: ") + (cached ? "yes" : "no"));
    return cached != 0;
}

// [SVRFIX2] Convert the game's device-creation recipe to the one that PROVABLY supports keyed-mutex
// shared textures on this machine ([SVRDIAG2]: null-adapter HARDWARE device -> KEYEDMUTEX OK, while the
// game's explicit-adapter UNKNOWN device -> KEYEDMUTEX FAIL, same process/GPU/flags). SteamVR's client
// creates exactly such a keyed-mutex "sync texture" on the session device, so the game's device must be
// able to. The proven recipe (adapter=null, driverType=HARDWARE) is forced ONLY when the game used the
// failing pattern (UNKNOWN + explicit adapter) AND SteamVR is the active runtime - single-GPU here, so
// null resolves to the same adapter. Oculus/VDXR: this never runs, device creation bit-identical.
void MaybeForceKeyedMutexRecipe(IDXGIAdapter*& adapter, D3D_DRIVER_TYPE& driverType, const char* which) noexcept
{
    if (!IsSteamVrActiveRuntime()) return;
    if (driverType != D3D_DRIVER_TYPE_UNKNOWN || adapter == nullptr) return;
    LogLine(std::string("[SVRFIX2] ") + which +
            " used UNKNOWN+explicit-adapter (keyed-mutex-incapable on SteamVR); forcing null-adapter HARDWARE.");
    adapter = nullptr;
    driverType = D3D_DRIVER_TYPE_HARDWARE;
}

HRESULT WINAPI D3D11CreateDeviceHook(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType, HMODULE software,
                                     UINT flags, const D3D_FEATURE_LEVEL* levels, UINT levelCount,
                                     UINT sdkVersion, ID3D11Device** device, D3D_FEATURE_LEVEL* outLevel,
                                     ID3D11DeviceContext** context) noexcept
{
    flags = StripSingleThreadedFlag(flags, "D3D11CreateDevice");
    MaybeForceKeyedMutexRecipe(adapter, driverType, "D3D11CreateDevice");
    // [SVRDIAG2] the full creation signature of every device the game makes: a clean-flag game device
    // still fails KEYEDMUTEX (E_INVALIDARG) while an identically-flagged probe device outside the game
    // succeeds - the difference must live in one of these parameters or in process-wide state.
    {
        char pl[224];
        char fls[64] = {};
        for (UINT i = 0; i < levelCount && levels != nullptr && i < 4; ++i)
            std::snprintf(fls + strlen(fls), sizeof(fls) - strlen(fls), "%04X,", static_cast<unsigned>(levels[i]));
        std::snprintf(pl, sizeof(pl),
                      "[SVRDIAG2] D3D11CreateDevice adapter=%p driverType=%d flags=0x%X levels=[%s] n=%u sdk=%u",
                      reinterpret_cast<void*>(adapter), static_cast<int>(driverType), flags, fls, levelCount,
                      sdkVersion);
        LogLine(pl);
    }
    return g_origD3D11CreateDevice != nullptr
               ? g_origD3D11CreateDevice(adapter, driverType, software, flags, levels, levelCount,
                                         sdkVersion, device, outLevel, context)
               : E_FAIL;
}

HRESULT WINAPI D3D11CreateDeviceAndSwapChainHook(IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType,
                                                 HMODULE software, UINT flags, const D3D_FEATURE_LEVEL* levels,
                                                 UINT levelCount, UINT sdkVersion,
                                                 const DXGI_SWAP_CHAIN_DESC* scDesc, IDXGISwapChain** swapChain,
                                                 ID3D11Device** device, D3D_FEATURE_LEVEL* outLevel,
                                                 ID3D11DeviceContext** context) noexcept
{
    flags = StripSingleThreadedFlag(flags, "D3D11CreateDeviceAndSwapChain");
    MaybeForceKeyedMutexRecipe(adapter, driverType, "D3D11CreateDeviceAndSwapChain");
    return g_origD3D11CreateDeviceAndSwapChain != nullptr
               ? g_origD3D11CreateDeviceAndSwapChain(adapter, driverType, software, flags, levels, levelCount,
                                                     sdkVersion, scDesc, swapChain, device, outLevel, context)
               : E_FAIL;
}

// [DOF] Public entry: force GamerSettings.ini DepthOfField. Forwards to the anon-namespace worker (which
// also owns GameConfigPath and the preserving read-modify-write).
bool EnsureDepthOfFieldSetting(bool disable) noexcept
{
    return WriteDofToGamerSettings(disable);
}

bool EnsureBioEngineSmoothing() noexcept
{
    return EnsureBioEngineSmoothingWorker();
}

void InstallDisplayQueryHooks() noexcept
{
    // [SVRFIX] device-creation hooks first: this runs seconds before LE1 creates its device (measured:
    // hooks 17:39:29 vs device 17:39:35), and the flag must be gone at creation - it cannot be cleared
    // off a live device.
    {
        HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
        if (d3d11 == nullptr) d3d11 = LoadLibraryW(L"d3d11.dll");
        if (d3d11 != nullptr)
        {
            void* cd = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDevice"));
            if (cd != nullptr)
                InstallInlineHook(cd, reinterpret_cast<void*>(&D3D11CreateDeviceHook),
                                  reinterpret_cast<void**>(&g_origD3D11CreateDevice), "D3D11CreateDevice");
            void* cds = reinterpret_cast<void*>(GetProcAddress(d3d11, "D3D11CreateDeviceAndSwapChain"));
            if (cds != nullptr)
                InstallInlineHook(cds, reinterpret_cast<void*>(&D3D11CreateDeviceAndSwapChainHook),
                                  reinterpret_cast<void**>(&g_origD3D11CreateDeviceAndSwapChain),
                                  "D3D11CreateDeviceAndSwapChain");
        }
        else
        {
            LogLine("[SVRFIX] d3d11.dll not loadable - SINGLETHREADED strip unavailable this run.");
        }
    }

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr)
    {
        LogLine("[DISPQ] user32.dll not loaded - display query hooks skipped.");
        return;
    }

    void* enumW = reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsW"));
    void* enumA = reinterpret_cast<void*>(GetProcAddress(user32, "EnumDisplaySettingsA"));
    void* metrics = reinterpret_cast<void*>(GetProcAddress(user32, "GetSystemMetrics"));

    if (enumW != nullptr)
        InstallInlineHook(enumW, reinterpret_cast<void*>(&EnumDisplaySettingsWHook),
                          reinterpret_cast<void**>(&g_origEnumDisplaySettingsW), "EnumDisplaySettingsW");
    if (enumA != nullptr)
        InstallInlineHook(enumA, reinterpret_cast<void*>(&EnumDisplaySettingsAHook),
                          reinterpret_cast<void**>(&g_origEnumDisplaySettingsA), "EnumDisplaySettingsA");
    if (metrics != nullptr)
        InstallInlineHook(metrics, reinterpret_cast<void*>(&GetSystemMetricsHook),
                          reinterpret_cast<void**>(&g_origGetSystemMetrics), "GetSystemMetrics");

    // [DISPQ2] the remaining GDI/monitor candidates for the fourth source.
    void* monInfo = reinterpret_cast<void*>(GetProcAddress(user32, "GetMonitorInfoW"));
    if (monInfo != nullptr)
        InstallInlineHook(monInfo, reinterpret_cast<void*>(&GetMonitorInfoWHook),
                          reinterpret_cast<void**>(&g_origGetMonitorInfoW), "GetMonitorInfoW");

    void* monInfoA = reinterpret_cast<void*>(GetProcAddress(user32, "GetMonitorInfoA"));
    if (monInfoA != nullptr)
        InstallInlineHook(monInfoA, reinterpret_cast<void*>(&GetMonitorInfoAHook),
                          reinterpret_cast<void**>(&g_origGetMonitorInfoA), "GetMonitorInfoA");

    HMODULE gdi32 = GetModuleHandleW(L"gdi32.dll");
    if (gdi32 == nullptr) gdi32 = LoadLibraryW(L"gdi32.dll");
    if (gdi32 != nullptr)
    {
        void* caps = reinterpret_cast<void*>(GetProcAddress(gdi32, "GetDeviceCaps"));
        if (caps != nullptr)
            InstallInlineHook(caps, reinterpret_cast<void*>(&GetDeviceCapsHook),
                              reinterpret_cast<void**>(&g_origGetDeviceCaps), "GetDeviceCaps");
    }

    // [INICHK] catch anyone re-opening GamerSettings.ini before the swapchain exists.
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (k32 != nullptr)
    {
        void* cfw = reinterpret_cast<void*>(GetProcAddress(k32, "CreateFileW"));
        if (cfw != nullptr)
            InstallInlineHook(cfw, reinterpret_cast<void*>(&CreateFileWHook),
                              reinterpret_cast<void**>(&g_origCreateFileW), "CreateFileW");

        // [EXITKILL] hard-terminate on ExitProcess so Meta's Quest Link runtime teardown can't hang the exit.
        void* ep = reinterpret_cast<void*>(GetProcAddress(k32, "ExitProcess"));
        if (ep != nullptr)
            InstallInlineHook(ep, reinterpret_cast<void*>(&ExitProcessHook),
                              reinterpret_cast<void**>(&g_origExitProcess), "ExitProcess");
    }
}

bool GetRealPrimarySize(unsigned* outW, unsigned* outH) noexcept
{
    if (outW == nullptr || outH == nullptr) return false;
    if (g_realPrimaryW == 0 || g_realPrimaryH == 0) return false;
    *outW = g_realPrimaryW;
    *outH = g_realPrimaryH;
    return true;
}

// Cache the headset's recommended eye size for the NEXT launch. It cannot help this one: OpenXR only reports
// it after session creation, which is well after the backbuffer exists. First run stays vanilla; every run
// after sizes itself to the headset. Writes only on change - this is called every frame.
void NoteHmdEyeSize(unsigned eyeW, unsigned eyeH, int vrMode) noexcept
{
    if (eyeW == 0 || eyeH == 0) return;
    if (g_autoRes.enabled == 0) return;   // dormant - see AutoResState. Do not create/refresh the sidecar.

    const bool eyeChanged = (g_autoRes.eyeW != eyeW || g_autoRes.eyeH != eyeH);
    const bool modeChanged = (g_autoRes.vrMode != vrMode);
    if (!eyeChanged && !modeChanged) return;

    // A different headset (or a mode switch) invalidates a target calibrated for the old one - re-derive it
    // from the ideal and let the frame budget walk it back down.
    if (eyeChanged || modeChanged)
    {
        g_autoRes.targetW = 0;
        g_autoRes.targetH = 0;
    }
    g_autoRes.eyeW = eyeW;
    g_autoRes.eyeH = eyeH;
    g_autoRes.vrMode = vrMode;
    WriteAutoResFile(g_autoRes);

    unsigned iw = 0, ih = 0;
    ComputeIdealTarget(eyeW, eyeH, &iw, &ih);
    char line[192];
    std::snprintf(line, sizeof(line),
                  "[AUTORES] cached hmdEye=%ux%u vrMode=%d ideal=%ux%u (target resets, applies next launch)",
                  eyeW, eyeH, vrMode, iw, ih);
    LogLine(line);
}

bool IsDibrStereoReady() noexcept
{
    // Reconnected: when the Enhancements depth-map master is ON and depth is captured, the depth-warp path owns
    // the eyes (real same-frame stereo via RenderDibrGridEye). The submit branch makes AER yield to this.
    return g_depthMapEnabled.load(std::memory_order_relaxed) &&
           g_depthReady.load(std::memory_order_acquire) &&
           g_depthSrv != nullptr;
}

void SetStereoUiActive(bool active) noexcept
{
    g_stereoUiActive.store(active, std::memory_order_release);
}

void SetUiDupEnabled(bool enabled) noexcept
{
    g_uiDupEnabled.store(enabled, std::memory_order_release);
}

void SetStereoUiYShift(float frac) noexcept
{
    if (!(frac == frac)) frac = 0.0f;                 // NaN guard
    if (frac < -0.25f) frac = -0.25f;                 // keep the letterbox on-screen
    if (frac >  0.25f) frac =  0.25f;
    g_stereoUiYShift.store(frac, std::memory_order_relaxed);
}

bool GetUiDupEnabled() noexcept
{
    return g_uiDupEnabled.load(std::memory_order_acquire);
}

void SetDepthMapEnabled(bool enabled) noexcept
{
    g_depthMapEnabled.store(enabled, std::memory_order_relaxed);   // [M0.4] content-gated promotion, no blacklist
}

bool GetDepthMapEnabled() noexcept
{
    return g_depthMapEnabled.load(std::memory_order_relaxed);
}

void SetDepthVizShow(bool show) noexcept
{
    g_depthVizShow.store(show, std::memory_order_relaxed);
}

bool GetDepthVizShow() noexcept
{
    return g_depthVizShow.load(std::memory_order_relaxed);
}

void SetDepthMapTuning(float nearD, float farD, bool flip, float gamma) noexcept
{
    g_depthNear.store(nearD, std::memory_order_relaxed);
    g_depthFar.store(farD, std::memory_order_relaxed);
    g_depthFlip.store(flip ? 1.0f : 0.0f, std::memory_order_relaxed);
    g_depthGamma.store(gamma, std::memory_order_relaxed);
}

void SetDibrWarp(float gain, float convergence, bool flip) noexcept
{
    auto clampf = [](float v, float lo, float hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); };
    g_dibrGain.store(clampf(gain, 0.0f, 10.0f), std::memory_order_relaxed);
    g_dibrConvergence.store(clampf(convergence, 0.90f, 1.005f), std::memory_order_relaxed);
    g_dibrSign.store(flip ? -1.0f : 1.0f, std::memory_order_relaxed);
}

// Milliseconds since Bink last decoded a movie frame (ULLONG_MAX until the first frame ever).
unsigned long long LastBinkFrameAgeMs() noexcept
{
    const unsigned long long t = g_lastBinkFrameMs.load(std::memory_order_relaxed);
    if (t == 0) return ~0ull;
    const unsigned long long now = GetTickCount64();
    return now >= t ? (now - t) : 0;
}

void GetDepthProbe(float* center, float* tl, float* br, float* tr) noexcept
{
    if (center) *center = g_probeCenter.load(std::memory_order_relaxed);
    if (tl)     *tl     = g_probeTL.load(std::memory_order_relaxed);
    if (br)     *br     = g_probeBR.load(std::memory_order_relaxed);
    if (tr)     *tr     = g_probeTR.load(std::memory_order_relaxed);
}

// [RELIEF M0] public wrapper for the anon-namespace TryPresentDepthViz (internal linkage): paint the
// greyscale depth viz onto the swapchain's backbuffer NOW. xr_session calls it pre-eye-copy so the
// headset shows the captured depth; all the gates (enable + viz toggle + ready) live in the callee.
void PaintDepthVizNow(IDXGISwapChain* swapChain) noexcept
{
    TryPresentDepthViz(swapChain);
}

// [RELIEF M0] quarter-center taps (per-eye view centers in SBS stereo; plain L/R samples otherwise).
void GetDepthProbeLR(float* leftCenter, float* rightCenter) noexcept
{
    if (leftCenter)  *leftCenter  = g_probeLC.load(std::memory_order_relaxed);
    if (rightCenter) *rightCenter = g_probeRC.load(std::memory_order_relaxed);
}

// [RELIEF M0] capture stats: current depth-copy size + cumulative copy count (caller diffs for a rate).
// W/H are plain fields written on the render thread - a racy read here is diagnostic-benign.
void GetDepthCaptureStats(unsigned* width, unsigned* height, unsigned* copies) noexcept
{
    if (width)  *width  = g_depthCopyW;
    if (height) *height = g_depthCopyH;
    if (copies) *copies = g_depthCopyCount.load(std::memory_order_relaxed);
}

// [RELIEF M0.1/M0.3] snapshot of the per-resource depth-draw table, SORTED by draw count desc, so the
// real scene depth (highest draws) always shows even if it's in a high slot. Racy-benign diagnostic.
int GetDepthDrawTableSnapshot(void** resOut, unsigned* drawsOut, int maxEntries) noexcept
{
    void* r[16] = {}; unsigned d[16] = {}; int total = 0;
    for (const auto& e : g_depthDraws)
    {
        if (e.dsv == nullptr) continue;
        r[total] = e.dsv; d[total] = e.draws; ++total;
    }
    for (int i = 1; i < total; ++i)   // insertion sort desc (<=16 entries)
    {
        void* rk = r[i]; unsigned dk = d[i]; int j = i - 1;
        while (j >= 0 && d[j] < dk) { r[j + 1] = r[j]; d[j + 1] = d[j]; --j; }
        r[j + 1] = rk; d[j + 1] = dk;
    }
    int n = total < maxEntries ? total : maxEntries;
    for (int i = 0; i < n; ++i) { if (resOut) resOut[i] = r[i]; if (drawsOut) drawsOut[i] = d[i]; }
    return n;
}

// [RELIEF M0.3] draw-attribution meter: total draws seen vs those attributed to a depth resource, since
// the last call (read-and-reset). total>>attrib for a mode = its depth is bound by a path not hooked.
void GetDrawAttribStats(unsigned* total, unsigned* attributed) noexcept
{
    if (total)      *total      = g_dbgDrawsTotal.exchange(0, std::memory_order_relaxed);
    if (attributed) *attributed = g_dbgDrawsAttrib.exchange(0, std::memory_order_relaxed);
}

void SetRenderResolution(unsigned width, unsigned height) noexcept
{
    g_sceneRenderW.store(width, std::memory_order_release);
    g_sceneRenderH.store(height, std::memory_order_release);
}

ID3D11Texture2D* RenderDibrEye(ID3D11Texture2D* backBuffer, ID3D11Texture2D* outTex, ID3D11RenderTargetView* outRtv, float eyeScale) noexcept
{
    if (g_gameContext == nullptr || g_gameDevice == nullptr || backBuffer == nullptr) return nullptr;
    if (outTex == nullptr || outRtv == nullptr) return nullptr;
    if (!g_depthReady.load(std::memory_order_acquire) || g_depthSrv == nullptr) return nullptr;
    if (!EnsureDepthVizShaders()) return nullptr;
    D3D11_TEXTURE2D_DESC bbd = {}; backBuffer->GetDesc(&bbd);
    g_gameContext->CopyResource(g_dibrColorCopy, backBuffer);   // snapshot the finished frame to sample

    ID3D11RenderTargetView* oldRtv[8] = {}; ID3D11DepthStencilView* oldDsv = nullptr;
    g_gameContext->OMGetRenderTargets(8, oldRtv, &oldDsv);
    D3D11_VIEWPORT oldVp[16] = {}; UINT oldVpN = 16; g_gameContext->RSGetViewports(&oldVpN, oldVp);
    ID3D11RasterizerState* oldRs = nullptr; g_gameContext->RSGetState(&oldRs);
    ID3D11DepthStencilState* oldDs = nullptr; UINT oldRef = 0; g_gameContext->OMGetDepthStencilState(&oldDs, &oldRef);
    float oldBlendFactor[4] = {}; UINT oldSampleMask = 0xffffffff; ID3D11BlendState* oldBlend = nullptr;
    g_gameContext->OMGetBlendState(&oldBlend, oldBlendFactor, &oldSampleMask);
    D3D11_PRIMITIVE_TOPOLOGY oldTopo; g_gameContext->IAGetPrimitiveTopology(&oldTopo);
    ID3D11InputLayout* oldIl = nullptr; g_gameContext->IAGetInputLayout(&oldIl);
    ID3D11VertexShader* oldVs = nullptr; g_gameContext->VSGetShader(&oldVs, nullptr, nullptr);
    ID3D11PixelShader* oldPs = nullptr; g_gameContext->PSGetShader(&oldPs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSrv[2] = {}; g_gameContext->PSGetShaderResources(0, 2, oldSrv);
    ID3D11SamplerState* oldSamp = nullptr; g_gameContext->PSGetSamplers(0, 1, &oldSamp);

    D3D11_VIEWPORT vp = {}; vp.Width = static_cast<float>(bbd.Width); vp.Height = static_cast<float>(bbd.Height); vp.MaxDepth = 1.0f;
    const float bf[4] = {0, 0, 0, 0};
    g_gameContext->OMSetRenderTargets(1, &outRtv, nullptr);
    g_gameContext->RSSetViewports(1, &vp);
    g_gameContext->RSSetState(g_depthVizRaster);
    g_gameContext->OMSetDepthStencilState(g_depthVizDepthState, 0);
    g_gameContext->OMSetBlendState(g_depthVizBlend, bf, 0xffffffff);
    g_gameContext->IASetInputLayout(nullptr);
    g_gameContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_gameContext->VSSetShader(g_depthVizVs, nullptr, 0);
    g_gameContext->PSSetShader(g_depthVizPs, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = { g_dibrColorSrv, g_depthSrv };
    g_gameContext->PSSetShaderResources(0, 2, srvs);
    g_gameContext->PSSetSamplers(0, 1, &g_depthVizSampler);
    if (g_dibrParamsCb != nullptr)   // push the live warp tunables from the menu
    {
        D3D11_MAPPED_SUBRESOURCE mp = {};
        if (SUCCEEDED(g_gameContext->Map(g_dibrParamsCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp)))
        {
            float* p = static_cast<float*>(mp.pData);
            FillDibrParamBuffer(p, eyeScale);
            g_gameContext->Unmap(g_dibrParamsCb, 0);
        }
        g_gameContext->PSSetConstantBuffers(0, 1, &g_dibrParamsCb);
    }
    g_gameContext->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[2] = {}; g_gameContext->PSSetShaderResources(0, 2, nullSrv);
    g_gameContext->OMSetRenderTargets(8, oldRtv, oldDsv);
    g_gameContext->RSSetViewports(oldVpN, oldVp);
    g_gameContext->RSSetState(oldRs);
    g_gameContext->OMSetDepthStencilState(oldDs, oldRef);
    g_gameContext->OMSetBlendState(oldBlend, oldBlendFactor, oldSampleMask);
    g_gameContext->IASetPrimitiveTopology(oldTopo);
    g_gameContext->IASetInputLayout(oldIl);
    g_gameContext->VSSetShader(oldVs, nullptr, 0);
    g_gameContext->PSSetShader(oldPs, nullptr, 0);
    g_gameContext->PSSetShaderResources(0, 2, oldSrv);
    g_gameContext->PSSetSamplers(0, 1, &oldSamp);
    for (auto*& r : oldRtv) SafeRelease(r);
    SafeRelease(oldDsv); SafeRelease(oldRs); SafeRelease(oldDs); SafeRelease(oldBlend); SafeRelease(oldIl);
    SafeRelease(oldVs); SafeRelease(oldPs); SafeRelease(oldSrv[0]); SafeRelease(oldSrv[1]); SafeRelease(oldSamp);
    return outTex;
}

// [RELIEF_PERF] Sample the warp SOURCE directly instead of CopyResource-ing it into g_dibrColorCopy first.
// The SFR pass textures are created RENDER_TARGET|SHADER_RESOURCE (the UI-mirror work), so an SRV on them
// succeeds and saves a full-frame copy PER EYE PER PRESENT (2 x 151MB at 6144 - a measured chunk of the
// depth-pop frame cost). The game's swapchain backbuffer usually has no SHADER_RESOURCE bind -> SRV create
// fails once, the failure is remembered, and that texture uses the legacy copy path. Tiny AddRef'd LRU so
// SFR's alternating pass textures each keep their SRV; stale entries (res change) round-robin out.
namespace {
struct ReliefSrcSrv { ID3D11Texture2D* tex; ID3D11ShaderResourceView* srv; bool failed; };
ReliefSrcSrv g_reliefSrcSrvCache[4] = {};
int g_reliefSrcSrvNext = 0;

ID3D11ShaderResourceView* GetReliefSourceSrv(ID3D11Texture2D* tex) noexcept
{
    for (const auto& e : g_reliefSrcSrvCache)
    {
        if (e.tex == tex) return e.failed ? nullptr : e.srv;
    }
    ID3D11ShaderResourceView* srv = nullptr;
    const HRESULT hr = g_gameDevice->CreateShaderResourceView(tex, nullptr, &srv);
    ReliefSrcSrv& slot = g_reliefSrcSrvCache[g_reliefSrcSrvNext];
    g_reliefSrcSrvNext = (g_reliefSrcSrvNext + 1) & 3;
    if (slot.srv != nullptr) slot.srv->Release();
    if (slot.tex != nullptr) slot.tex->Release();
    slot.failed = FAILED(hr) || srv == nullptr;
    slot.srv = slot.failed ? nullptr : srv;
    slot.tex = tex;
    tex->AddRef();
    static std::atomic<int> s_srvLogs{0};
    if (s_srvLogs.fetch_add(1, std::memory_order_relaxed) < 4)
    {
        LogLine(std::string("[RELIEF_PERF] warp source ") +
                (slot.failed ? "has no SRV bind -> fallback full-frame copy" : "sampled DIRECT (input copy skipped)"));
    }
    return slot.srv;
}
}  // namespace

// [RELIEF M1] Render the backbuffer with the depth-driven pop into g_reliefTex. mode 0 = SBS (stereo, two
// halves warp opposite, clamped per-half); mode 1 = full frame (AER, whole frame is one eye = eyeSign).
// Returns g_reliefTex or nullptr if not ready / strength ~0. Present-thread only.
ID3D11Texture2D* RenderReliefPass(ID3D11Texture2D* backBuffer, float mode, float eyeSign) noexcept
{
    if (g_gameContext == nullptr || g_gameDevice == nullptr || backBuffer == nullptr) return nullptr;
    // Off = caller uses raw bb. [DEPTH DARKEN]/[UNSHARP POP] keep the pass alive at base pop strength 0
    // so either can run alone (they ride this same fullscreen pass).
    if (g_reliefStrength.load(std::memory_order_relaxed) <= 0.0001f &&
        g_reliefDarkStrength.load(std::memory_order_relaxed) <= 0.0001f &&
        g_reliefUnsharpStrength.load(std::memory_order_relaxed) <= 0.0001f) return nullptr;
    if (!g_depthReady.load(std::memory_order_acquire) || g_depthSrv == nullptr) return nullptr;
    if (!EnsureDepthVizShaders() || g_reliefPs == nullptr) return nullptr;
    D3D11_TEXTURE2D_DESC bbd = {}; backBuffer->GetDesc(&bbd);
    if (!EnsureReliefTex(bbd)) return nullptr;
    // [RELIEF_PERF] direct SRV when the source supports it (SFR pass textures); copy fallback otherwise.
    ID3D11ShaderResourceView* srcSrv = GetReliefSourceSrv(backBuffer);
    if (srcSrv == nullptr)
    {
        if (!EnsureColorCopy(bbd)) return nullptr;
        g_gameContext->CopyResource(g_dibrColorCopy, backBuffer);   // sampleable color
        srcSrv = g_dibrColorSrv;
    }

    // save state that gets touched
    ID3D11RenderTargetView* oldRtv[8] = {}; ID3D11DepthStencilView* oldDsv = nullptr;
    g_gameContext->OMGetRenderTargets(8, oldRtv, &oldDsv);
    D3D11_VIEWPORT oldVp[16] = {}; UINT oldVpN = 16; g_gameContext->RSGetViewports(&oldVpN, oldVp);
    ID3D11RasterizerState* oldRs = nullptr; g_gameContext->RSGetState(&oldRs);
    ID3D11DepthStencilState* oldDs = nullptr; UINT oldRef = 0; g_gameContext->OMGetDepthStencilState(&oldDs, &oldRef);
    float oldBf[4] = {}; UINT oldMask = 0xffffffff; ID3D11BlendState* oldBlend = nullptr;
    g_gameContext->OMGetBlendState(&oldBlend, oldBf, &oldMask);
    D3D11_PRIMITIVE_TOPOLOGY oldTopo; g_gameContext->IAGetPrimitiveTopology(&oldTopo);
    ID3D11InputLayout* oldIl = nullptr; g_gameContext->IAGetInputLayout(&oldIl);
    ID3D11VertexShader* oldVs = nullptr; g_gameContext->VSGetShader(&oldVs, nullptr, nullptr);
    ID3D11PixelShader* oldPs = nullptr; g_gameContext->PSGetShader(&oldPs, nullptr, nullptr);
    ID3D11ShaderResourceView* oldSrv[2] = {}; g_gameContext->PSGetShaderResources(0, 2, oldSrv);
    ID3D11SamplerState* oldSamp = nullptr; g_gameContext->PSGetSamplers(0, 1, &oldSamp);
    ID3D11Buffer* oldCb = nullptr; g_gameContext->PSGetConstantBuffers(0, 1, &oldCb);

    D3D11_VIEWPORT vp = {}; vp.Width = static_cast<float>(bbd.Width); vp.Height = static_cast<float>(bbd.Height); vp.MaxDepth = 1.0f;
    const float bf[4] = {0, 0, 0, 0};
    g_gameContext->OMSetRenderTargets(1, &g_reliefRtv, nullptr);
    g_gameContext->RSSetViewports(1, &vp);
    g_gameContext->RSSetState(g_depthVizRaster);
    g_gameContext->OMSetDepthStencilState(g_depthVizDepthState, 0);
    g_gameContext->OMSetBlendState(g_depthVizBlend, bf, 0xffffffff);
    g_gameContext->IASetInputLayout(nullptr);
    g_gameContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_gameContext->VSSetShader(g_depthVizVs, nullptr, 0);
    g_gameContext->PSSetShader(g_reliefPs, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = { srcSrv, g_depthSrv };
    g_gameContext->PSSetShaderResources(0, 2, srvs);
    g_gameContext->PSSetSamplers(0, 1, &g_depthVizSampler);
    if (g_reliefParamsCb != nullptr)
    {
        D3D11_MAPPED_SUBRESOURCE mp = {};
        if (SUCCEEDED(g_gameContext->Map(g_reliefParamsCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mp)))
        {
            float* p = static_cast<float*>(mp.pData);
            p[0] = g_reliefStrength.load(std::memory_order_relaxed);
            p[1] = g_reliefConvergence.load(std::memory_order_relaxed);
            p[2] = g_reliefSign.load(std::memory_order_relaxed);
            p[3] = g_reliefCurve.load(std::memory_order_relaxed);
            p[4] = g_reliefEdge.load(std::memory_order_relaxed);
            p[5] = g_reliefNearFreeze.load(std::memory_order_relaxed);
            p[6] = mode;
            p[7] = eyeSign;
            p[8] = g_reliefDarkStrength.load(std::memory_order_relaxed);
            p[9] = g_reliefDarkRadius.load(std::memory_order_relaxed);
            p[10] = bbd.Width  > 0 ? 1.0f / static_cast<float>(bbd.Width)  : 0.0f;
            p[11] = bbd.Height > 0 ? 1.0f / static_cast<float>(bbd.Height) : 0.0f;
            p[12] = g_reliefUnsharpStrength.load(std::memory_order_relaxed);
            p[13] = g_reliefUnsharpRadius.load(std::memory_order_relaxed);
            p[14] = 0.0f;
            p[15] = 0.0f;
            g_gameContext->Unmap(g_reliefParamsCb, 0);
        }
        g_gameContext->PSSetConstantBuffers(0, 1, &g_reliefParamsCb);
    }
    g_gameContext->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[2] = {}; g_gameContext->PSSetShaderResources(0, 2, nullSrv);
    g_gameContext->OMSetRenderTargets(8, oldRtv, oldDsv);
    g_gameContext->RSSetViewports(oldVpN, oldVp);
    g_gameContext->RSSetState(oldRs);
    g_gameContext->OMSetDepthStencilState(oldDs, oldRef);
    g_gameContext->OMSetBlendState(oldBlend, oldBf, oldMask);
    g_gameContext->IASetPrimitiveTopology(oldTopo);
    g_gameContext->IASetInputLayout(oldIl);
    g_gameContext->VSSetShader(oldVs, nullptr, 0);
    g_gameContext->PSSetShader(oldPs, nullptr, 0);
    g_gameContext->PSSetShaderResources(0, 2, oldSrv);
    g_gameContext->PSSetSamplers(0, 1, &oldSamp);
    g_gameContext->PSSetConstantBuffers(0, 1, &oldCb);
    for (auto*& r : oldRtv) SafeRelease(r);
    SafeRelease(oldDsv); SafeRelease(oldRs); SafeRelease(oldDs); SafeRelease(oldBlend); SafeRelease(oldIl);
    SafeRelease(oldVs); SafeRelease(oldPs); SafeRelease(oldSrv[0]); SafeRelease(oldSrv[1]); SafeRelease(oldSamp); SafeRelease(oldCb);
    return g_reliefTex;
}

// [RELIEF M1] stereo: warp the SBS backbuffer (per-half). CopySbsHalves reads the result into the eyes.
ID3D11Texture2D* RenderReliefSbs(ID3D11Texture2D* backBuffer) noexcept
{
    return RenderReliefPass(backBuffer, 0.0f, 1.0f);
}

// [RELIEF M1.3] AER: warp a full-frame eye (whole frame = one eye, eyeSign = +/-1). The caller
// CopyResource's the result into that eye's AER history slot.
ID3D11Texture2D* RenderReliefFull(ID3D11Texture2D* backBuffer, float eyeSign) noexcept
{
    return RenderReliefPass(backBuffer, 1.0f, eyeSign);
}

void SetReliefWarp(float strength, float convergence, float sign, float curve, float edgeGuard, float nearFreeze,
                   float darkStrength, float darkRadius, float unsharpStrength, float unsharpRadius) noexcept
{
    auto cl = [](float v, float lo, float hi){ return v < lo ? lo : (v > hi ? hi : v); };
    g_reliefStrength.store(cl(strength, 0.0f, 0.2f), std::memory_order_relaxed);
    g_reliefConvergence.store(cl(convergence, 0.90f, 1.005f), std::memory_order_relaxed);
    g_reliefSign.store(sign < 0.0f ? -1.0f : 1.0f, std::memory_order_relaxed);
    g_reliefCurve.store(cl(curve, 0.05f, 4.0f), std::memory_order_relaxed);
    g_reliefEdge.store(cl(edgeGuard, 0.0f, 1.0f), std::memory_order_relaxed);
    g_reliefNearFreeze.store(cl(nearFreeze, 0.90f, 1.02f), std::memory_order_relaxed);
    g_reliefDarkStrength.store(cl(darkStrength, 0.0f, 1.0f), std::memory_order_relaxed);
    g_reliefDarkRadius.store(cl(darkRadius, 1.0f, 32.0f), std::memory_order_relaxed);
    g_reliefUnsharpStrength.store(cl(unsharpStrength, 0.0f, 0.2f), std::memory_order_relaxed);
    g_reliefUnsharpRadius.store(cl(unsharpRadius, 4.0f, 96.0f), std::memory_order_relaxed);
}

// [RELIEF] greyscale viz: auto-fit on/off. When off, the manual near/far/gamma window (SetDepthMapTuning)
// is honored so you can park the window and read where the subject sits vs the environment.
void SetDepthVizAutoFit(bool on) noexcept { g_depthVizAutoFit.store(on, std::memory_order_relaxed); }
bool GetDepthVizAutoFit() noexcept { return g_depthVizAutoFit.load(std::memory_order_relaxed); }

bool IsReliefDepthReady() noexcept
{
    return g_depthReady.load(std::memory_order_acquire) && g_depthSrv != nullptr;
}

// Render the full-frame DIBR warp (synthesized right eye) from the finished color frame + captured depth,
// returning a texture matching the backbuffer (so xr_session CopyResource's it into the right eye exactly like
// the game frame goes to the left). Runs on the present thread during the eye submit. nullptr if not ready.
ID3D11Texture2D* GetDibrRightEye(ID3D11Texture2D* backBuffer) noexcept
{
    // DIBR v2 M0 (2026-07-07): restored the CLEAN GUARD-GATHER warp - the "minimal
    // artifacts" build. The 06-23 "ghost fix" swapped in
    // the grid-SCATTER warp, which throws foreground into disocclusions = the silhouette ghosting. The 06-26
    // gather-restore attempt crashed because it wired the WRONG function (RenderFarBgGatherEye, untested);
    // RenderDibrEye is the proven path (was live for a week, still exercised by the left-framing branch).
    if (g_gameContext == nullptr || g_gameDevice == nullptr || backBuffer == nullptr) return nullptr;
    D3D11_TEXTURE2D_DESC bbd = {}; backBuffer->GetDesc(&bbd);
    if (!EnsureColorCopy(bbd) || !EnsureWarpedTex(bbd)) return nullptr;
    return RenderDibrEye(backBuffer, g_dibrWarpedTex, g_dibrWarpedRtv, 1.0f);
}

ID3D11Texture2D* GetDibrLeftEye(ID3D11Texture2D* backBuffer) noexcept
{
    if (std::fabs(g_dibrSourceScale.load(std::memory_order_relaxed) - 1.0f) < 0.001f) return nullptr;
    if (g_gameContext == nullptr || g_gameDevice == nullptr || backBuffer == nullptr) return nullptr;
    D3D11_TEXTURE2D_DESC bbd = {}; backBuffer->GetDesc(&bbd);
    if (!EnsureColorCopy(bbd) || !EnsureLeftDibrTex(bbd)) return nullptr;
    return RenderDibrEye(backBuffer, g_dibrLeftTex, g_dibrLeftRtv, 0.0f);
}

bool ReflLogArmed() noexcept
{
    return g_reflLogArmed.load(std::memory_order_relaxed);
}

void SetReflFixInputs(float yawRad, float pitchRad, float camX, float camY, float camZ) noexcept
{
    g_reflFixYawRad.store(yawRad, std::memory_order_relaxed);
    g_reflFixPitchRad.store(pitchRad, std::memory_order_relaxed);
    g_reflFixCamX.store(camX, std::memory_order_relaxed);
    g_reflFixCamY.store(camY, std::memory_order_relaxed);
    g_reflFixCamZ.store(camZ, std::memory_order_relaxed);
    g_reflFixHaveInputs.store(true, std::memory_order_release);
}

void SetSfrCineBoundary(bool on) noexcept
{
    // [CLEANVRCINE] reset the diag throttle on each convo/cutscene entry so every scene logs its first presents
    if (on && !g_sfrCineBoundary.load(std::memory_order_relaxed))
        g_sfrCineDiagLogs.store(0, std::memory_order_relaxed);
    g_sfrCineBoundary.store(on, std::memory_order_relaxed);
}

void NotifySfrReplayEnqueued() noexcept
{
    // Game thread: a replay was just enqueued; keep the render-side capture machine armed for the
    // next few presents (renewed every replay frame, so it disarms itself soon after SFR stops).
    g_sfrCapturePresentsLeft.store(4, std::memory_order_release);
}

bool PreallocateStereoPassTextures() noexcept
{
    if (g_gameSwapChain == nullptr) return false;
    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(g_gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) ||
        backBuffer == nullptr)
    {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    backBuffer->GetDesc(&desc);
    backBuffer->Release();
    std::lock_guard<std::mutex> lock(g_stereoPassMutex);
    return EnsureStereoPassTextures(desc);
}

bool CaptureStereoPass(int pass) noexcept
{
    if (pass < 0 || pass > 1 || g_gameSwapChain == nullptr || g_gameContext == nullptr)
    {
        return false;
    }

    ID3D11Texture2D* backBuffer = nullptr;
    if (FAILED(g_gameSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer))) ||
        backBuffer == nullptr)
    {
        LogLine("[PASSCAP] GetBuffer FAILED for pass=" + std::to_string(pass));
        return false;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    backBuffer->GetDesc(&desc);
    std::lock_guard<std::mutex> lock(g_stereoPassMutex);
    const bool ok = EnsureStereoPassTextures(desc);
    if (ok && g_stereoPassTex[g_stereoWriteBank][pass] != nullptr)
    {
        g_gameContext->CopyResource(g_stereoPassTex[g_stereoWriteBank][pass], backBuffer);
        const unsigned long long n = g_stereoPassCopies[pass].fetch_add(1, std::memory_order_acq_rel) + 1;
        if (n <= 16 || (n % 120) == 1)
        {
            LogLine("[PASSCAP] pass=" + std::to_string(pass) +
                    " copied count=" + std::to_string(n) +
                    " bank=" + std::to_string(g_stereoWriteBank) +
                    " src=" + std::to_string(desc.Width) + "x" + std::to_string(desc.Height) +
                    " fmt=" + std::to_string(static_cast<int>(desc.Format)));
        }
        if (pass == 0)
        {
            // Both SFR passes reuse one main-pass head-look latch. Stamp it when pass 0's pixels
            // enter this bank, then publish that metadata only when pass 1 completes the pair.
            g_stereoPairArm[g_stereoWriteBank] =
                static_cast<unsigned long long>(MELEVR::RenderHook::GetPairArmSeq());
            g_stereoWriteHasPass0 = true;
        }
        else if (g_stereoWriteHasPass0)
        {
            g_stereoPublishedBank = g_stereoWriteBank;
            ++g_stereoPublishedPairCount;
            if (g_stereoPublishedPairCount <= 16 || (g_stereoPublishedPairCount % 120) == 1)
            {
                LogLine("[PASSCAP] published complete stereo pair count=" +
                        std::to_string(g_stereoPublishedPairCount) +
                        " bank=" + std::to_string(g_stereoPublishedBank));
            }
            g_stereoWriteBank = (g_stereoWriteBank + 1) % kStereoPassBanks;
            if (g_stereoWriteBank == g_stereoPublishedBank)
            {
                g_stereoWriteBank = (g_stereoWriteBank + 1) % kStereoPassBanks;
            }
            g_stereoWriteHasPass0 = false;
        }
        else if (pass == 1)
        {
            LogLine("[PASSCAP] pass=1 copied without pass0; pair not published.");
        }
    }

    SafeRelease(backBuffer);
    return ok;
}

bool GetStereoPassTexture(int pass, ID3D11Texture2D** outTexture, unsigned long long* outCopyCount) noexcept
{
    if (outTexture == nullptr || pass < 0 || pass > 1)
    {
        return false;
    }
    *outTexture = nullptr;

    std::lock_guard<std::mutex> lock(g_stereoPassMutex);
    const int bank = g_stereoPublishedBank;
    const unsigned long long count = g_stereoPublishedPairCount;
    if (outCopyCount != nullptr)
    {
        *outCopyCount = count;
    }
    if (bank < 0 || bank >= kStereoPassBanks || count == 0)
    {
        return false;
    }

    ID3D11Texture2D* tex = g_stereoPassTex[bank][pass];
    if (tex == nullptr)
    {
        return false;
    }

    tex->AddRef();
    *outTexture = tex;
    return true;
}

bool GetStereoPassPair(ID3D11Texture2D** outLeft, ID3D11Texture2D** outRight,
                       unsigned long long* outPairCount,
                       unsigned long long* outPairArm) noexcept
{
    if (outLeft == nullptr || outRight == nullptr)
    {
        return false;
    }
    *outLeft = nullptr;
    *outRight = nullptr;

    std::lock_guard<std::mutex> lock(g_stereoPassMutex);
    const int bank = g_stereoPublishedBank;
    const unsigned long long count = g_stereoPublishedPairCount;
    if (outPairCount != nullptr)
    {
        *outPairCount = count;
    }
    if (outPairArm != nullptr)
    {
        *outPairArm = (bank >= 0 && bank < kStereoPassBanks) ? g_stereoPairArm[bank] : 0;
    }
    if (bank < 0 || bank >= kStereoPassBanks || count == 0)
    {
        return false;
    }

    ID3D11Texture2D* left = g_stereoPassTex[bank][0];
    ID3D11Texture2D* right = g_stereoPassTex[bank][1];
    if (left == nullptr || right == nullptr)
    {
        return false;
    }

    left->AddRef();
    right->AddRef();
    *outLeft = left;
    *outRight = right;
    return true;
}

bool SaveReplayD3DState() noexcept
{
    std::lock_guard<std::mutex> lock(g_replayStateMutex);
    const bool ok = SaveContextState(g_replayState);
    static std::atomic<int> s_logs{0};
    const int n = s_logs.fetch_add(1, std::memory_order_relaxed);
    if (n < 8)
    {
        LogLine(std::string("[FVCREPLAY] D3D state save ") + (ok ? "ok" : "FAILED"));
    }
    return ok;
}

void RestoreReplayD3DState() noexcept
{
    std::lock_guard<std::mutex> lock(g_replayStateMutex);
    RestoreContextState(g_replayState);
    static std::atomic<int> s_logs{0};
    const int n = s_logs.fetch_add(1, std::memory_order_relaxed);
    if (n < 8)
    {
        LogLine("[FVCREPLAY] D3D state restored after replay.");
    }
}

void LogDeviceHealth(const char* label) noexcept
{
    if (g_gameDevice == nullptr) return;
    const HRESULT hr = g_gameDevice->GetDeviceRemovedReason();
    if (hr != S_OK)
    {
        LogLine(std::string("[D3DHEALTH] ") + (label != nullptr ? label : "unknown") +
                " device removed reason=" + HexHRESULT(hr));
    }
}

void LogVideoMemory(const char* label, bool force) noexcept
{
    if (g_gameDevice == nullptr) return;
    const ULONGLONG now = GetTickCount64();
    if (!force && g_lastVramLogMs != 0 && now - g_lastVramLogMs < 1000)
    {
        return;
    }
    g_lastVramLogMs = now;

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIAdapter3* adapter3 = nullptr;
    if (FAILED(g_gameDevice->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice))) ||
        dxgiDevice == nullptr)
    {
        return;
    }
    if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) && adapter != nullptr &&
        SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&adapter3))) &&
        adapter3 != nullptr)
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
        if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        {
            const unsigned long long mb = static_cast<unsigned long long>(info.CurrentUsage / (1024ull * 1024ull));
            const unsigned long long budgetMb = static_cast<unsigned long long>(info.Budget / (1024ull * 1024ull));
            const unsigned long long availMb = static_cast<unsigned long long>(info.AvailableForReservation / (1024ull * 1024ull));
            LogLine(std::string("[VRAM] ") + (label != nullptr ? label : "unknown") +
                    " currentMB=" + std::to_string(mb) +
                    " budgetMB=" + std::to_string(budgetMb) +
                    " availableReserveMB=" + std::to_string(availMb));
        }
    }
    SafeRelease(adapter3);
    SafeRelease(adapter);
    SafeRelease(dxgiDevice);
}

std::atomic_bool g_factoryCaptureSuppressed{false};

void SetFactoryCaptureSuppressed(bool suppressed) noexcept
{
    g_factoryCaptureSuppressed.store(suppressed, std::memory_order_release);
}

void TryInstallFactoryHooks(void* factory, REFIID requestedFactoryId) noexcept
{
    UNREFERENCED_PARAMETER(requestedFactoryId);
    if (factory == nullptr)
    {
        return;
    }
    // [SVRDIAG2] this diagnostic D3D11CreateDevice pulls an INTERNAL CreateDXGIFactory2 inside DXGI;
    // capturing/hooking that internal factory re-entrantly from the game's CreateSwapChain callstack
    // crashed at boot (log-proven 2026-07-20: last lines = SVRDIAG2 create -> Factory2 captured -> dead).
    // The mod's own device probes are never the game's render factory - skip them entirely.
    if (g_factoryCaptureSuppressed.load(std::memory_order_acquire))
    {
        LogLine("[DXGIHOOK] factory capture suppressed (mod-internal device probe).");
        return;
    }

    std::lock_guard<std::mutex> lock(g_hookMutex);

    // [DISPQ2] IDXGIOutput vtable. Walk factory -> adapter 0 -> output 0 just to reach the vtable; the vtable
    // is shared by every IDXGIOutput instance, so patching it once covers whichever output the game queries.
    // IDXGIObject adds 4 slots after IUnknown's 3, so IDXGIOutput starts at 7.
    {
        IDXGIFactory* fac = reinterpret_cast<IDXGIFactory*>(factory);
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(fac->EnumAdapters(0, &adapter)) && adapter != nullptr)
        {
            IDXGIOutput* output = nullptr;
            if (SUCCEEDED(adapter->EnumOutputs(0, &output)) && output != nullptr)
            {
                void** ovt = *reinterpret_cast<void***>(output);
                HookSlot(ovt, 7, reinterpret_cast<void*>(&OutputGetDescHook),
                         reinterpret_cast<void**>(&g_origOutputGetDesc), &g_outputGetDescSlot,
                         "IDXGIOutput::GetDesc");
                HookSlot(ovt, 8, reinterpret_cast<void*>(&GetDisplayModeListHook),
                         reinterpret_cast<void**>(&g_origGetDisplayModeList), &g_getDisplayModeListSlot,
                         "IDXGIOutput::GetDisplayModeList");
                HookSlot(ovt, 9, reinterpret_cast<void*>(&FindClosestMatchingModeHook),
                         reinterpret_cast<void**>(&g_origFindClosestMatchingMode), &g_findClosestModeSlot,
                         "IDXGIOutput::FindClosestMatchingMode");
                output->Release();
            }
            adapter->Release();
        }
    }

    // Legacy IDXGIFactory::CreateSwapChain (slot 10).
    void** vtable = *reinterpret_cast<void***>(factory);
    HookSlot(vtable, kCreateSwapChainVTableIndex, reinterpret_cast<void*>(&CreateSwapChainHook),
             reinterpret_cast<void**>(&g_originalCreateSwapChain), &g_createSwapChainSlot,
             "IDXGIFactory::CreateSwapChain");

    // Modern IDXGIFactory2 entry points (LE1 uses CreateSwapChainForHwnd).
    IDXGIFactory2* factory2 = nullptr;
    if (SUCCEEDED(reinterpret_cast<IUnknown*>(factory)->QueryInterface(__uuidof(IDXGIFactory2),
                                                                       reinterpret_cast<void**>(&factory2))) &&
        factory2 != nullptr)
    {
        void** vtable2 = *reinterpret_cast<void***>(factory2);
        HookSlot(vtable2, kCreateSwapChainForHwndVTableIndex, reinterpret_cast<void*>(&CreateSwapChainForHwndHook),
                 reinterpret_cast<void**>(&g_originalCreateSwapChainForHwnd), &g_createSwapChainForHwndSlot,
                 "IDXGIFactory2::CreateSwapChainForHwnd");
        HookSlot(vtable2, kCreateSwapChainForCoreWindowVTableIndex,
                 reinterpret_cast<void*>(&CreateSwapChainForCoreWindowHook),
                 reinterpret_cast<void**>(&g_originalCreateSwapChainForCoreWindow),
                 &g_createSwapChainForCoreWindowSlot, "IDXGIFactory2::CreateSwapChainForCoreWindow");
        HookSlot(vtable2, kCreateSwapChainForCompositionVTableIndex,
                 reinterpret_cast<void*>(&CreateSwapChainForCompositionHook),
                 reinterpret_cast<void**>(&g_originalCreateSwapChainForComposition),
                 &g_createSwapChainForCompositionSlot, "IDXGIFactory2::CreateSwapChainForComposition");
        factory2->Release();
    }
}
}
