// ============================================================================
// MELE VR - Stage 1: VR controller input (integrated from mele-vr-stage1 draft
// 2026-09-23, review-corrected 2026-09-23). See xr_input.h for the module
// contract. Render-thread only. Fail-safe on every OpenXR error: the
// published frame just goes invalid and the callers (XInput hook / aim
// source) fall back to the pre-Stage-1 behaviour.
//
// Design doc: docs/STAGE1_CONTROLLER_DESIGN.md (sections 4-6, 9, 11).
//
// REVIEW FIXES vs the draft (2026-09-23, new chat):
//  - A_COUNT: the action table has 28 entries (2 poses + 2x13), not 29 - the
//    draft's static_assert(==29) did not compile.
//  - Action-state API: OpenXR has xrSyncActions(session, XrActionsSyncInfo*)
//    and xrGetActionState{Boolean,Float,Vector2f}(session, ...) - there is no
//    "xrSyncInputs"/"xrUpdateActionState". The draft resolved non-existent
//    names and would have failed in Init(). The bundled openxr_loader.dll's
//    string table was checked: it carries exactly these names (1.0 generation,
//    no 0.9 aliases) - so no 0.9 fallback is needed.
//  - Resolve is now TOLERANT: every missing function is logged individually
//    and the decision comes after the full pass - the first run's [XRINPUT]
//    lines show the complete loader generation picture instead of stopping at
//    the first miss.
//  - EnsureSessionObjects failures are LATCHED per session (the draft re-logged
//    the same error every frame).
//  - Init() is idempotent (session re-init must not create a second action set).
//  - BuildVirtualGamepad zeroes the WHOLE XINPUT_STATE (the draft left
//    dwPacketNumber as-is, which is garbage when no physical pad is connected).
//  - On first tracked frame a one-shot [XRINPUT] line logs right/left validity
//    (M0/T1 diagnostics, before any consumer is wired in).
//
// DRAFT NOTES (still open, see design doc):
//  - Aim low-pass alpha is frame-rate dependent (config 0..1); dt-based in 1.1.
//  - Mapping of face buttons to ME1 abilities is a DEFAULT (1:1) pending the
//    controllerLogRealPad verification (design doc 11, T4).
// ============================================================================

#include "xr_input.h"

