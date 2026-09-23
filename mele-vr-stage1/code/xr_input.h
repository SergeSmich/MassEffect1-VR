#pragma once

// ============================================================================
// MELE VR - Stage 1: VR controller input (DRAFT, 2026-09-23).
//
// Owns the OpenXR input side of the mod: one action set, a full "gamepad"
// mirror of both hands (poses, triggers, grips, sticks, face, dpad), per-hand
// action spaces located against the app space (same space as the head pose),
// and per-frame sync/locate/update on the game's render thread.
//
// Two consumers:
//   1. Aim source  - GetAimDeg(): smoothed yaw/pitch of the right controller's
//      pointing ray, in the HeadEulerDegrees convention (forward = R*(0,0,-1)).
//      xr_session.cpp swaps it in for the head angles at the two existing
//      DriveAimWithHead() call sites (on-foot + Mako). NO new game-object write
//      path is introduced.
//   2. Virtual pad - BuildVirtualGamepad(): synthesizes an XINPUT_STATE from
//      the controller frame. vr_menu.cpp's XInputGetState(Ex) hook fills it
//      when cfg.controllerInput is on and a controller frame is valid; the
//      game then "sees" a plugged-in gamepad.
//
// Threading: everything here runs on the game render thread (the same thread
// that drives OnPresent / LocateHead). g_frame is plain (no atomics/locks) -
// single-writer, single-reader by construction.
//
// Fail-safe: ANY error (missing loader function, failed action create, lost
// tracking) makes the frame invalid -> callers fall back to the current
// behaviour (real-pad passthrough + head aim). The feature can never be
// "half on".
//
// ABI: no OpenXR SDK headers in this project - all types/constants are
// hand-written in xr_types.h (values pinned 2026-09-23, see
// docs/STAGE1_CONTROLLER_DESIGN.md section 9). Init() logs which loader
// generation (1.0 vs 0.9 action API) was resolved - the first run decides
// which compile path (MELEVR_XRINPUT_09) is final.
// ============================================================================

#include <cstdint>

#include "xr_types.h"

#include <Xinput.h>   // XINPUT_STATE / XINPUT_GAMEPAD types only - NO xinput.lib linkage
                      // (the project resolves XInput dynamically, as vr_menu.cpp does)

namespace MELEVR::Config { struct VrConfig; }   // fwd (see vr_config.h)

namespace MELEVR::XrInput
{
using namespace MELEVR::Xr;   // XrInstance/XrSession/XrSpace/XrTime/XrQuaternionf/...

// ---- published per-frame snapshot ------------------------------------------
struct HandFrame
{
    bool poseValid = false;
    XrQuaternionf poseOrientation = {};   // in app space (same basis as head pose)
    XrVector3f    posePosition = {};      // in app space (unused by Stage 1, kept for Stage 2)
    float trigger = 0.0f;                 // 0..1
    float squeeze = 0.0f;                 // 0..1
    float stickX = 0.0f;                  // -1..1
    float stickY = 0.0f;                  // -1..1
    bool grip = false;
    bool stickClick = false;
    bool a = false, b = false, x = false, y = false;
    bool dpadUp = false, dpadDown = false, dpadLeft = false, dpadRight = false;
};

struct Frame
{
    bool valid = false;        // at least one hand tracked this frame
    bool rightConnected = false;
    bool leftConnected = false;
    HandFrame right;
    HandFrame left;
    bool aimValid = false;     // smoothed aim angles ready (right pose + controllerAim on)
    float aimYawDeg = 0.0f;
    float aimPitchDeg = 0.0f;
};

// One-time, render thread, AFTER xrCreateSession (actions need the session for
// action spaces; instance-level parts are created immediately, session-bound
// parts are (re)created lazily in OnFrame on session restarts).
// getProc: the module's xrGetInstanceProcAddr trampoline (passed in because the
// session's loader handle is file-static in xr_session.cpp).
// Returns true when the action API resolved and the action set/actions were
// created (a later tracking failure only invalidates frames, never this).
bool Init(XrInstance instance, XrSession session, PFN_xrGetInstanceProcAddr getProc) noexcept;

// Called from the session state-changed handler when the session leaves RUNNING
// (STOPPING branch, right after xrEndSession). Session-bound objects (action
// spaces, attach) die with the session - drop them so OnFrame re-creates them
// for the next RUNNING session instead of using stale handles.
void SessionInvalidated() noexcept;

void Shutdown() noexcept;

// True when the module is ready (functions resolved + action set created).
bool IsReady() noexcept;

// Per frame, render thread, same place the head is located. Call with the
// app-space handle (g_appSpace in xr_session.cpp) and the smoothed head
// quaternion used for the submit tag - the head is needed for the aim
// head-blend (cfg.controllerAimHeadBlend) and only when controllerAim is on.
void OnFrame(XrSpace appSpace, XrTime displayTime, const XrQuaternionf& headQuat) noexcept;

// Current published frame (always non-const-valid; check .valid).
const Frame& GetFrame() noexcept;

// Smoothed aim yaw/pitch (degrees), HeadEulerDegrees convention. False when the
// right hand pose is not valid this frame (caller keeps the head aim source).
bool GetAimDeg(float* yawDeg, float* pitchDeg) noexcept;

// True while controller aim is actually driving this frame (aim valid AND
// cfg.controllerAim on) - used by xr_session.cpp to decide render-side look.
bool AimActive() noexcept;

// Fill *state (gamepad fields + dwFlags) from the current frame using cfg
// (deadzones, right-stick policy). Returns true when a controller frame was
// used - the caller MUST then return ERROR_SUCCESS from the XInput hook even
// if no physical pad is connected (the game sees a plugged-in pad).
// dwPacketNumber is left as-is; the caller increments it.
bool BuildVirtualGamepad(XINPUT_STATE* state, const Config::VrConfig& cfg) noexcept;

// 2 Hz log of the REAL XInput state (mapping discovery, cfg.controllerLogRealPad).
// Called from the hook BEFORE any synthesis; no-op when disabled.
void LogRealPadThrottled(const XINPUT_STATE* realState, DWORD result) noexcept;
}
