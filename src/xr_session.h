#pragma once

#include <cstdint>

struct ID3D11Device;
struct IDXGISwapChain;

// ============================================================================
// MELE VR - CLEAN REBUILD, Milestone 0: stable flat-mono OpenXR output.
//
// This is a from-scratch foundation. It does EXACTLY one thing: take the game's rendered backbuffer and put
// it on a head-locked flat panel in the headset, in both eyes (mono). NOTHING else - no stereo, no AER, no
// head tracking, no camera writes, no comfort, no FOV work, no replay, no menu. Features are added back one
// milestone at a time as fresh, isolated code, so nothing can interfere by construction.
//
// The pipe (dxgi proxy + d3d_capture swapchain/Present hook) is the proven plumbing; this session does the
// OpenXR side. Driven from the game's Present hook on the game's render thread (so the runtime composites with
// the same single-threaded D3D11 context the game uses).
// ============================================================================

namespace MELEVR::XrSession
{
struct ResolutionStats
{
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t sourceWidth = 0;
    uint32_t sourceHeight = 0;
    uint32_t eyeWidth = 0;
    uint32_t eyeHeight = 0;
    uint32_t recommendedEyeWidth = 0;
    uint32_t recommendedEyeHeight = 0;
};

// Called once from d3d_capture with the game's device + its backbuffer format and size.
void TryCreateSession(ID3D11Device* device, int64_t colorFormat, uint32_t width, uint32_t height) noexcept;

// Called every Present (game render thread): drive the OpenXR frame loop + copy the backbuffer to the eye.
void OnPresent(IDXGISwapChain* gameSwapChain) noexcept;

// No-headset FLAT path (OpenXR disabled via MELEVR_DISABLE_OPENXR marker): RunFrame never runs, so this
// drives just the Scaleform HUD + subtitle controls (PCHUD / conversation / subtitle render-mode) so they can
// be tuned and verified on the monitor. No XR, no submission.
void FlatHudTick() noexcept;

// Menu readout: current game/source texture size vs the actual submitted OpenXR eye texture.
bool GetResolutionStats(ResolutionStats* out) noexcept;
// True when the game's backbuffer is an HDR float format (Windows/game HDR is ON -> blue/doubled image).
bool IsHdrDetected() noexcept;
// [MIRRORTHROTTLE] True once the headset is being submitted to, i.e. the flat window is only a mirror.
bool IsHeadsetDriven() noexcept;

// DLL detach: signal the frame loop to stop.
void RequestShutdown() noexcept;
}