#include "logger.h"
#include "vr_config.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace MELEVR::XrInput
{
using namespace MELEVR::Xr;
using MELEVR::Logger::LogLine;

namespace
{
// ---- action table ------------------------------------------------------------
// One "gamepad" mirror per hand + a pose action per hand. Tracking goes through
// ACTION SPACES (not reference spaces): input spaces (reference space with
// targetLocation=INPUT) only exist in OpenXR 1.1+; action spaces are available
// in the 1.0 generation the bundled loader is. Design doc 4.2.
enum ActionId
{
    A_RIGHT_POSE = 0, A_LEFT_POSE,
    A_R_TRIG, A_R_SQZ, A_R_GRIP, A_R_STICK, A_R_STICKCLK,
    A_R_A, A_R_B, A_R_X, A_R_Y,
    A_R_DU, A_R_DD, A_R_DL, A_R_DR,
    A_L_TRIG, A_L_SQZ, A_L_GRIP, A_L_STICK, A_L_STICKCLK,
    A_L_A, A_L_B, A_L_X, A_L_Y,
    A_L_DU, A_L_DD, A_L_DL, A_L_DR,
    A_COUNT
};
static_assert(A_COUNT == 28, "action table size (2 poses + 2x13)");

struct ActionDef
{
    const char* name;
    int32_t     type;    // XrActionType, 1.0 values: 1 bool, 2 float, 3 vec2, 4 pose
    const char* path;    // standard KHR simple-controller input path
};

constexpr ActionDef kActions[A_COUNT] = {
    { "right/pose",             XR_ACTION_TYPE_POSE_INPUT_VALUE,    "/user/input/right/pose" },
    { "left/pose",              XR_ACTION_TYPE_POSE_INPUT_VALUE,    "/user/input/left/pose"  },
    { "right/trigger",          XR_ACTION_TYPE_FLOAT_INPUT_VALUE,   "/user/input/right/gamepad/trigger" },
    { "right/squeeze",          XR_ACTION_TYPE_FLOAT_INPUT_VALUE,   "/user/input/right/gamepad/squeeze" },
    { "right/grip",             XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/right/gamepad/grip" },
    { "right/thumbstick",       XR_ACTION_TYPE_VECTOR2F_INPUT_VALUE,"/user/input/right/gamepad/thumbstick" },
    { "right/thumbstick/click", XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/right/gamepad/thumbstick/click" },
    { "right/a",                XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/right/a" },
    { "right/b",                XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/right/b" },
    { "right/x",                XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/right/x" },
    { "right/y",                XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/right/y" },
    { "right/dpad/up",          XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/right/dpad/up" },
    { "right/dpad/down",        XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/right/dpad/down" },
    { "right/dpad/left",        XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/right/dpad/left" },
    { "right/dpad/right",       XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/right/dpad/right" },
    { "left/trigger",           XR_ACTION_TYPE_FLOAT_INPUT_VALUE,   "/user/input/left/gamepad/trigger" },
    { "left/squeeze",           XR_ACTION_TYPE_FLOAT_INPUT_VALUE,   "/user/input/left/gamepad/squeeze" },
    { "left/grip",              XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/left/gamepad/grip" },
    { "left/thumbstick",        XR_ACTION_TYPE_VECTOR2F_INPUT_VALUE,"/user/input/left/gamepad/thumbstick" },
    { "left/thumbstick/click",  XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/left/gamepad/thumbstick/click" },
    { "left/a",                 XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/left/a" },
    { "left/b",                 XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/left/b" },
    { "left/x",                 XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/left/x" },
    { "left/y",                 XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/left/y" },
    { "left/dpad/up",           XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/left/dpad/up" },
    { "left/dpad/down",         XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/left/dpad/down" },
    { "left/dpad/left",         XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/left/dpad/left" },
    { "left/dpad/right",        XR_ACTION_TYPE_BOOLEAN_INPUT_VALUE, "/user/input/left/dpad/right" },
};
static_assert(A_COUNT == (int)(sizeof(kActions) / sizeof(kActions[0])), "kActions table matches the enum");

constexpr const char* kSimpleControllerProfile = "/interaction_profiles/khr/simple_controller";

// ---- loader functions (resolved by Init through the session's getProc) --------
struct Fn
{
    PFN_xrStringToPath                        stringToPath = nullptr;
    PFN_xrCreateActionSet                     createActionSet = nullptr;
    PFN_xrDestroyActionSet                    destroyActionSet = nullptr;
    PFN_xrCreateAction                        createAction = nullptr;
    PFN_xrDestroyAction                       destroyAction = nullptr;
    PFN_xrSuggestInteractionProfileBindings   suggestBindings = nullptr;
    PFN_xrAttachSessionActionSets             attachSets = nullptr;
    PFN_xrSyncActions                         syncActions = nullptr;
    PFN_xrGetActionStateBoolean               getActionStateBoolean = nullptr;
    PFN_xrGetActionStateFloat                 getActionStateFloat = nullptr;
    PFN_xrGetActionStateVector2f              getActionStateVector2f = nullptr;
    PFN_xrCreateActionSpace                   createActionSpace = nullptr;
    PFN_xrLocateSpace                         locateSpace = nullptr;
    PFN_xrDestroySpace                        destroySpace = nullptr;    // own copy; harmless dup of session's
    bool ready = false;
};

// ---- module state (render thread only) ----------------------------------------
Fn            g_fn = {};
XrInstance    g_instance = nullptr;
XrSession     g_session = nullptr;         // session the session objects belong to
XrActionSet   g_actionSet = nullptr;
XrAction      g_actions[A_COUNT] = {};
XrPath        g_actionPaths[A_COUNT] = {};
XrSpace       g_rightSpace = nullptr;
XrSpace       g_leftSpace = nullptr;
bool          g_sessionReady = false;      // attached + spaces created for g_session
bool          g_sessionSetupFailed = false; // latch: session-object setup failed this session (log once)
bool          g_firstFrameLogged = false;   // one-shot "first tracked frame" log
Frame         g_frame = {};

bool          g_aimLatched = false;
XrQuaternionf g_aimQuat = {};

// ---- small math helpers ---------------------------------------------------------
XrQuaternionf QuatNormalize(const XrQuaternionf& q) noexcept
{
    const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    XrQuaternionf r = q;
    if (n > 1e-6f) { r.x /= n; r.y /= n; r.z /= n; r.w /= n; }
    return r;
}

XrQuaternionf QuatSlerp(const XrQuaternionf& a, const XrQuaternionf& b, float t) noexcept
{
    XrQuaternionf bb = b;
    float dot = a.x * bb.x + a.y * bb.y + a.z * bb.z + a.w * bb.w;
    if (dot < 0.0f) { dot = -dot; bb.x = -bb.x; bb.y = -bb.y; bb.z = -bb.z; bb.w = -bb.w; }
    XrQuaternionf r = {};
    if (dot > 0.9995f)   // near-parallel: normalize a linear mix (angle step per frame is small)
    {
        r.x = a.x + t * (bb.x - a.x); r.y = a.y + t * (bb.y - a.y);
        r.z = a.z + t * (bb.z - a.z); r.w = a.w + t * (bb.w - a.w);
        return QuatNormalize(r);
    }
    const float theta = std::acos(dot < 1.0f ? dot : 1.0f);
    const float s = std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) / s;
    const float wb = std::sin(t * theta) / s;
    r.x = a.x * wa + bb.x * wb; r.y = a.y * wa + bb.y * wb;
    r.z = a.z * wa + bb.z * wb; r.w = a.w * wa + bb.w * wb;
    return r;
}

// MUST match xr_session.cpp HeadEulerDegrees exactly (forward = R*(0,0,-1)).
void QuatYawPitchDeg(const XrQuaternionf& q, float& yawDeg, float& pitchDeg) noexcept
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float fx = -2.0f * (x * z + w * y);
    const float fy = -2.0f * (y * z - w * x);
    const float fz = -(1.0f - 2.0f * (x * x + y * y));
    constexpr float kRad2Deg = 57.2957795f;
    const float cfy = fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy);
    yawDeg = std::atan2(fx, -fz) * kRad2Deg;
    pitchDeg = std::asin(cfy) * kRad2Deg;
}

void DeadzoneRescale(float* x, float* y, float dz) noexcept
{
    const float m = std::sqrt(*x * *x + *y * *y);
    if (m < dz) { *x = 0.0f; *y = 0.0f; return; }
    const float s = (m - dz) / (1.0f - dz) / m;
    *x *= s; *y *= s;
}

// ---- session-bound (re)setup: attach action set + (re)create action spaces ----
// Failures are LATCHED (g_sessionSetupFailed): OnFrame runs every frame, so an
// attach/create problem must log once per session, not every frame.
bool EnsureSessionObjects() noexcept
{
    if (g_sessionReady) return true;
    if (g_sessionSetupFailed) return false;

    XrSessionActionSetsAttachInfo ai = {};
    ai.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO_VALUE;
    ai.countActionSets = 1;
    ai.actionSets = &g_actionSet;
    if (!XrSucceeded(g_fn.attachSets(g_session, &ai)))
    {
        g_sessionSetupFailed = true;
        LogLine("[XRINPUT] xrAttachSessionActionSets failed - controller input stays off this session");
        return false;
    }

    if (g_rightSpace != nullptr || g_leftSpace != nullptr)   // previous session's spaces
    {
        if (g_rightSpace != nullptr) g_fn.destroySpace(g_rightSpace);
        if (g_leftSpace  != nullptr) g_fn.destroySpace(g_leftSpace);
        g_rightSpace = g_leftSpace = nullptr;
    }
    XrActionSpaceCreateInfo sci = {};
    sci.type = XR_TYPE_ACTION_SPACE_CREATE_INFO_VALUE;
    sci.subactionPath = 0;               // XR_PATH_INVALID
    sci.poseInActionSpace = {};          // identity
    sci.action = g_actions[A_RIGHT_POSE];
    if (!XrSucceeded(g_fn.createActionSpace(g_session, &sci, &g_rightSpace)))
    {
        g_sessionSetupFailed = true;
        LogLine("[XRINPUT] xrCreateActionSpace(right) failed");
        return false;
    }
    sci.action = g_actions[A_LEFT_POSE];
    if (!XrSucceeded(g_fn.createActionSpace(g_session, &sci, &g_leftSpace)))
    {
        g_sessionSetupFailed = true;
        g_fn.destroySpace(g_rightSpace); // keep the pair together
        g_rightSpace = nullptr;
        LogLine("[XRINPUT] xrCreateActionSpace(left) failed");
        return false;
    }
    g_sessionReady = true;
    LogLine("[XRINPUT] session objects ready (attach + 2 action spaces)");
    return true;
}

// ---- action state readers (session-based; subactionPath = INVALID) -------------
void ReadFloat(XrAction a, float* out) noexcept
{
    XrActionStateGetInfo gi = {};
    gi.type = XR_TYPE_ACTION_STATE_GET_INFO_VALUE;
    gi.action = a;
    XrActionStateFloat st = {};
    st.type = XR_TYPE_ACTION_STATE_FLOAT_VALUE;
    if (XrSucceeded(g_fn.getActionStateFloat(g_session, &gi, &st)) && st.isActive != 0)
        *out = st.currentState;
}

void ReadBool(XrAction a, bool* out) noexcept
{
    XrActionStateGetInfo gi = {};
    gi.type = XR_TYPE_ACTION_STATE_GET_INFO_VALUE;
    gi.action = a;
    XrActionStateBoolean st = {};
    st.type = XR_TYPE_ACTION_STATE_BOOLEAN_VALUE;
    if (XrSucceeded(g_fn.getActionStateBoolean(g_session, &gi, &st)) && st.isActive != 0)
        *out = (st.currentState != 0);
}

void ReadVec2(XrAction a, float* x, float* y) noexcept
{
    XrActionStateGetInfo gi = {};
    gi.type = XR_TYPE_ACTION_STATE_GET_INFO_VALUE;
    gi.action = a;
    XrActionStateVector2f st = {};
    st.type = XR_TYPE_ACTION_STATE_VECTOR2F_VALUE;
    if (XrSucceeded(g_fn.getActionStateVector2f(g_session, &gi, &st)) && st.isActive != 0)
    { *x = st.currentState.x; *y = st.currentState.y; }
}

void FillHand(int base, HandFrame& h) noexcept
{
    ReadFloat(g_actions[base + 0], &h.trigger);
    ReadFloat(g_actions[base + 1], &h.squeeze);
    ReadBool(g_actions[base + 2], &h.grip);
    ReadVec2(g_actions[base + 3], &h.stickX, &h.stickY);
    ReadBool(g_actions[base + 4], &h.stickClick);
    ReadBool(g_actions[base + 5], &h.a);
    ReadBool(g_actions[base + 6], &h.b);
    ReadBool(g_actions[base + 7], &h.x);
    ReadBool(g_actions[base + 8], &h.y);
    ReadBool(g_actions[base + 9], &h.dpadUp);
    ReadBool(g_actions[base + 10], &h.dpadDown);
    ReadBool(g_actions[base + 11], &h.dpadLeft);
    ReadBool(g_actions[base + 12], &h.dpadRight);
}

}   // namespace

// =============================================================================
bool Init(XrInstance instance, XrSession session, PFN_xrGetInstanceProcAddr getProc) noexcept
{
    if (instance == nullptr || session == nullptr || getProc == nullptr) return false;
    if (g_fn.ready && g_actionSet != nullptr) return true;   // idempotent (session re-init)
    g_instance = instance;
    g_session = session;

    auto resolve = [&](const char* name, void** out) noexcept -> bool
    {
        PFN_xrVoidFunction p = nullptr;
        const XrResult r = getProc(instance, name, &p);
        if (!XrSucceeded(r) || p == nullptr) return false;
        *out = p;
        return true;
    };
    // Tolerant resolve: log every miss, decide after the full pass. The first
    // run's [XRINPUT] lines therefore show the loader's COMPLETE action-API
    // surface (which generation it is) instead of stopping at the first miss.
    bool resolveFailed = false;
#define TRY_R(name, field) \
    do { if (!resolve(name, reinterpret_cast<void**>(&g_fn.field))) \
        { LogLine(std::string("[XRINPUT] resolve ") + name + " FAILED"); resolveFailed = true; } } while (0)
    TRY_R("xrStringToPath", stringToPath)
    TRY_R("xrCreateActionSet", createActionSet)
    TRY_R("xrDestroyActionSet", destroyActionSet)
    TRY_R("xrCreateAction", createAction)
    TRY_R("xrDestroyAction", destroyAction)
    TRY_R("xrSuggestInteractionProfileBindings", suggestBindings)
    TRY_R("xrAttachSessionActionSets", attachSets)
    TRY_R("xrSyncActions", syncActions)
    TRY_R("xrGetActionStateBoolean", getActionStateBoolean)
    TRY_R("xrGetActionStateFloat", getActionStateFloat)
    TRY_R("xrGetActionStateVector2f", getActionStateVector2f)
    TRY_R("xrCreateActionSpace", createActionSpace)
    TRY_R("xrLocateSpace", locateSpace)
    TRY_R("xrDestroySpace", destroySpace)
#undef TRY_R

    if (resolveFailed)
    {
        LogLine("[XRINPUT] loader gen probe: REQUIRED action-API function missing "
                "(see per-name lines above) - module stays OFF (fail-safe)");
        g_fn.ready = false;
        return false;
    }
    g_fn.ready = true;
    LogLine("[XRINPUT] loader gen probe: 1.0-generation action API fully resolved "
            "(createActionSet/createAction/suggestBindings/attachSessionActionSets/"
            "syncActions/getActionState{Boolean,Float,Vector2f}/createActionSpace/locateSpace)");

    // ---- action set (instance-level, created once) ----
    XrActionSetCreateInfo asi = {};
    asi.type = XR_TYPE_ACTION_SET_CREATE_INFO_VALUE;
    std::strncpy(asi.actionSetName, "melevr-input", sizeof(asi.actionSetName) - 1);
    std::strncpy(asi.localizedActionSetName, "MELE VR input", sizeof(asi.localizedActionSetName) - 1);
    asi.priority = 0;
    if (!XrSucceeded(g_fn.createActionSet(instance, &asi, &g_actionSet)))
    {
        LogLine("[XRINPUT] xrCreateActionSet failed");
        g_fn.ready = false;
        return false;
    }

    XrPath profile = 0;
    if (!XrSucceeded(g_fn.stringToPath(instance, kSimpleControllerProfile, &profile)))
    {
        LogLine("[XRINPUT] xrStringToPath(profile) failed");
        g_fn.ready = false;
        return false;
    }

    // ---- actions + suggested bindings (KHR simple controller) ----
    XrActionSuggestedBinding bindings[A_COUNT];
    for (int i = 0; i < A_COUNT; ++i)
    {
        XrActionCreateInfo aci = {};
        aci.type = XR_TYPE_ACTION_CREATE_INFO_VALUE;
        std::strncpy(aci.actionName, kActions[i].name, sizeof(aci.actionName) - 1);
        std::strncpy(aci.localizedActionName, kActions[i].name, sizeof(aci.localizedActionName) - 1);
        aci.actionType = kActions[i].type;
        aci.countSubactionPaths = 0;
        aci.subactionPaths = nullptr;
        if (!XrSucceeded(g_fn.createAction(g_actionSet, &aci, &g_actions[i])))
        {
            LogLine(std::string("[XRINPUT] xrCreateAction('") + kActions[i].name + "') failed");
            g_fn.ready = false;
            return false;
        }
        if (!XrSucceeded(g_fn.stringToPath(instance, kActions[i].path, &g_actionPaths[i])))
        {
            LogLine(std::string("[XRINPUT] xrStringToPath('") + kActions[i].path + "') failed");
            g_fn.ready = false;
            return false;
        }
        bindings[i].action = g_actions[i];
        bindings[i].binding = g_actionPaths[i];
    }

    XrInteractionProfileSuggestedBinding sb = {};
    sb.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING_VALUE;
    sb.interactionProfile = profile;
    sb.countSuggestedBindings = A_COUNT;
    sb.suggestedBindings = bindings;
    if (!XrSucceeded(g_fn.suggestBindings(instance, &sb)))
    {
        LogLine("[XRINPUT] xrSuggestInteractionProfileBindings failed");
        g_fn.ready = false;
        return false;
    }

    LogLine(std::string("[XRINPUT] ready: action set + ") + std::to_string(A_COUNT) +
            " actions + simple-controller bindings; session objects (attach/spaces) on first RUNNING frame");
    return true;
}

void Shutdown() noexcept
{
    if (g_fn.ready && g_actionSet != nullptr)
    {
        for (int i = 0; i < A_COUNT; ++i)
            if (g_actions[i] != nullptr) g_fn.destroyAction(g_actions[i]);
        g_fn.destroyActionSet(g_actionSet);
        g_actionSet = nullptr;
        for (int i = 0; i < A_COUNT; ++i) g_actions[i] = nullptr;
    }
    if (g_fn.destroySpace != nullptr)
    {
        if (g_rightSpace != nullptr) { g_fn.destroySpace(g_rightSpace); g_rightSpace = nullptr; }
        if (g_leftSpace  != nullptr) { g_fn.destroySpace(g_leftSpace);  g_leftSpace  = nullptr; }
    }
    g_fn = {};
    g_sessionReady = false;
    g_sessionSetupFailed = false;
    g_aimLatched = false;
    g_frame = {};
}

bool IsReady() noexcept { return g_fn.ready; }

void SessionInvalidated() noexcept
{
    if (g_sessionReady)
    {
        if (g_rightSpace != nullptr) { g_fn.destroySpace(g_rightSpace); g_rightSpace = nullptr; }
        if (g_leftSpace  != nullptr) { g_fn.destroySpace(g_leftSpace);  g_leftSpace  = nullptr; }
        g_sessionReady = false;
        LogLine("[XRINPUT] session ended - action spaces dropped (recreated on next RUNNING)");
    }
    g_sessionSetupFailed = false;   // fresh session: allow the setup log to fire again
    g_firstFrameLogged = false;
    g_aimLatched = false;           // re-latch on the next session; no snap to the old quat
    g_frame = {};
}

void OnFrame(XrSpace appSpace, XrTime displayTime, const XrQuaternionf& headQuat) noexcept
{
    g_frame = {};   // fail-safe: a stale frame never leaks into this present
    if (!g_fn.ready || g_session == nullptr || appSpace == nullptr) return;

    if (!EnsureSessionObjects()) return;

    // 1) sync inputs once per frame (one action set, no subactions)
    XrActiveActionSet aas = {};
    aas.actionSet = g_actionSet;
    aas.subactionPath = 0;   // XR_PATH_INVALID
    XrActionsSyncInfo si = {};
    si.type = XR_TYPE_ACTIONS_SYNC_INFO_VALUE;
    si.countActiveActionSets = 1;
    si.activeActionSets = &aas;
    if (!XrSucceeded(g_fn.syncActions(g_session, &si))) return;

    // 2) locate both hands against the app space (same basis as the head pose)
    auto locateHand = [&](XrSpace space, HandFrame& out) noexcept -> bool
    {
        XrSpaceLocation loc = {};
        loc.type = XR_TYPE_SPACE_LOCATION_VALUE;
        if (!XrSucceeded(g_fn.locateSpace(space, appSpace, displayTime, &loc))) return false;
        const bool ok = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT_VALUE) != 0 &&
                        (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT_VALUE) != 0;
        if (!ok) return false;
        out.poseValid = true;
        out.poseOrientation = loc.pose.orientation;
        out.posePosition = loc.pose.position;
        return true;
    };
    g_frame.rightConnected = locateHand(g_rightSpace, g_frame.right);
    g_frame.leftConnected = locateHand(g_leftSpace, g_frame.left);
    if (!g_frame.rightConnected && !g_frame.leftConnected) return;
    g_frame.valid = true;
    if (!g_firstFrameLogged)
    {
        g_firstFrameLogged = true;
        LogLine(std::string("[XRINPUT] first tracked frame: right=") +
                (g_frame.rightConnected ? "valid" : "lost") + ", left=" +
                (g_frame.leftConnected ? "valid" : "lost"));
    }

    // 3) read action states
    FillHand(A_R_TRIG, g_frame.right);
    FillHand(A_L_TRIG, g_frame.left);

    // 4) aim source: right hand ray, head-blended, low-passed
    //    t in the slerp below is the HEAD weight (guarded so 0 = pure controller).
    const auto& cfg = MELEVR::Config::Get();
    if (cfg.controllerAim && g_frame.right.poseValid)
    {
        XrQuaternionf target = g_frame.right.poseOrientation;
        if (cfg.controllerAimHeadBlend > 0.001f)
            target = QuatSlerp(headQuat, target, cfg.controllerAimHeadBlend);
        if (!g_aimLatched)
        {
            g_aimQuat = target;
            g_aimLatched = true;
        }
        else
        {
            // frame-rate dependent on purpose (simple + tunable); dt-based in 1.1
            const float alpha = 1.0f - std::min(0.99f, cfg.controllerAimSmoothing);
            g_aimQuat = QuatSlerp(g_aimQuat, target, alpha);
        }
        QuatYawPitchDeg(g_aimQuat, g_frame.aimYawDeg, g_frame.aimPitchDeg);
        g_frame.aimValid = true;
    }
}

const Frame& GetFrame() noexcept { return g_frame; }

bool GetAimDeg(float* yawDeg, float* pitchDeg) noexcept
{
    const auto& cfg = MELEVR::Config::Get();
    if (!cfg.controllerAim || !g_frame.valid || !g_frame.aimValid) return false;
    *yawDeg = g_frame.aimYawDeg;
    *pitchDeg = g_frame.aimPitchDeg;
    return true;
}

bool AimActive() noexcept
{
    return MELEVR::Config::Get().controllerAim && g_frame.valid && g_frame.aimValid;
}

bool BuildVirtualGamepad(XINPUT_STATE* state, const MELEVR::Config::VrConfig& cfg) noexcept
{
    if (state == nullptr || !g_frame.valid) return false;

    const HandFrame& r = g_frame.right;
    const HandFrame& l = g_frame.left;
    // Zero the WHOLE state (dwPacketNumber included): when no physical pad is
    // connected the read left it untouched, and the caller only increments.
    std::memset(state, 0, sizeof(*state));
    XINPUT_GAMEPAD& g = state->Gamepad;

    const float dz = cfg.controllerTriggerDeadzone;

    // Triggers: binary fire (over deadzone = full) - deterministic, no auto-fire jitter.
    g.bRightTrigger = (r.trigger > dz) ? 255 : 0;
    g.bLeftTrigger = (l.trigger > dz) ? 255 : 0;

    // Grips -> shoulders, stick clicks -> L3/R3.
    if (r.grip) g.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (l.grip) g.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (r.stickClick) g.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (l.stickClick) g.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;

    // Face buttons 1:1 - DEFAULT pending controllerLogRealPad verification (T4).
    if (r.a) g.wButtons |= XINPUT_GAMEPAD_A;
    if (r.b) g.wButtons |= XINPUT_GAMEPAD_B;
    if (r.x) g.wButtons |= XINPUT_GAMEPAD_X;
    if (r.y) g.wButtons |= XINPUT_GAMEPAD_Y;

    // Dpad (either hand; ME1 binds the weapon wheel / ui there).
    if (r.dpadUp || l.dpadUp) g.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    if (r.dpadDown || l.dpadDown) g.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (r.dpadLeft || l.dpadLeft) g.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (r.dpadRight || l.dpadRight) g.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;

    // Left stick = movement (deadzone + rescale); the hook applies the same
    // RotateMoveStickByHeadLook correction afterwards as for the real pad.
    float lx = l.stickX, ly = l.stickY;
    DeadzoneRescale(&lx, &ly, 0.15f);
    g.sThumbLX = static_cast<SHORT>(lx * 32767.0f);
    g.sThumbLY = static_cast<SHORT>(ly * 32767.0f);

    // Right stick: head owns the look by default; opt-in mirrors it.
    if (cfg.controllerRightStickLook)
    {
        float rx = r.stickX, ry = r.stickY;
        DeadzoneRescale(&rx, &ry, 0.15f);
        g.sThumbRX = static_cast<SHORT>(rx * 32767.0f);
        g.sThumbRY = static_cast<SHORT>(ry * 32767.0f);
    }

    state->dwFlags = XINPUT_FLAG_GAMEPAD;
    return true;   // caller: dwPacketNumber++, return ERROR_SUCCESS from the hook
}

void LogRealPadThrottled(const XINPUT_STATE* realState, DWORD result) noexcept
{
    if (!MELEVR::Config::Get().controllerLogRealPad || realState == nullptr) return;
    static uint64_t s_lastMs = 0;
    const uint64_t now = static_cast<uint64_t>(GetTickCount64());
    if (now - s_lastMs < 500) return;   // 2 Hz
    s_lastMs = now;
    const XINPUT_GAMEPAD& g = realState->Gamepad;
    char btns[8];
    std::snprintf(btns, sizeof(btns), "%04X", g.wButtons);
    LogLine(std::string("[PADMAP] result=") + std::to_string(result) +
            " btns=0x" + btns +
            " trgR=" + std::to_string(g.bRightTrigger) + " trgL=" + std::to_string(g.bLeftTrigger) +
            " LX=" + std::to_string(g.sThumbLX) + " LY=" + std::to_string(g.sThumbLY) +
            " RX=" + std::to_string(g.sThumbRX) + " RY=" + std::to_string(g.sThumbRY));
}

}   // namespace MELEVR::XrInput
