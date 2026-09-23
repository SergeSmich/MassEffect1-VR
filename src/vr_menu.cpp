#include "vr_menu.h"

#include "vr_config.h"
#include "d3d_capture.h"  // DIBR depth-warp tunables (Set/GetDibrParams)
#include "render_hook.h"
#include "logger.h"
#include "xr_session.h"
#include "pchud.h"
#include "head_aim.h"   // [DECOUPLEMENU] ReadGameModeSEH: 7 = full-screen GUI
#include "xr_input.h"   // Stage 1: controller input (M0: menu section only)

#include <Windows.h>
#include <Xinput.h>
#include <d3d11.h>

#include <atomic>
#include <cmath>
#include <cstdio>  // sprintf_s for the VK-name label
#include <string>

#include "MinHook.h"
#include "imgui.h"
#include "backends/imgui_impl_dx11.h"

namespace MELEVR::Menu
{
namespace
{
bool g_open = false;
bool g_ready = false;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11Texture2D* g_tex = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
int g_w = 900, g_h = 720;
bool g_flatRender = false;

// After a VR-mode pick writes GamerSettings.ini, remember the resolution so a "restart to apply"
// banner (0 = no pending note). Set from the mode radio; purely a UI hint.
int s_modeResNote = 0;
int s_modeResNoteY = 0;

// ---- VR headset (OpenXR) enable toggle -------------------------------------------------------------
// XR on/off is decided once at boot by IsOpenXrDisabledForBoundary() (d3d_capture.cpp): the presence of
// MELEVR_DISABLE_OPENXR.txt next to the game exe = flat mono on the monitor, no headset. This toggle
// just creates/removes that marker + flags a restart, so VR can be switched without touching files.
std::wstring XrMarkerPath()
{
    wchar_t exePath[MAX_PATH] = {};
    const DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L"";
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash == nullptr) return L"";
    *(slash + 1) = L'\0';
    return std::wstring(exePath) + L"MELEVR_DISABLE_OPENXR.txt";
}

bool XrDisabledMarkerExists()
{
    const std::wstring p = XrMarkerPath();
    return !p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// enabled=true -> remove the marker (VR on); enabled=false -> write it (flat on the monitor).
void SetXrEnabled(bool enabled)
{
    const std::wstring p = XrMarkerPath();
    if (p.empty()) return;
    if (enabled)
    {
        DeleteFileW(p.c_str());
    }
    else
    {
        HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            const char msg[] = "flat: XR disabled from the Insert menu\n";
            DWORD wrote = 0;
            WriteFile(h, msg, sizeof(msg) - 1, &wrote, nullptr);
            CloseHandle(h);
        }
    }
}
bool s_xrRestartPending = false;

// XInput block: while the menu is open the GAME's XInputGetState returns a NEUTRAL pad (so navigating
// the menu can't drive the character/camera). The menu reads the REAL pad via the original trampoline.
using XInputGetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetState_t g_origXInput = nullptr;
XInputGetState_t g_origXInputEx = nullptr;   // XInputGetStateEx (ordinal 100) real-read trampoline

// Live left-stick deflection magnitude (0..32767), captured from the REAL pad each poll before any menu
// zeroing. xr_session reads it to tell "sprinting" (stick pushed hard) from "standing" during a camera
// transition blend, so the FP-storm head-look switch can engage on the sprint instead of waiting ~1s for
// the CombatStorm camera mode to settle.
std::atomic<int> g_leftStickMag{0};

inline void CaptureLeftStick(const XINPUT_STATE* state, DWORD result, DWORD idx) noexcept
{
    if (result == ERROR_SUCCESS && idx == 0 && state != nullptr)
    {
        const int lx = state->Gamepad.sThumbLX;
        const int ly = state->Gamepad.sThumbLY;
        g_leftStickMag.store((std::max)(std::abs(lx), std::abs(ly)), std::memory_order_relaxed);
    }
}

// Stage 1 M2: replace the game's XInput result only when the user explicitly
// enables the virtual gamepad and a valid OpenXR hand frame exists. Any XR
// failure returns the original physical-pad result unchanged.
inline DWORD MaybeSynthesizeControllerPad(XINPUT_STATE* state, DWORD realResult) noexcept
{
    if (g_open || state == nullptr) return realResult;   // never drive the menu from VR actions
    const auto& c = MELEVR::Config::Get();
    if (!c.controllerInput) return realResult;
    if (!MELEVR::XrInput::BuildVirtualGamepad(state, c)) return realResult;
    ++state->dwPacketNumber;
    return ERROR_SUCCESS;
}

// Double-click R3 (right stick-click twice quickly) = recenter, on ANY controller: the game polls the pad
// through XInput, so every controller that drives the game (Xbox natively, DualSense/DualShock/others via
// Steam Input or a remapper) reaches this same hook. Double-click (not a single R3, which is melee) so it
// never fires during normal play; only the button state is READ, never blocked (melee still works).
// Latched so one double-click = exactly one recenter; xr_session drains it via ConsumeRecenterCombo().
// Ignored while the menu is open.
constexpr ULONGLONG kR3DoubleClickMs = 400;   // second click must land within this window
std::atomic<bool> g_recenterComboArm{false};
std::atomic<bool> g_r3Prev{false};
std::atomic<ULONGLONG> g_lastR3PressMs{0};

inline void DetectRecenterCombo(const XINPUT_STATE* state, DWORD result, DWORD idx) noexcept
{
    if (result != ERROR_SUCCESS || idx != 0 || state == nullptr) return;
    const bool down = (state->Gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
    const bool prev = g_r3Prev.exchange(down, std::memory_order_relaxed);
    if (!down || prev || g_open) return;   // only act on R3 rising edge, outside the menu
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last = g_lastR3PressMs.exchange(now, std::memory_order_relaxed);
    if (now - last <= kR3DoubleClickMs)    // second click inside the window -> recenter
    {
        g_recenterComboArm.store(true, std::memory_order_relaxed);
        g_lastR3PressMs.store(0, std::memory_order_relaxed);   // reset so a 3rd quick click needs a fresh pair
    }
}

// [MOVEFIX] Move follows head (2026-07-16): rotate the LEFT stick (movement) vector by the head-look yaw
// currently applied to the rendered view, so pushing forward runs where the head is LOOKING, not where the
// game's movement basis (ControlRotation) points. This is the FP-storm inversion fix: during sprints the
// view is head-look-rotated but the character steers off the raw stick, so "look left" read as "moving
// right". HeadLookYawUU is the EXACT render-side offset (invert + sensitivity already baked in) and is 0
// whenever head-look is off (menus, cine, Mako, weapon-out head-aim while walking) -> exact no-op there.
// Rotation preserves stick magnitude, so dead zones behave identically; components clamped to int16.
inline void RotateMoveStickByHeadLook(XINPUT_GAMEPAD* pad) noexcept
{
    const int32_t yawUU = MELEVR::RenderHook::HeadLookYawUU();
    if (yawUU == 0) return;
    if (pad->sThumbLX == 0 && pad->sThumbLY == 0) return;
    // Positive yawUU = view turned LEFT (proven: ApplyHeadRotation Ry maps ahead->view-right for yaw>0,
    // i.e. the camera rotated left). Rotate the stick vector left by the same angle: fwd -> fwd-left.
    const float a = static_cast<float>(yawUU) * (6.2831853f / 65536.0f);
    const float c = std::cos(a), s = std::sin(a);
    const float lx = static_cast<float>(pad->sThumbLX);
    const float ly = static_cast<float>(pad->sThumbLY);
    float rx = lx * c - ly * s;   // x=right, y=forward; +a rotates forward toward left (-x)
    float ry = lx * s + ly * c;
    if (rx > 32767.0f) rx = 32767.0f; else if (rx < -32768.0f) rx = -32768.0f;
    if (ry > 32767.0f) ry = 32767.0f; else if (ry < -32768.0f) ry = -32768.0f;
    pad->sThumbLX = static_cast<SHORT>(rx);
    pad->sThumbLY = static_cast<SHORT>(ry);
}

// [DECOUPLEMENU 2026-08-22] Decoupled pitch/yaw zero a look axis before the game ever reads it,
// which is right for gameplay and wrong for menus: while a full-screen GUI or the galaxy map is up,
// that axis is how you move the cursor, so entries simply cannot be reached. Reported against ME1:
// "some commands like universe map are out of reach unless DP is off". The option is about gameplay
// look, so it stands down whenever a menu owns the screen and comes back by itself afterwards.
// Cached briefly because the input hooks are polled far faster than this state can change.
bool MenuOwnsLook() noexcept
{
    static unsigned long long s_lastMs = 0;
    static bool s_cached = false;
    const unsigned long long now = GetTickCount64();
    if (now - s_lastMs >= 50)
    {
        s_lastMs = now;
        s_cached = (MELEVR::HeadAim::ReadGameModeSEH() == 7);   // 7 = GUI full-screen menu
    }
    return s_cached;
}

DWORD WINAPI HookedXInputGetState(DWORD idx, XINPUT_STATE* state) noexcept
{
    const DWORD realResult = g_origXInput ? g_origXInput(idx, state) : ERROR_DEVICE_NOT_CONNECTED;
    MELEVR::XrInput::LogRealPadThrottled(state, realResult);
    CaptureLeftStick(state, realResult, idx);
    DetectRecenterCombo(state, realResult, idx);   // double-click R3 = recenter (read before synthesis/menu zeroing)
    DWORD result = MaybeSynthesizeControllerPad(state, realResult);
    if (state != nullptr)
    {
        if (g_open)
        {
            ZeroMemory(&state->Gamepad, sizeof(XINPUT_GAMEPAD));  // game sees a neutral pad while the menu is open
        }
        else
        {
            const auto& c = MELEVR::Config::Get();
            const bool menuUi = MenuOwnsLook();   // [DECOUPLEMENU] give the axes back to menus
            if (c.decoupledPitch && !menuUi) state->Gamepad.sThumbRY = 0;  // head owns look up/down; stick only turns (yaw)
            if (c.decoupledYaw   && !menuUi) state->Gamepad.sThumbRX = 0;  // head owns turning; stick only tilts (pitch)
            if (c.moveFollowsHead) RotateMoveStickByHeadLook(&state->Gamepad);   // [MOVEFIX] run where you look
        }
    }
    return result;
}

// XInputGetStateEx (ordinal 100, no named export in xinput1_3/1_4). Some engines poll the pad through THIS
// entry, bypassing the named hook - that path was the menu camera leak fixed in the rebuild. Ported here so
// the menu camera-freeze holds on gamepad ALONE, now that the ControlRotation pin is gone. READ-ONLY: it only
// zeroes the pad the game reads while the menu is open; it never writes a game object.
DWORD WINAPI HookedXInputGetStateEx(DWORD idx, XINPUT_STATE* state) noexcept
{
    const DWORD realResult = g_origXInputEx ? g_origXInputEx(idx, state)
                                            : (g_origXInput ? g_origXInput(idx, state) : ERROR_DEVICE_NOT_CONNECTED);
    MELEVR::XrInput::LogRealPadThrottled(state, realResult);
    CaptureLeftStick(state, realResult, idx);
    DetectRecenterCombo(state, realResult, idx);   // double-click R3 = recenter (Ex poll path)
    DWORD result = MaybeSynthesizeControllerPad(state, realResult);
    if (state != nullptr)
    {
        if (g_open)
        {
            ZeroMemory(&state->Gamepad, sizeof(XINPUT_GAMEPAD));
        }
        else
        {
            const auto& c = MELEVR::Config::Get();   // same decouple as the named hook, for the Ex poll path
            const bool menuUi = MenuOwnsLook();   // [DECOUPLEMENU]
            if (c.decoupledPitch && !menuUi) state->Gamepad.sThumbRY = 0;
            if (c.decoupledYaw   && !menuUi) state->Gamepad.sThumbRX = 0;
            if (c.moveFollowsHead) RotateMoveStickByHeadLook(&state->Gamepad);   // [MOVEFIX]
        }
    }
    return result;
}


// Dishonored-style hard input block: window messages alone are not enough for UE games that read mouse
// through DirectInput/GetDeviceState or recenter the cursor every frame. Mouse movement still reaches the
// OS cursor so the menu can be dragged; buttons/keys are swallowed and DirectInput state is zeroed.
volatile LONG g_llLBtn = 0;
volatile LONG g_llRBtn = 0;
volatile LONG g_llWheel = 0;
HHOOK g_mouseLL = nullptr;
HHOOK g_keyLL = nullptr;
HANDLE g_llThread = nullptr;
DWORD g_llThreadId = 0;

using SetCursorPos_t = BOOL(WINAPI*)(int, int);
SetCursorPos_t g_origSetCursorPos = nullptr;

using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using DICreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(void*, REFGUID, LPVOID*, LPUNKNOWN);
using DIGetState_t = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID);
using DIGetData_t = HRESULT(STDMETHODCALLTYPE*)(void*, DWORD, LPVOID, LPDWORD, DWORD);
using GetRawInputData_t = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
GetRawInputData_t g_origGetRawInputData = nullptr;
bool g_rawInputHooked = false;
DirectInput8Create_t g_origDirectInput8Create = nullptr;
DICreateDevice_t g_origDICreateDevice = nullptr;
DIGetState_t g_origDIGetState = nullptr;
DIGetData_t g_origDIGetData = nullptr;
bool g_diCreateDeviceHooked = false;
bool g_diDeviceStateHooked = false;
bool g_setCursorHooked = false;
bool g_directInputHooked = false;
bool g_llThreadStarted = false;

// Key-rebind capture: while g_captureTarget != null, the next key press is written into it. One shared
// capture for every rebindable key (recenter, menu open, FP toggle, depth pop). g_captureTarget is set/
// cleared on the RENDER thread (BuildUI/PollRecenterRebind) but READ from the LL-hook thread (MenuKeyLLProc,
// below - a different OS thread, see MenuLLPumpThread) to know whether to capture a key there; atomic for
// that cross-thread read, not because the target int itself is written cross-thread (it never is).
// Declared here (ahead of MenuKeyLLProc) since the hook needs it; used again further down by the UI.
bool g_capturingKey = false;      // legacy alias kept for the recenter UI; mirrors (g_captureTarget != null)
std::atomic<int*> g_captureTarget{nullptr};
// Set by MenuKeyLLProc (the LL-hook thread) when it captures a vkCode during rebind; consumed and applied
// to *g_captureTarget by PollRecenterRebind on the RENDER thread. This handoff exists because this project's OWN
// low-level keyboard hook swallows every key except the live menu key while the menu is open (see
// MenuKeyLLProc) - a WH_KEYBOARD_LL hook that blocks an event (returns nonzero) does so BEFORE the OS's
// low-level input processing updates the key-state table GetAsyncKeyState reads from. So the old
// GetAsyncKeyState-polling approach in PollRecenterRebind could never see the NEW key: the hook itself ate it
// first. Fix: capture the vkCode directly inside the hook, before it gets swallowed, and hand it off here.
std::atomic<int> g_capturedVk{0};

LRESULT CALLBACK MenuMouseLLProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_open)
    {
        switch (wp)
        {
        case WM_LBUTTONDOWN: InterlockedExchange(&g_llLBtn, 1); return 1;
        case WM_LBUTTONUP:   InterlockedExchange(&g_llLBtn, 0); return 1;
        case WM_RBUTTONDOWN: InterlockedExchange(&g_llRBtn, 1); return 1;
        case WM_RBUTTONUP:   InterlockedExchange(&g_llRBtn, 0); return 1;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_XBUTTONDOWN: case WM_XBUTTONUP: return 1;
        case WM_MOUSEWHEEL:
        {
            const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
            if (m != nullptr) InterlockedExchangeAdd(&g_llWheel, static_cast<SHORT>(HIWORD(m->mouseData)));
            return 1;
        }
        case WM_MOUSEMOVE:
        default:
            break;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK MenuKeyLLProc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_open)
    {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (k != nullptr)
        {
            // Key-rebind capture (fix, 2026-07-12): grab the vkCode HERE, on keydown, before this same hook
            // swallows it below. Still returns 1 either way - the key never reaches the game during capture.
            if ((wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) &&
                g_captureTarget.load(std::memory_order_acquire) != nullptr &&
                k->vkCode != VK_LBUTTON && k->vkCode != VK_RBUTTON)
            {
                g_capturedVk.store(static_cast<int>(k->vkCode), std::memory_order_release);
                return 1;
            }
            // Exclude the LIVE menu-open key (cfg.menuKey), not a hardcoded VK_INSERT: xr_session.cpp's
            // close-the-menu check reads cfg.menuKey via GetAsyncKeyState, same as everything else this hook
            // swallows. If someone rebinds "Menu open key" away from Insert and this stayed hardcoded, their
            // new key would get swallowed same as any other - GetAsyncKeyState would never see it - and they'd
            // be locked in the menu with no way to close it. menuKey has no "disabled" state (see vr_config.h:
            // "no disable: that would lock you out"), so it's always a valid vk here.
            if (static_cast<int>(k->vkCode) != MELEVR::Config::Get().menuKey) return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

DWORD WINAPI MenuLLPumpThread(LPVOID) noexcept
{
    g_llThreadId = GetCurrentThreadId();
    HMODULE hookModule = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&MenuMouseLLProc), &hookModule);
    g_mouseLL = SetWindowsHookExW(WH_MOUSE_LL, MenuMouseLLProc, hookModule, 0);
    const DWORD mouseErr = (g_mouseLL != nullptr) ? 0 : GetLastError();
    g_keyLL = SetWindowsHookExW(WH_KEYBOARD_LL, MenuKeyLLProc, hookModule, 0);
    const DWORD keyErr = (g_keyLL != nullptr) ? 0 : GetLastError();
    if (g_mouseLL != nullptr && g_keyLL != nullptr)
    {
        MELEVR::Logger::LogLine("[MENUINPUT] low-level mouse/kbd hooks ready");
    }
    else
    {
        MELEVR::Logger::LogLine(std::string("[MENUINPUT] WARN low-level mouse/kbd hook install failed mouseErr=") +
            std::to_string(mouseErr) + " keyErr=" + std::to_string(keyErr));
    }
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

BOOL WINAPI HookedSetCursorPos(int x, int y) noexcept
{
    return g_open ? TRUE : (g_origSetCursorPos ? g_origSetCursorPos(x, y) : FALSE);
}

void PatchVtableSlot(void* obj, int slot, void* hook, void** original) noexcept
{
    if (obj == nullptr) return;
    void** vt = *reinterpret_cast<void***>(obj);
    if (vt == nullptr) return;
    DWORD oldProtect = 0;
    if (VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &oldProtect))
    {
        if (original != nullptr && *original == nullptr) *original = vt[slot];
        vt[slot] = hook;
        VirtualProtect(&vt[slot], sizeof(void*), oldProtect, &oldProtect);
    }
}

// [MOUSEDECOUPLE] Decoupled pitch/yaw for the MOUSE. The gamepad half just zeroes a stick axis in the
// XInput hook, but mouse look never passes through XInput, so the flags did nothing for mouse users.
// A mouse can arrive two ways and both are covered, zeroing ONE axis and leaving the other alone:
// DirectInput device state/buffered data, and WM_INPUT raw input (see HookedGetRawInputData).
// Which one the game actually uses is logged once, the first time an axis gets suppressed.
bool g_mouseDecoupleLoggedDI = false;
bool g_mouseDecoupleLoggedRaw = false;
// dinput.h is deliberately not included here (this file only forward-declares the DirectInput entry
// points as function pointers), so the two mouse-state sizes and the axis offsets are spelled out.
// DIMOUSESTATE  = LONG lX, lY, lZ + BYTE rgbButtons[4] = 16 bytes
// DIMOUSESTATE2 = LONG lX, lY, lZ + BYTE rgbButtons[8] = 20 bytes
// DIMOFS_X / _Y / _Z are the byte offsets of those LONGs, i.e. 0 / 4 / 8.
constexpr DWORD kDiMouseStateBytes  = 16;
constexpr DWORD kDiMouseState2Bytes = 20;
constexpr DWORD kDiMouseOfsX = 0;
constexpr DWORD kDiMouseOfsY = 4;

HRESULT STDMETHODCALLTYPE HookedDIGetState(void* self, DWORD bytes, LPVOID data) noexcept
{
    const HRESULT hr = g_origDIGetState ? g_origDIGetState(self, bytes, data) : E_FAIL;
    if (data == nullptr || bytes == 0) return hr;
    if (g_open) { ZeroMemory(data, bytes); return hr; }   // menu open: whole device silenced, as before
    // Mouse state is DIMOUSESTATE (16 bytes) or DIMOUSESTATE2 (20); a keyboard is 256. Both mouse
    // layouts start with LONG lX, lY, lZ, so the axis writes below are valid for either.
    if (bytes == kDiMouseStateBytes || bytes == kDiMouseState2Bytes)
    {
        const MELEVR::Config::VrConfig& c = MELEVR::Config::Get();
        if ((c.decoupledYaw || c.decoupledPitch) && !MenuOwnsLook())   // [DECOUPLEMENU]
        {
            LONG* axis = reinterpret_cast<LONG*>(data);
            if (c.decoupledYaw)   axis[0] = 0;   // head owns turning
            if (c.decoupledPitch) axis[1] = 0;   // head owns look up/down
            if (!g_mouseDecoupleLoggedDI)
            {
                g_mouseDecoupleLoggedDI = true;
                MELEVR::Logger::LogLine("[MOUSEDECOUPLE] mouse reaches the game through DirectInput GetDeviceState; axis suppressed");
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedDIGetData(void* self, DWORD cbObjectData, LPVOID data, LPDWORD count, DWORD flags) noexcept
{
    const HRESULT hr = g_origDIGetData ? g_origDIGetData(self, cbObjectData, data, count, flags) : E_FAIL;
    if (g_open && count != nullptr) { *count = 0; return hr; }
    // Buffered path: each element is a DIDEVICEOBJECTDATA whose dwOfs says which axis it carries.
    // Zero the DATA rather than dropping the element, so sequence numbers the game may track stay intact.
    if (data != nullptr && count != nullptr && *count > 0 && cbObjectData >= sizeof(DWORD) * 2)
    {
        const MELEVR::Config::VrConfig& c = MELEVR::Config::Get();
        if ((c.decoupledYaw || c.decoupledPitch) && !MenuOwnsLook())   // [DECOUPLEMENU]
        {
            unsigned char* p = reinterpret_cast<unsigned char*>(data);
            for (DWORD i = 0; i < *count; ++i, p += cbObjectData)
            {
                const DWORD ofs = *reinterpret_cast<const DWORD*>(p);
                const bool isX = (ofs == kDiMouseOfsX);
                const bool isY = (ofs == kDiMouseOfsY);
                if ((isX && c.decoupledYaw) || (isY && c.decoupledPitch))
                {
                    *reinterpret_cast<DWORD*>(p + sizeof(DWORD)) = 0;
                    if (!g_mouseDecoupleLoggedDI)
                    {
                        g_mouseDecoupleLoggedDI = true;
                        MELEVR::Logger::LogLine("[MOUSEDECOUPLE] mouse reaches the game through DirectInput GetDeviceData; axis suppressed");
                    }
                }
            }
        }
    }
    return hr;
}

// Raw input (WM_INPUT) is the other way a UE3 build reads the mouse. Same per-axis suppression.
UINT WINAPI HookedGetRawInputData(HRAWINPUT handle, UINT command, LPVOID data, PUINT size, UINT headerSize) noexcept
{
    const UINT r = g_origGetRawInputData ? g_origGetRawInputData(handle, command, data, size, headerSize)
                                         : static_cast<UINT>(-1);
    if (g_open || command != RID_INPUT || data == nullptr || r == static_cast<UINT>(-1)) return r;
    RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(data);
    if (ri->header.dwType != RIM_TYPEMOUSE) return r;
    const MELEVR::Config::VrConfig& c = MELEVR::Config::Get();
    if (!c.decoupledYaw && !c.decoupledPitch) return r;
    if ((ri->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0) return r;   // absolute devices: not mouse-look
    if (c.decoupledYaw)   ri->data.mouse.lLastX = 0;
    if (c.decoupledPitch) ri->data.mouse.lLastY = 0;
    if (!g_mouseDecoupleLoggedRaw)
    {
        g_mouseDecoupleLoggedRaw = true;
        MELEVR::Logger::LogLine("[MOUSEDECOUPLE] mouse reaches the game through raw input (WM_INPUT); axis suppressed");
    }
    return r;
}

HRESULT STDMETHODCALLTYPE HookedDICreateDevice(void* self, REFGUID guid, LPVOID* outDevice, LPUNKNOWN outer) noexcept
{
    const HRESULT hr = g_origDICreateDevice ? g_origDICreateDevice(self, guid, outDevice, outer) : E_FAIL;
    if (SUCCEEDED(hr) && outDevice != nullptr && *outDevice != nullptr && !g_diDeviceStateHooked)
    {
        PatchVtableSlot(*outDevice, 9, reinterpret_cast<void*>(&HookedDIGetState), reinterpret_cast<void**>(&g_origDIGetState));
        PatchVtableSlot(*outDevice, 10, reinterpret_cast<void*>(&HookedDIGetData), reinterpret_cast<void**>(&g_origDIGetData));
        g_diDeviceStateHooked = (g_origDIGetState != nullptr || g_origDIGetData != nullptr);
        if (g_diDeviceStateHooked) MELEVR::Logger::LogLine("[MENUINPUT] DirectInput device state hooks ready");
    }
    return hr;
}

HRESULT WINAPI HookedDirectInput8Create(HINSTANCE hinst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) noexcept
{
    const HRESULT hr = g_origDirectInput8Create ? g_origDirectInput8Create(hinst, version, riid, out, outer) : E_FAIL;
    if (SUCCEEDED(hr) && out != nullptr && *out != nullptr && !g_diCreateDeviceHooked)
    {
        PatchVtableSlot(*out, 3, reinterpret_cast<void*>(&HookedDICreateDevice), reinterpret_cast<void**>(&g_origDICreateDevice));
        g_diCreateDeviceHooked = (g_origDICreateDevice != nullptr);
        if (g_diCreateDeviceHooked) MELEVR::Logger::LogLine("[MENUINPUT] DirectInput CreateDevice hook ready");
    }
    return hr;
}
// Window-message block: while the menu is open, swallow the game's mouse + keyboard so navigating the
// menu can't rotate/fire/move the game. Input is polled directly (GetCursorPos / GetAsyncKeyState / XInput),
// which is unaffected by message swallowing - so the menu stays fully controllable and INSERT still closes it.
WNDPROC g_origWndProc = nullptr;
HWND g_gameHwnd = nullptr;

LRESULT CALLBACK MenuWndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (g_open)
    {
        switch (msg)
        {
        case WM_INPUT:
        case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
            return 0;  // swallowed - the game never sees it while the menu is open
        }
    }
    return CallWindowProcW(g_origWndProc, h, msg, w, l);
}

// "Recenter now" was clicked; ConsumeRecenterRequest() drains it (one-shot).
bool g_recenterRequest = false;

// "Saved" confirmation: frames remaining to show the little confirmation text after Save.
int g_savedFlashFrames = 0;

// Human-readable name for a VK code, for the "current recenter key" label.
const char* VkName(int vk) noexcept
{
    static char buf[32];
    switch (vk)
    {
    case VK_HOME:   return "Home";
    case VK_END:    return "End";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_PRIOR:  return "PageUp";
    case VK_NEXT:   return "PageDown";
    case VK_SPACE:  return "Space";
    case VK_TAB:    return "Tab";
    case VK_BACK:   return "Backspace";
    case VK_RETURN: return "Enter";
    default: break;
    }
    if (vk >= '0' && vk <= '9') { buf[0] = static_cast<char>(vk); buf[1] = 0; return buf; }
    if (vk >= 'A' && vk <= 'Z') { buf[0] = static_cast<char>(vk); buf[1] = 0; return buf; }
    if (vk >= VK_F1 && vk <= VK_F24) { sprintf_s(buf, "F%d", vk - VK_F1 + 1); return buf; }
    sprintf_s(buf, "VK 0x%02X", vk);
    return buf;
}

// Self-polled input: the menu reads its OWN mouse (GetCursorPos / GetAsyncKeyState) and the REAL pad
// (via g_origXInput, bypassing the block) and feeds both to ImGui. Called at the top of RenderFrame, so
// the menu no longer needs the parent to push input. Replicates the parked build's proven mapping.
void ApplyGamepadInput() noexcept
{
    ImGuiIO& io = ImGui::GetIO();

    // ---- mouse (mouse+keyboard players): a visible cursor in the menu, mapped from the OS cursor ----
    io.MouseDrawCursor = true;
    ClipCursor(nullptr);  // free the cursor (the game may have clipped it for mouse-look) so it can roam the menu
    POINT pt = {};
    if (GetCursorPos(&pt))
    {
        HWND h = g_gameHwnd ? g_gameHwnd : GetForegroundWindow();
        RECT rc = {};
        POINT origin = {0, 0};
        if (h != nullptr && ClientToScreen(h, &origin) && ScreenToClient(h, &pt) &&
            GetClientRect(h, &rc) && rc.right > 0 && rc.bottom > 0)
        {
            // The render can now be LARGER than the physical screen (the mod reports a bigger monitor so the
            // game draws past it - see InstallDisplayQueryHooks). The window then hangs off the desktop, but
            // the OS cursor cannot: it stops at the real screen edge. Normalising by the full client rect
            // therefore makes the bottom of the menu unreachable - in AER at 3072x3264 on a 2160-tall screen
            // the cursor could only ever reach 2160/3264 = 66% of the menu height.
            //
            // Normalise by the REACHABLE part of the client rect instead. Must use the real primary size:
            // GetSystemMetrics is one of the calls spoofed here, so asking Windows would return that same lie.
            float reachW = static_cast<float>(rc.right);
            float reachH = static_cast<float>(rc.bottom);
            unsigned realW = 0, realH = 0;
            if (MELEVR::D3DCapture::GetRealPrimarySize(&realW, &realH) && realW > 0 && realH > 0)
            {
                const float availW = static_cast<float>(realW) - static_cast<float>(origin.x);
                const float availH = static_cast<float>(realH) - static_cast<float>(origin.y);
                if (availW > 1.0f && availW < reachW) reachW = availW;
                if (availH > 1.0f && availH < reachH) reachH = availH;
            }
            io.AddMousePosEvent(static_cast<float>(pt.x) / reachW * static_cast<float>(g_w),
                                static_cast<float>(pt.y) / reachH * static_cast<float>(g_h));
        }
    }
    const bool llMouseActive = (g_mouseLL != nullptr);
    io.AddMouseButtonEvent(0, llMouseActive ? (g_llLBtn != 0) : ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0));
    io.AddMouseButtonEvent(1, llMouseActive ? (g_llRBtn != 0) : ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0));
    const LONG wheel = InterlockedExchange(&g_llWheel, 0);
    if (wheel != 0) io.AddMouseWheelEvent(0.0f, static_cast<float>(wheel) / static_cast<float>(WHEEL_DELTA));

    // ---- gamepad: read the REAL pad (bypasses the block) and feed ImGui gamepad-nav keys ----
    // Reads ONLY through g_origXInput (the trampoline set by InstallInputBlock) - the clean project
    // doesn't link the static xinput import, so there's no direct-call fallback. Neutral until installed.
    XINPUT_STATE xs = {};
    const DWORD r = g_origXInput ? g_origXInput(0, &xs) : ERROR_DEVICE_NOT_CONNECTED;
    if (r != ERROR_SUCCESS) ZeroMemory(&xs, sizeof(xs));
    const unsigned short pad = xs.Gamepad.wButtons;
    const float lx = static_cast<float>(xs.Gamepad.sThumbLX) / 32767.0f;
    const float ly = static_cast<float>(xs.Gamepad.sThumbLY) / 32767.0f;

    auto btn = [&](int xb, ImGuiKey key) { io.AddKeyEvent(key, (pad & xb) != 0); };
    btn(XINPUT_GAMEPAD_DPAD_UP,        ImGuiKey_GamepadDpadUp);
    btn(XINPUT_GAMEPAD_DPAD_DOWN,      ImGuiKey_GamepadDpadDown);
    btn(XINPUT_GAMEPAD_DPAD_LEFT,      ImGuiKey_GamepadDpadLeft);
    btn(XINPUT_GAMEPAD_DPAD_RIGHT,     ImGuiKey_GamepadDpadRight);
    btn(XINPUT_GAMEPAD_A,              ImGuiKey_GamepadFaceDown);   // activate
    btn(XINPUT_GAMEPAD_B,              ImGuiKey_GamepadFaceRight);  // cancel / back
    btn(XINPUT_GAMEPAD_X,              ImGuiKey_GamepadFaceLeft);
    btn(XINPUT_GAMEPAD_Y,              ImGuiKey_GamepadFaceUp);
    btn(XINPUT_GAMEPAD_LEFT_SHOULDER,  ImGuiKey_GamepadL1);
    btn(XINPUT_GAMEPAD_RIGHT_SHOULDER, ImGuiKey_GamepadR1);

    // Left stick drives nav (move focus + tweak sliders). Dead-zone at 0.2, analog value for smooth tweaks.
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp,    ly > 0.2f,  ly > 0.0f ?  ly : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown,  ly < -0.2f, ly < 0.0f ? -ly : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft,  lx < -0.2f, lx < 0.0f ? -lx : 0.0f);
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, lx > 0.2f,  lx > 0.0f ?  lx : 0.0f);
}

// If rebind-capture mode is active, apply whatever key MenuKeyLLProc captured on the hook thread (it sees
// the key BEFORE the hook itself swallows it - see g_capturedVk's comment; a GetAsyncKeyState scan here would
// never see anything, since that key was already blocked from ever reaching the OS key-state table).
void PollRecenterRebind() noexcept
{
    int* target = g_captureTarget.load(std::memory_order_acquire);
    if (target == nullptr) { g_capturingKey = false; return; }
    const int vk = g_capturedVk.exchange(0, std::memory_order_acq_rel);
    if (vk != 0)
    {
        *target = vk;
        g_captureTarget.store(nullptr, std::memory_order_release);
        g_capturingKey = false;
    }
}

// "Key: X [Rebind]" row that captures the next keypress into *field. Distinct id per row. Caller adds
// its own ResetBtn afterward (ResetBtn is defined below this point).
void KeyRebindRow(const char* label, int& field, const char* id) noexcept
{
    ImGui::Text("%s: %s", label, VkName(field));
    ImGui::SameLine();
    if (g_captureTarget.load(std::memory_order_acquire) == &field)
    {
        ImGui::TextDisabled("press a key...");
    }
    else
    {
        ImGui::PushID(id);
        if (ImGui::Button("Rebind"))
        {
            g_captureTarget.store(&field, std::memory_order_release);
            g_capturingKey = true;
        }
        ImGui::PopID();
    }
}

// File-static defaults: a default-constructed VrConfig is the source of truth for every per-setting
// "rst" button - clicking one copies the matching field back out of here.
const Config::VrConfig kDefaults{};

// Per-setting reset: draws a small "rst" button on the same line as the control just submitted, and
// restores `field` to its default when clicked. `id` must be unique per call site (it's the ImGui id).
template <typename T>
void ResetBtn(const char* id, T& field, const T& def) noexcept
{
    ImGui::SameLine();
    // PushID makes the button's ID from the FULL id string (any length) - the old "rst##<id>" into a char[32]
    // overflowed for long ids (e.g. "cutsceneDisableHeadTracking"), which sprintf_s emptied to "", colliding
    // with any other overflowed reset button = ImGui "2 visible items with conflicting ID". Fixed 2026-07-07.
    ImGui::PushID(id);
    if (ImGui::SmallButton("rst")) field = def;
    ImGui::PopID();
}

float ControlWidth() noexcept
{
    return g_flatRender ? 360.0f : static_cast<float>(g_w) * 0.45f;
}

// [RELIEF M1.3] the depth-pop controls, shared by the Stereo AND AER sections (only one shows at a time).
// modeLabel = "plain stereo" / "plain AER" for the on/off hint.
void DrawReliefControls(Config::VrConfig& c, const char* modeLabel) noexcept
{
    // Depth pop is a Stereo/AER-only experimental feature - never for DIBR (which IS depth reprojection).
    if (c.dibrEnabled) return;
    (void)modeLabel;
    ImGui::SeparatorText("Depth pop - EXPERIMENTAL");
    ImGui::TextDisabled("Adds extra depth on top of the selected VR mode. Expect edge artifacts around");
    ImGui::TextDisabled("close objects (the depth-warp tradeoff, same as DIBR mode).");
    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                       "Warning: this costs real framerate on top of your VR mode and can cause slowdowns. "
                       "Test before a play session.");
    ImGui::Checkbox("Depth pop ENABLED", &c.reliefEnabled);   // clean A/B toggle
    ResetBtn("reliefEnabled", c.reliefEnabled, kDefaults.reliefEnabled);
    KeyRebindRow("Depth pop toggle key", c.depthPopKey, "depthPopKey");   // boots OFF; hotkey opts in per session
    ResetBtn("depthPopKey", c.depthPopKey, kDefaults.depthPopKey);
    ImGui::Checkbox("Depth pop key enabled", &c.depthPopKeyEnabled);
    ResetBtn("depthPopKeyEnabled", c.depthPopKeyEnabled, kDefaults.depthPopKeyEnabled);
    if (!c.reliefEnabled)
    {
        // don't leave the depth-map viz stuck on when the feature is off (its toggle lives inside)
        if (MELEVR::D3DCapture::GetDepthVizShow()) MELEVR::D3DCapture::SetDepthVizShow(false);
        return;
    }
    {
        // Stereo and AER hold SEPARATE depth-pop values - the warp differs (SBS halves vs a whole frame per
        // eye with opposite sign), so one number cannot serve both. These sliders edit the set for whichever
        // mode is live, so what you drag is always what you are looking at.
        const bool aer = c.aerEnabled;
        float* pStrength     = aer ? &c.reliefStrengthAer     : &c.reliefStrength;
        float* pEdgeGuard    = aer ? &c.reliefEdgeGuardAer    : &c.reliefEdgeGuard;
        float* pNearFreeze   = aer ? &c.reliefNearFreezeAer   : &c.reliefNearFreeze;
        float* pConvergence  = aer ? &c.reliefConvergenceAer  : &c.reliefConvergence;
        float* pCurve        = aer ? &c.reliefCurveAer        : &c.reliefCurve;
        bool*  pAutoConverge = aer ? &c.reliefAutoConvergeAer : &c.reliefAutoConverge;
        bool*  pFlip         = aer ? &c.reliefFlipAer         : &c.reliefFlip;

        const float dStrength    = aer ? kDefaults.reliefStrengthAer     : kDefaults.reliefStrength;
        const float dEdgeGuard   = aer ? kDefaults.reliefEdgeGuardAer    : kDefaults.reliefEdgeGuard;
        const float dNearFreeze  = aer ? kDefaults.reliefNearFreezeAer   : kDefaults.reliefNearFreeze;
        const float dConvergence = aer ? kDefaults.reliefConvergenceAer  : kDefaults.reliefConvergence;
        const float dCurve       = aer ? kDefaults.reliefCurveAer        : kDefaults.reliefCurve;
        const bool  dAutoConv    = aer ? kDefaults.reliefAutoConvergeAer : kDefaults.reliefAutoConverge;
        const bool  dFlip        = aer ? kDefaults.reliefFlipAer         : kDefaults.reliefFlip;

        ImGui::TextDisabled("Tuning the %s values (each mode keeps its own).", aer ? "AER" : "Stereo");

        // Depth pop costs real GPU time (a full-screen warp pass + a depth copy every frame). At the AER
        // Max resolution tier that cost has nowhere to go: measured 2026-07-10, avgMs jumped from a locked
        // 16.67ms (60fps, captureHz steady at 60.0) to 18-20ms the moment the warp turned on, and AER's own
        // cadence meter started wandering 49.6-60Hz - which IS the flicker (AER flicker is cadence drift
        // against the headset refresh, not a rendering artifact - see the AER baseline notes). One tier down
        // (Sharp, 2560x2720) leaves enough headroom and the cadence stayed locked with pop on. Not a code
        // bug - it is a straightforward frame-budget overrun, so warn rather than silently cap anything.
        MELEVR::XrSession::ResolutionStats rsRelief = {};
        if (aer && MELEVR::XrSession::GetResolutionStats(&rsRelief) && rsRelief.sourceWidth >= 2900)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.70f, 0.20f, 1.0f));
            ImGui::TextWrapped("!!  Depth pop + Max resolution can flicker in AER  !!  The warp pass pushes "
                               "the frame over budget, which drifts AER's cadence off the headset refresh. "
                               "If it flickers with pop on, drop to Sharp resolution ^(rerun MELE-VR.bat^) - "
                               "confirmed steady at that tier.");
            ImGui::PopStyleColor();
        }

        ImGui::SetNextItemWidth(ControlWidth());
        ImGui::SliderFloat("Depth pop strength", pStrength, 0.0f, 0.10f, "%.4f");
        ResetBtn("reliefStrength", *pStrength, dStrength);
        // [UNSHARP POP] shared across modes (one warp term, not per-mode like the base pop values):
        // pops LOCAL depth contrast (surfaces vs their surroundings) instead of distance-from-convergence,
        // so relief amplifies while world scale holds. Works alongside or instead of the base strength.
        ImGui::SetNextItemWidth(ControlWidth());
        ImGui::SliderFloat("Local pop (unsharp)", &c.reliefUnsharpStrength, 0.0f, 0.10f, "%.4f");
        ResetBtn("reliefUnsharpStrength", c.reliefUnsharpStrength, kDefaults.reliefUnsharpStrength);
        ImGui::SetNextItemWidth(ControlWidth());
        ImGui::SliderFloat("Local pop radius (px)", &c.reliefUnsharpRadius, 4.0f, 96.0f, "%.0f");
        ResetBtn("reliefUnsharpRadius", c.reliefUnsharpRadius, kDefaults.reliefUnsharpRadius);
        ImGui::SetNextItemWidth(ControlWidth());
        ImGui::SliderFloat("Edge guard (less ghost)", pEdgeGuard, 0.0f, 1.0f, "%.2f");
        ResetBtn("reliefEdgeGuard", *pEdgeGuard, dEdgeGuard);
        ImGui::SetNextItemWidth(ControlWidth());
        ImGui::SliderFloat("Near freeze (protect Shepard)", pNearFreeze, 0.90f, 1.02f, "%.4f");
        ResetBtn("reliefNearFreeze", *pNearFreeze, dNearFreeze);
        ImGui::Checkbox("Auto-converge pop (track subject)", pAutoConverge);
        ResetBtn("reliefAutoConverge", *pAutoConverge, dAutoConv);
        if (!*pAutoConverge)
        {
            ImGui::SetNextItemWidth(ControlWidth());
            ImGui::SliderFloat("Pop convergence", pConvergence, 0.90f, 1.00f, "%.4f");
            ResetBtn("reliefConvergence", *pConvergence, dConvergence);
        }
        ImGui::SetNextItemWidth(ControlWidth());
        ImGui::SliderFloat("Pop curve", pCurve, 0.25f, 3.0f, "%.2f");
        ResetBtn("reliefCurve", *pCurve, dCurve);
        ImGui::Checkbox("Flip pop (if it sinks in)", pFlip);
        ResetBtn("reliefFlip", *pFlip, dFlip);
    }

    // [DEPTH DARKEN] (research doc sec 3.1): soft shadow on the FAR side of every depth edge - makes objects
    // read as solid/separate without spending any stereo disparity (works even with pop strength at 0).
    // One shared set for all modes: it's a color effect, identical in both eyes, so mode doesn't change it.
    ImGui::SeparatorText("Depth darkening (solidity)");
    ImGui::TextDisabled("Darkens behind object edges - depth reads stronger with zero extra ghosting.");
    ImGui::SetNextItemWidth(ControlWidth());
    ImGui::SliderFloat("Darkening strength", &c.reliefDarkStrength, 0.0f, 1.0f, "%.2f");
    ResetBtn("reliefDarkStrength", c.reliefDarkStrength, kDefaults.reliefDarkStrength);
    ImGui::SetNextItemWidth(ControlWidth());
    ImGui::SliderFloat("Darkening radius (px)", &c.reliefDarkRadius, 1.0f, 32.0f, "%.0f");
    ResetBtn("reliefDarkRadius", c.reliefDarkRadius, kDefaults.reliefDarkRadius);

    ImGui::SeparatorText("Depth map (greyscale)");
    bool viz = MELEVR::D3DCapture::GetDepthVizShow();
    if (ImGui::Checkbox("Show depth map", &viz))
        MELEVR::D3DCapture::SetDepthVizShow(viz);
    if (viz)
    {
        ImGui::Checkbox("Manual window (else auto-fit)", &c.reliefVizManual);
        if (c.reliefVizManual)
        {
            ImGui::SetNextItemWidth(ControlWidth());
            ImGui::SliderFloat("White at depth (near)", &c.depthMapNear, 0.90f, 1.00f, "%.4f");
            ImGui::SetNextItemWidth(ControlWidth());
            ImGui::SliderFloat("Black at depth (far)", &c.depthMapFar, 0.80f, 1.00f, "%.4f");
            ImGui::SetNextItemWidth(ControlWidth());
            ImGui::SliderFloat("Gamma", &c.depthMapGamma, 0.20f, 3.0f, "%.2f");
            ImGui::Checkbox("Invert (flip black/white)", &c.depthMapFlip);
        }
    }
}

void ClampMenuOffsets(Config::VrConfig& c) noexcept
{
    if (c.menuOffsetXM < -1.5f) c.menuOffsetXM = -1.5f;
    else if (c.menuOffsetXM > 1.5f) c.menuOffsetXM = 1.5f;
    if (c.menuOffsetYM < -1.0f) c.menuOffsetYM = -1.0f;
    else if (c.menuOffsetYM > 1.0f) c.menuOffsetYM = 1.0f;
}

void DragMenuFromCurrentItem(Config::VrConfig& c) noexcept
{
    if (g_flatRender) return;
    if (!ImGui::IsItemActive() || !ImGui::IsMouseDragging(0, 0.0f)) return;
    const ImVec2 delta = ImGui::GetIO().MouseDelta;
    const float w = (g_w > 1) ? static_cast<float>(g_w) : 900.0f;
    const float h = (g_h > 1) ? static_cast<float>(g_h) : 720.0f;
    c.menuOffsetXM += delta.x / w;
    c.menuOffsetYM -= delta.y / h * (static_cast<float>(g_h) / static_cast<float>(g_w));
    ClampMenuOffsets(c);
}

// ============================================================================
// CLEAN, ORGANIZED UI - one window, CollapsingHeaders. Every checkbox/slider binds directly to the live
// config so edits apply instantly. Profiles up top, per-setting resets next to each control.
// ============================================================================
void BuildUI() noexcept
{
    Config::VrConfig& c = Config::Get();

    ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoCollapse;
    if (g_flatRender)
    {
        // Fit-any-screen (2026-07-18): the fixed 760x620 first-use size overflowed small displays
        // and the window could spawn with its grips off-screen = "can't move or resize it". Clamp
        // the initial size to the display and keep the window draggable/resizable at any resolution.
        const ImVec2 disp = ImGui::GetIO().DisplaySize;
        const float initW = (disp.x > 100.0f && disp.x - 48.0f < 760.0f) ? disp.x - 48.0f : 760.0f;
        const float initH = (disp.y > 100.0f && disp.y - 48.0f < 620.0f) ? disp.y - 48.0f : 620.0f;
        ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(initW, initH), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(320.0f, 240.0f), ImVec2(FLT_MAX, FLT_MAX));
    }
    else
    {
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(static_cast<float>(g_w), static_cast<float>(g_h)));
        windowFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                       ImGuiWindowFlags_NoBringToFrontOnFocus;
    }
    ImGui::Begin("MELE VR", nullptr, windowFlags);

    // Title banner stays above the tab bar. In-headset, it is also the quad drag handle.
    if (g_flatRender)
    {
        ImGui::TextUnformatted("MELE VR");
    }
    else
    {
        ImGui::Button("MELE VR - drag", ImVec2(-1.0f, 42.0f));
        DragMenuFromCurrentItem(c);
    }
    ImGui::Separator();

    // ---- LIVE HDR WARNING: the game's backbuffer is a float format = Windows/game HDR is ON. That gives the
    // blue / doubled headset image. Show it every time it's detected, not just first run. ----
    if (MELEVR::XrSession::IsHdrDetected())
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.30f, 0.25f, 1.0f));
        ImGui::TextWrapped("!!  HDR IS ON  !!  Turn HDR OFF in the game's video options, or the headset "
                           "image stays blue / doubled.");
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    // ---- LIVE RESOLUTION WARNING: the game reset the resolution the installer wrote. Happens when the user
    // opens the in-game video options (that dropdown can only offer resolutions the MONITOR supports - the
    // exact ceiling this mod exists to step around), or when the game flips BorderlessWindow off by itself
    // (observed 2026-07-10: chrome ate 64px of height and the frame came out 6030x3392 instead of 6144x3456).
    // Silent on old installs, where the bat never wrote expectedRes*. ----
    if (c.expectedResX > 0 && c.expectedResY > 0)
    {
        MELEVR::XrSession::ResolutionStats rs = {};
        if (MELEVR::XrSession::GetResolutionStats(&rs) && rs.sourceWidth > 0)
        {
            // 5% slack: a pixel or two off is not a reset.
            const bool reset = rs.sourceWidth < static_cast<uint32_t>(c.expectedResX) * 95u / 100u;
            if (reset)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.30f, 0.25f, 1.0f));
                ImGui::TextWrapped("!!  RESOLUTION WAS RESET  !!  The game is rendering at %ux%u, but the "
                                   "installer set %dx%d. Changing the in-game video options resets it. "
                                   "Quit and run MELE-VR.bat again to restore the sharp image.",
                                   rs.sourceWidth, rs.sourceHeight, c.expectedResX, c.expectedResY);
                ImGui::PopStyleColor();
                ImGui::Separator();
            }
        }
    }

    // ---- FIRST-RUN WELCOME: shown once (auto-opened this menu) until the user clicks 'Got it'. ----
    if (!c.firstRunDone)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.88f, 0.30f, 1.0f));
        ImGui::TextWrapped("Welcome to MELE VR!");
        ImGui::PopStyleColor();
        ImGui::TextWrapped("INSERT opens / closes this menu.   R = recenter.   K = first person.   P = depth pop.");
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.20f, 1.0f),
                           "IMPORTANT: turn OFF HDR in the game's video options, or the headset image will "
                           "be blue / doubled.");
        const char* modeName = c.aerEnabled ? "AER" : (c.dibrEnabled ? "DIBR" : (c.sfr2Enabled ? "Stereo" : "Mono"));
        ImGui::TextDisabled("You're in %s mode. Switch modes or tweak anything in the tabs below.", modeName);
        if (ImGui::Button("Got it - don't show this again"))
        {
            c.firstRunDone = true;
            MELEVR::Config::SaveToIni();
        }
        ImGui::Separator();
    }

    // ---- TABS: major areas split into tabs so no single page overflows the fixed 900x720 window ----
    if (ImGui::BeginTabBar("vrtabs"))
    {
        // ==== TAB: Tracking (Recenter + Head Look + Combat) ====
        if (ImGui::BeginTabItem("Tracking"))
        {
            // ---- Recenter ----
            if (ImGui::CollapsingHeader("Recenter", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (ImGui::Button("Recenter now")) g_recenterRequest = true;
                ImGui::TextDisabled("You can also recenter by double-clicking the right analog stick.");

                KeyRebindRow("Recenter key", c.recenterKey, "recenterKey");
                ResetBtn("recenterKey", c.recenterKey, kDefaults.recenterKey);
                ImGui::Checkbox("Recenter key enabled", &c.recenterKeyEnabled);
                ResetBtn("recenterKeyEnabled", c.recenterKeyEnabled, kDefaults.recenterKeyEnabled);

                ImGui::Separator();
                KeyRebindRow("Menu open key", c.menuKey, "menuKey");
                ResetBtn("menuKey", c.menuKey, kDefaults.menuKey);
                ImGui::TextDisabled("Opens / closes this menu.");
            }

            // ---- Head Look (explore) ----
            if (ImGui::CollapsingHeader("Head Look (explore)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Checkbox("Head tracking enabled", &c.headLookEnabled);
                ResetBtn("headLookEnabled", c.headLookEnabled, kDefaults.headLookEnabled);
                ImGui::Checkbox("Invert look yaw",   &c.invertLookYaw);
                ResetBtn("invertLookYaw", c.invertLookYaw, kDefaults.invertLookYaw);
                ImGui::Checkbox("Invert look pitch", &c.invertLookPitch);
                ResetBtn("invertLookPitch", c.invertLookPitch, kDefaults.invertLookPitch);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Look sensitivity", &c.lookSensitivity, 0.25f, 2.0f, "%.2f");
                ResetBtn("lookSensitivity", c.lookSensitivity, kDefaults.lookSensitivity);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Head-look smoothing", &c.headLookSmoothing, 0.0f, 0.9f, "%.2f");
                ResetBtn("headLookSmoothing", c.headLookSmoothing, kDefaults.headLookSmoothing);
            }

            // ---- Head Aim (HMD -> game ControlRotation; global during gameplay, no weapon detection) ----
            if (ImGui::CollapsingHeader("Head Aim", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Checkbox("Head drives aim (crosshair follows head)", &c.combatHeadAim);
                ResetBtn("combatHeadAim", c.combatHeadAim, kDefaults.combatHeadAim);
                ImGui::Checkbox("Invert aim yaw",   &c.invertAimYaw);
                ResetBtn("invertAimYaw", c.invertAimYaw, kDefaults.invertAimYaw);
                ImGui::Checkbox("Invert aim pitch", &c.invertAimPitch);
                ResetBtn("invertAimPitch", c.invertAimPitch, kDefaults.invertAimPitch);
                ImGui::Checkbox("Decoupled pitch (look up/down with head only)", &c.decoupledPitch);
                ResetBtn("decoupledPitch", c.decoupledPitch, kDefaults.decoupledPitch);
                ImGui::TextDisabled("Stick still turns sideways; only your head tilts the view up/down.");
                ImGui::TextDisabled("Note: also stops the stick raising/lowering the Mako cannon.");
                ImGui::Checkbox("Decoupled yaw (turn left/right with head only)", &c.decoupledYaw);
                ResetBtn("decoupledYaw", c.decoupledYaw, kDefaults.decoupledYaw);
                ImGui::TextDisabled("Stick stops turning you sideways; use your head + Recenter (R) to face forward.");
                // [MOVEFIX] "Run where you look" checkbox REMOVED from the menu 2026-07-17 ("it
                // shouldn't [be a toggle] anyway") - this is the confirmed FP-storm fix (the "look left,
                // move right" bug), not an experiment; unchecking it would silently reintroduce
                // the bug with no visible cause. c.moveFollowsHead stays baked true (default + ini), just
                // with no UI path to turn it off.
                ImGui::Separator();
                ImGui::Checkbox("EXPERIMENTAL: hold camera during head aim", &c.combatCamHold);
                ResetBtn("combatCamHold", c.combatCamHold, kDefaults.combatCamHold);
                ImGui::TextDisabled("Aim/body follow your head but the camera stops orbiting Shepard.");
                ImGui::TextDisabled("Crosshair stays accurate (holds position, not look direction).");
                if (c.combatCamHold)
                {
                    ImGui::SetNextItemWidth(ControlWidth());
                    ImGui::SliderFloat("Cam-hold strength", &c.combatCamHoldGain, 0.0f, 4.0f, "%.2f");
                    ResetBtn("combatCamHoldGain", c.combatCamHoldGain, kDefaults.combatCamHoldGain);
                    ImGui::TextDisabled("Raise until the camera stops swinging; too high = over-corrects.");
                    ImGui::Checkbox("  Invert (if camera swings the wrong way)", &c.combatCamHoldInvert);
                    ResetBtn("combatCamHoldInvert", c.combatCamHoldInvert, kDefaults.combatCamHoldInvert);
                }
                ImGui::Separator();
                ImGui::Checkbox("EXPERIMENTAL: Mako head aim", &c.makoHeadAim);
                ResetBtn("makoHeadAim", c.makoHeadAim, kDefaults.makoHeadAim);
            }

            // ---- Controller Input (Stage 1, 2026-09-23) ----
            // All OFF by default. M2 supports Oculus Touch / Quest 2 (including
            // Virtual Desktop) through the OpenXR action profile + XInput hooks.
            if (ImGui::CollapsingHeader("Controller Input (Stage 1)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Checkbox("Enable controller input (virtual gamepad)", &c.controllerInput);
                ResetBtn("controllerInput", c.controllerInput, kDefaults.controllerInput);
                ImGui::TextDisabled("Trigger=fire, grips=L1/R1, stick clicks=L3/R3, face=A/B/X/Y, dpad, left stick=move.");
                ImGui::Checkbox("Aim with right controller", &c.controllerAim);
                ResetBtn("controllerAim", c.controllerAim, kDefaults.controllerAim);
                ImGui::TextDisabled("Controller ray drives the crosshair (same ControlRotation write as head aim).");
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Aim smoothing", &c.controllerAimSmoothing, 0.0f, 0.9f, "%.2f");
                ResetBtn("controllerAimSmoothing", c.controllerAimSmoothing, kDefaults.controllerAimSmoothing);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Aim head blend (0=controller, 1=head)", &c.controllerAimHeadBlend, 0.0f, 1.0f, "%.2f");
                ResetBtn("controllerAimHeadBlend", c.controllerAimHeadBlend, kDefaults.controllerAimHeadBlend);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Trigger deadzone", &c.controllerTriggerDeadzone, 0.0f, 0.5f, "%.2f");
                ResetBtn("controllerTriggerDeadzone", c.controllerTriggerDeadzone, kDefaults.controllerTriggerDeadzone);
                ImGui::Checkbox("Right stick drives look (off = head owns look)", &c.controllerRightStickLook);
                ResetBtn("controllerRightStickLook", c.controllerRightStickLook, kDefaults.controllerRightStickLook);
                ImGui::Checkbox("Log real gamepad (mapping discovery, 2 Hz)", &c.controllerLogRealPad);
                ResetBtn("controllerLogRealPad", c.controllerLogRealPad, kDefaults.controllerLogRealPad);
                ImGui::TextDisabled("Verify button->action mapping vs ME1 XInput binds - see docs/STAGE1_CONTROLLER_DESIGN.md 11.");
            }

            // ---- Conversations & Cutscenes: [CINEVR2 2026-07-18, final shape] flat screen
            // by default; VR cine = opt-in EXPERIMENTAL per context, half-gameplay separation baked,
            // head-tracking toggle (off = head-locked 3D picture - must keep working), honest zoom.
            // Copy + layout tightened 2026-07-18 (round 2): zoom sits directly under its own toggle;
            // the screen-size sliders were cut (convoMonoQuadWidth/cutsceneMonoQuadWidth have no
            // effect while VR cine + head tracking are on - the only tested path - so exposing
            // them read as broken controls). Fields stay in the config for the flat/no-tracking paths
            // that DO use them; they just keep their baked defaults with no UI. ----
            if (ImGui::CollapsingHeader("Conversations & Cutscenes", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Separator();
                ImGui::Checkbox("VR Conversations", &c.cineVrConvo);
                ResetBtn("cineVrConvo", c.cineVrConvo, kDefaults.cineVrConvo);
                ImGui::Checkbox("VR Cutscenes", &c.cineVrCutscene);
                ResetBtn("cineVrCutscene", c.cineVrCutscene, kDefaults.cineVrCutscene);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Cine zoom", &c.cineZoom, 0.50f, 2.50f, "%.2fx");
                ResetBtn("cineZoom", c.cineZoom, kDefaults.cineZoom);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Cine depth", &c.cineSepScale, 0.25f, 1.00f, "%.2fx");
                ResetBtn("cineSepScale", c.cineSepScale, kDefaults.cineSepScale);
                // poseTagExact has no menu row: [CINETAG-AUTO] routing is the confirmed baseline
                // (default on) and a visible toggle only invites turning the fix off. Ini-only A/B.
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Head-tracking lock (gameplay)", &c.poseTagDelayFrames, 0.0f, 3.0f, "%.2f");
                ResetBtn("poseTagDelayFrames", c.poseTagDelayFrames, kDefaults.poseTagDelayFrames);
                ImGui::Checkbox("Head tracking in conversations/cutscenes", &c.cineHeadTracking);
                ResetBtn("cineHeadTracking", c.cineHeadTracking, kDefaults.cineHeadTracking);
                if (c.cineHeadTracking)
                {
                    ImGui::SetNextItemWidth(ControlWidth());
                    ImGui::SliderFloat("Cine head-tracking lock", &c.cinePoseTagDelayFrames, 0.0f, 3.0f, "%.2f");
                    ResetBtn("cinePoseTagDelayFrames", c.cinePoseTagDelayFrames, kDefaults.cinePoseTagDelayFrames);
                }
            }

            // ---- Menus: full-screen front-end (pause / inventory / squad / journal / map / options) ----
            // Detected via the engine's own game-mode flag (GUI mode), so it applies in EVERY VR mode and can
            // never bleed into gameplay. Loading screens & movies are handled separately.
            if (ImGui::CollapsingHeader("Menus", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Checkbox("Mono menus (readable, all VR modes)", &c.menuFlat);
                ResetBtn("menuFlat", c.menuFlat, kDefaults.menuFlat);
                if (c.menuFlat)
                {
                    ImGui::SetNextItemWidth(ControlWidth());
                    ImGui::SliderFloat("Menu mono size", &c.menuMonoQuadWidth, 1.00f, 3.50f, "%.2f");
                    ResetBtn("menuMonoQuadWidth", c.menuMonoQuadWidth, kDefaults.menuMonoQuadWidth);
                }
            }

            // ---- Galaxy map: mono by default, or VR with a zoom knob ----
            if (ImGui::CollapsingHeader("Galaxy map", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Checkbox("Mono galaxy map (off = VR)", &c.galaxyFlat);
                ResetBtn("galaxyFlat", c.galaxyFlat, kDefaults.galaxyFlat);
                if (c.galaxyFlat)
                {
                    ImGui::SetNextItemWidth(ControlWidth());
                    ImGui::SliderFloat("Galaxy mono size", &c.galaxyMonoQuadWidth, 1.00f, 3.50f, "%.2f");
                    ResetBtn("galaxyMonoQuadWidth", c.galaxyMonoQuadWidth, kDefaults.galaxyMonoQuadWidth);
                }
                else
                {
                    ImGui::SetNextItemWidth(ControlWidth());
                    ImGui::SliderFloat("Galaxy VR zoom", &c.galaxyVrZoom, 0.50f, 2.00f, "%.2fx");
                    ResetBtn("galaxyVrZoom", c.galaxyVrZoom, kDefaults.galaxyVrZoom);
                }
            }
            ImGui::EndTabItem();
        }

        // ==== TAB: First Person (M1: camera only, per-state eye offsets) ====
        if (ImGui::BeginTabItem("First Person"))
        {
            ImGui::Checkbox("First person camera", &c.fpEnabled);
            ResetBtn("fpEnabled", c.fpEnabled, kDefaults.fpEnabled);
            KeyRebindRow("Toggle key", c.fpToggleKey, "fpToggleKey");
            ResetBtn("fpToggleKey", c.fpToggleKey, kDefaults.fpToggleKey);
            ImGui::Checkbox("Toggle key enabled", &c.fpToggleKeyEnabled);
            ResetBtn("fpToggleKeyEnabled", c.fpToggleKeyEnabled, kDefaults.fpToggleKeyEnabled);
            ImGui::Checkbox("Hide head", &c.fpHideHead);
            ResetBtn("fpHideHead", c.fpHideHead, kDefaults.fpHideHead);
            if (c.fpHideHead)
            {
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Head hide delay (frames)", &c.fpHeadHideDelay, 0.0f, 60.0f, "%.0f");
                ResetBtn("fpHeadHideDelay", c.fpHeadHideDelay, kDefaults.fpHeadHideDelay);
            }
            ImGui::TextDisabled("Sniper scope, cover fire, Mako, convos & cutscenes stay third-person.");
            // FP eye position is genuinely per-user (seated vs standing, body size), so it stays
            // visible rather than tucked away as a dev-only control.
            if (c.fpEnabled)
            {
                struct FpUiState { const char* label; const char* prefix; MELEVR::Config::VrConfig::FpStateCfg* st; const MELEVR::Config::VrConfig::FpStateCfg* def; };
                FpUiState states[] = {
                    { "Exploration",         "fpExplore",      &c.fpExplore,      &kDefaults.fpExplore },
                    { "Exploration sprint",  "fpExploreStorm", &c.fpExploreStorm, &kDefaults.fpExploreStorm },
                    { "Combat",              "fpCombat",       &c.fpCombat,       &kDefaults.fpCombat },
                    { "Combat sprint",       "fpCombatStorm",  &c.fpCombatStorm,  &kDefaults.fpCombatStorm },
                    { "Combat ADS (aim)",    "fpCombatAim",    &c.fpCombatAim,    &kDefaults.fpCombatAim },
                };
                for (FpUiState& s : states)
                {
                    ImGui::PushID(s.prefix);
                    if (ImGui::CollapsingHeader(s.label, ImGuiTreeNodeFlags_DefaultOpen))
                    {
                        ImGui::Checkbox("First person in this state", &s.st->on);
                        ResetBtn("on", s.st->on, s.def->on);
                        ImGui::SetNextItemWidth(ControlWidth());
                        ImGui::SliderFloat("Forward (uu)", &s.st->fwd, -70.0f, 80.0f, "%.1f");
                        ResetBtn("fwd", s.st->fwd, s.def->fwd);
                        ImGui::SetNextItemWidth(ControlWidth());
                        ImGui::SliderFloat("Right (uu)", &s.st->right, -40.0f, 40.0f, "%.1f");
                        ResetBtn("right", s.st->right, s.def->right);
                        ImGui::SetNextItemWidth(ControlWidth());
                        ImGui::SliderFloat("Up (uu)", &s.st->up, 20.0f, 100.0f, "%.1f");
                        ResetBtn("up", s.st->up, s.def->up);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTabItem();
        }

        // ==== TAB: View (View framing + Lean 6DOF) ====
        if (ImGui::BeginTabItem("View"))
        {
            // ---- View (framing - muVR-style) ----
            if (ImGui::CollapsingHeader("View", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Checkbox("Camera shift enabled", &c.cameraOffsetEnabled);
                ResetBtn("cameraOffsetEnabled", c.cameraOffsetEnabled, kDefaults.cameraOffsetEnabled);
                if (c.cameraOffsetEnabled)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                       "Warning: shifting the camera can make geometry disappear (culling). VR framing only.");
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Shift right/left", &c.cameraOffsetRight, -2.0f, 2.0f, "%.2f m");
                ResetBtn("cameraOffsetRight", c.cameraOffsetRight, kDefaults.cameraOffsetRight);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Shift up/down", &c.cameraOffsetUp, -2.0f, 2.0f, "%.2f m");
                ResetBtn("cameraOffsetUp", c.cameraOffsetUp, kDefaults.cameraOffsetUp);
                ImGui::Checkbox("Screen distance enabled", &c.screenDistanceEnabled);
                ResetBtn("screenDistanceEnabled", c.screenDistanceEnabled, kDefaults.screenDistanceEnabled);
                if (c.screenDistanceEnabled)
                    ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                                       "Warning: changing screen distance can make geometry disappear (culling). VR framing only.");
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Screen distance", &c.flatScreenDistance, -4.0f, 4.0f, "%.2f m");
                ResetBtn("flatScreenDistance", c.flatScreenDistance, kDefaults.flatScreenDistance);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::Checkbox("VR FOV Fill", &c.vrFovFillEnabled);
                ResetBtn("vrFovFillEnabled", c.vrFovFillEnabled, kDefaults.vrFovFillEnabled);
                // Hidden entirely 2026-07-14: default ON (kDefaults.invMatrixFix = true), a
                // confirmed, always-on bug fix with no reason for anyone to ever touch it. No toggle.
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("VR Fill H", &c.vrFillH, 0.50f, 1.25f, "%.2f");
                ResetBtn("vrFillH", c.vrFillH, kDefaults.vrFillH);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("VR Fill V", &c.vrFillV, 0.50f, 1.25f, "%.2f");
                ResetBtn("vrFillV", c.vrFillV, kDefaults.vrFillV);

                MELEVR::XrSession::ResolutionStats res = {};
                if (MELEVR::XrSession::GetResolutionStats(&res))
                {
                    const float overscanX = res.eyeWidth != 0 ? static_cast<float>(res.sourceWidth) / static_cast<float>(res.eyeWidth) : 0.0f;
                    const float overscanY = res.eyeHeight != 0 ? static_cast<float>(res.sourceHeight) / static_cast<float>(res.eyeHeight) : 0.0f;
                    ImGui::Separator();
                    ImGui::Text("VR resolution: %ux%u", res.sourceWidth, res.sourceHeight);
                    ImGui::Text("Eye texture: %ux%u", res.eyeWidth, res.eyeHeight);
                    ImGui::Text("OpenXR recommended: %ux%u", res.recommendedEyeWidth, res.recommendedEyeHeight);
                    ImGui::Text("Overscan: %.2fx H  %.2fx V", overscanX, overscanY);
                }
            }

            // ---- Lean (positional 6DOF) ----
            if (ImGui::CollapsingHeader("Lean (6DOF)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Checkbox("Lean enabled", &c.leanEnabled);
                ResetBtn("leanEnabled", c.leanEnabled, kDefaults.leanEnabled);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Lean gain", &c.leanGain, 0.5f, 3.0f, "%.2f");
                ResetBtn("leanGain", c.leanGain, kDefaults.leanGain);
            }
            ImGui::EndTabItem();
        }

        // ==== TAB: Stereo / VR mode ====
        if (ImGui::BeginTabItem("VR"))
        {
            if (ImGui::CollapsingHeader("VR mode", ImGuiTreeNodeFlags_DefaultOpen))
            {
                // [MIRRORREC 2026-08-22] Desktop-mirror rate, for screen recording. In VR the flat window is
                // only a mirror - the headset image is already submitted before the mirror present - so it is
                // presented 1 frame in N. That throttle is not cosmetic: presenting every frame creates DWM
                // back-pressure that stalls the render thread, which is what capped ME2 at 60fps (87 -> 119.7
                // when throttled) and cost ME1 5.8ms -> 14.2ms per frame on identical work. It changes NOTHING
                // about resolution, image quality or the headset view - only the monitor picture and the frame
                // rate - so it is opt-in and the label says what it costs.
                {
                    const int cur = c.mirrorPresentEvery;
                    int sel = (cur <= 1) ? 2 : ((cur <= 2) ? 1 : 0);
                    static const char* const kMirrorItems[] = { "Normal (best frame rate)",
                                                                "Smooth - every 2nd frame",
                                                                "Every frame (smoothest, costs the most)" };
                    ImGui::SetNextItemWidth(360.0f);
                    if (ImGui::Combo("Desktop mirror (screen recording)", &sel, kMirrorItems, 3))
                    {
                        const int v = (sel == 2) ? 1 : ((sel == 1) ? 2 : 8);
                        c.mirrorPresentEvery = v;
                    }
                    ImGui::TextDisabled("Only the picture on your monitor. Does not change resolution, quality or");
                    ImGui::TextDisabled("the headset image. Raise it to record, put it back to Normal to play.");
                }
                ImGui::Separator();
                // "Stereo" = SFR now (the old SBS stereo was phased out 2026-07-15). Local radio enum:
                // 0 Mono, 1 Stereo(SFR), 2 AER, 3 DIBR - independent of the ini vrMode numbering.
                int vrMode = c.sfr2Enabled ? 1 : (c.aerEnabled ? 2 : (c.dibrEnabled ? 3 : 0));
                const int oldMode = vrMode;
                ImGui::RadioButton("Mono", &vrMode, 0);
                ImGui::SameLine();
                ImGui::RadioButton("Stereo##vrModeStereo", &vrMode, 1);
                ImGui::SameLine();
                ImGui::RadioButton("AER##vrModeAer", &vrMode, 2);
                ImGui::SameLine();
                ImGui::RadioButton("DIBR##vrModeDibr", &vrMode, 3);
                if (vrMode != oldMode)
                {
                    c.sfr2Enabled   = (vrMode == 1);
                    c.aerEnabled    = (vrMode == 2);
                    c.dibrEnabled   = (vrMode == 3);   // modes are mutually exclusive
                    c.stereoEnabled = false;           // legacy SBS stereo phased out - never activated

                    // setres per mode: write the matching resolution to GamerSettings.ini now; it applies on the
                    // next launch. Also persist the mode choice so the relaunch comes up in it.
                    MELEVR::Config::SaveToIni();
                    int wroteX = 0, wroteY = 0;
                    MELEVR::Config::ApplyResolutionForCurrentMode(&wroteX, &wroteY);
                    if (wroteX > 0) { s_modeResNote = wroteX; s_modeResNoteY = wroteY; }
                }
                {
                    ImGui::Checkbox("Auto-set resolution per mode", &c.applyModeResolution);
                    ImGui::TextDisabled("Stereo/AER/DIBR all need a ~square resolution (one full render per eye).");
                    ImGui::TextDisabled("The right one is written to GamerSettings.ini when you pick a mode.");
                }
                if (s_modeResNote > 0)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
                                       "Resolution set to %dx%d - RESTART the game to apply this mode.",
                                       s_modeResNote, s_modeResNoteY);
                }
            }

            // ---- Graphics ----
            if (ImGui::CollapsingHeader("Graphics", ImGuiTreeNodeFlags_DefaultOpen))
            {
                // Depth of field: writes DepthOfField into GamerSettings.ini [SystemSettings]. The engine
                // reads that at load, so it applies on the NEXT launch. Write immediately on toggle so the
                // file is already correct when the game restarts.
                if (ImGui::Checkbox("Disable depth of field", &c.disableDof))
                {
                    MELEVR::D3DCapture::EnsureDepthOfFieldSetting(c.disableDof);
                    MELEVR::Config::SaveToIni();
                }
                ResetBtn("disableDof", c.disableDof, kDefaults.disableDof);
                ImGui::TextDisabled("Can give the game a washed out look. RESTART to apply.");
            }

            // Legacy SBS "Stereo 1" UI block DELETED 2026-07-16 ("make sure the old stereo
            // never shows its ugly head again"). It was gated on c.stereoEnabled, which no UI path
            // sets anymore - but an old ini could re-arm it through key ordering. Belt: vr_config's
            // stereoEnabled key now MIGRATES to sfr2Enabled and pins itself false. Suspenders: the
            // panel is gone entirely. The render-side SBS code stays (dead, unreachable).

            // THE Stereo section (SFR). Formerly "Stereo 2"; promoted to the one Stereo mode 2026-07-15.
            if (c.sfr2Enabled && ImGui::CollapsingHeader("Stereo", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Stereo separation##sfr2", &c.sfr2HalfEyeUU, 0.0f, 8.0f, "%.2f uu");
                ResetBtn("sfr2HalfEyeUU", c.sfr2HalfEyeUU, kDefaults.sfr2HalfEyeUU);

                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Convergence##sfr2", &c.sfr2Convergence, -0.06f, 0.06f, "%.4f");
                ResetBtn("sfr2Convergence", c.sfr2Convergence, kDefaults.sfr2Convergence);

                // Resolution slider. SFR renders a FULL frame per eye, so the target is NEAR-square: the
                // height is width/kEyeWindowAspect, NOT width. Locking Y to X (what this did until
                // 2026-08-12) makes an exactly square target, which is the UI-4%-too-tall bug - so
                // touching this slider used to silently undo the aspect the installer had written.
                // Range back to 10240 now that [DOWNCHAIN] (xr_session.cpp) filters anything above ~2x
                // the eye texture down in halving passes before the eye blit.
                // Hidden from the normal menu 2026-07-16; resolution is picked in MELE-VR.bat.
                {
                    ImGui::SetNextItemWidth(ControlWidth());
                    const int prevRes = c.sfr2ResX;
                    ImGui::SliderInt("Stereo resolution (width)", &c.sfr2ResX, 1024, 10240, "%d");
                    c.sfr2ResY = MELEVR::Config::EyeHeightForWidth(c.sfr2ResX);
                    ResetBtn("sfr2ResX", c.sfr2ResX, kDefaults.sfr2ResX);
                    c.sfr2ResY = MELEVR::Config::EyeHeightForWidth(c.sfr2ResX);   // keep the aspect after a Reset
                    if (c.sfr2ResX != prevRes)
                    {
                        MELEVR::Config::SaveToIni();
                        int wx = 0, wy = 0;
                        MELEVR::Config::ApplyResolutionForCurrentMode(&wx, &wy);
                        if (wx > 0) { s_modeResNote = wx; s_modeResNoteY = wy; }
                    }
                    ImGui::TextDisabled("Restart to apply (writes GamerSettings.ini).");
                }

                // Frame pacing: paces to a clean fraction of the headset refresh. Default ON.
                ImGui::Checkbox("Cap frame rate for stability", &c.stereoFramePacing);
                ResetBtn("stereoFramePacing", c.stereoFramePacing, kDefaults.stereoFramePacing);

                // [TEARING] Default stays ON for everyone
                // (guarded in PresentHook) - this is only the visibility toggle, so the fix still works for users.
                {
                    ImGui::Checkbox("Uncap frame rate vs monitor refresh", &c.forceAllowTearing);
                    ResetBtn("forceAllowTearing", c.forceAllowTearing, kDefaults.forceAllowTearing);
                }

                // [LINKFOV] Quest Link fix - runtime-gated, no-op on VDXR, ships ON. Kill switch.
                ImGui::Checkbox("Quest Link image fix", &c.questFovMatch);
                ResetBtn("questFovMatch", c.questFovMatch, kDefaults.questFovMatch);

                // Swap eyes: user lever when depth reads inside-out.
                ImGui::Checkbox("Swap eyes##sfr2", &c.sfr2SwapEyes);
                ResetBtn("sfr2SwapEyes", c.sfr2SwapEyes, kDefaults.sfr2SwapEyes);

                // [RELIEF] Depth pop: per full-frame eye at submit (AER-style +-eyeSign on the separation).
                DrawReliefControls(c, "stereo");
            }

            if (c.aerEnabled && ImGui::CollapsingHeader("AER (alternate-eye)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                // "AER enabled" checkbox REMOVED 2026-07-18: redundant - the VR-mode radio
                // above is the one switch; a second enable bool here only invited inconsistent state.

                // AER square-resolution slider. ONLY shown in AER mode (square is an AER requirement; other
                // modes want wide). One slider, always square (Y locked to X), so it can't be set the wrong
                // shape. Wide range: low end for weak GPUs, high end for whoever wants to melt a 5090.
                if (c.aerEnabled)
                {
                    ImGui::SetNextItemWidth(ControlWidth());
                    const int prevRes = c.aerResX;
                    ImGui::SliderInt("AER resolution (square)", &c.aerResX, 1024, 7680, "%d");
                    c.aerResY = c.aerResX;   // always square - Y follows X, no way to break the aspect
                    ResetBtn("aerResX", c.aerResX, kDefaults.aerResX);
                    c.aerResY = c.aerResX;   // keep square even after a Reset
                    if (c.aerResX != prevRes)
                    {
                        MELEVR::Config::SaveToIni();
                        int wx = 0, wy = 0;
                        MELEVR::Config::ApplyResolutionForCurrentMode(&wx, &wy);
                        if (wx > 0) { s_modeResNote = wx; s_modeResNoteY = wy; }
                    }
                    ImGui::TextDisabled("Restart to apply (writes GamerSettings.ini).");
                }

                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("AER half eye (scale/IPD)", &c.aerHalfEyeUU, 0.0f, 8.0f, "%.2f uu");
                ResetBtn("aerHalfEyeUU", c.aerHalfEyeUU, kDefaults.aerHalfEyeUU);

                // Invert/swap eyes: eye-swap is a user lever in every mode.
                ImGui::Checkbox("Invert depth (swap eyes)", &c.aerSwapEyes);
                ResetBtn("aerSwapEyes", c.aerSwapEyes, kDefaults.aerSwapEyes);

                ImGui::Checkbox("Run at full frame rate##aer", &c.fullRefreshPacing);
                ResetBtn("fullRefreshPacing", c.fullRefreshPacing, kDefaults.fullRefreshPacing);

                {
                    ImGui::SetNextItemWidth(ControlWidth());
                    ImGui::SliderFloat("AER first-person eye scale", &c.aerFpEyeScale, 0.0f, 1.0f, "%.2f");
                    ResetBtn("aerFpEyeScale", c.aerFpEyeScale, kDefaults.aerFpEyeScale);

                    ImGui::Checkbox("Display-locked pacing (flicker fix)", &c.aerFramePacing);
                    ResetBtn("aerFramePacing", c.aerFramePacing, kDefaults.aerFramePacing);

                    // HAZARD: touching Pacing Hz live can hang the game - for testing only.
                    ImGui::SetNextItemWidth(ControlWidth());
                    ImGui::SliderInt("Pacing Hz (0 = auto)", &c.aerFramePacingHz, 0, 144);
                    ResetBtn("aerFramePacingHz", c.aerFramePacingHz, kDefaults.aerFramePacingHz);
                }


                // [RELIEF M1.3] the same depth-pop controls as Stereo (each fresh AER eye is warped full-frame).
                DrawReliefControls(c, "plain AER");
            }

            if (c.dibrEnabled && ImGui::CollapsingHeader("DIBR (depth reprojection)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::TextDisabled("Full framerate depth-warp stereo. Expect edge artifacts around close");
                ImGui::TextDisabled("objects (the depth-warp tradeoff).");
                if (ImGui::Checkbox("DIBR enabled", &c.dibrEnabled))
                {
                    if (c.dibrEnabled) { c.stereoEnabled = false; c.aerEnabled = false; }
                }
                ResetBtn("dibrEnabled", c.dibrEnabled, kDefaults.dibrEnabled);

                // DIBR renders one full-frame eye + reprojects the other, so it wants ~square res like AER.
                if (c.dibrEnabled)
                {
                    ImGui::SetNextItemWidth(ControlWidth());
                    const int prevRes = c.dibrResX;
                    ImGui::SliderInt("DIBR resolution (square)", &c.dibrResX, 1024, 7680, "%d");
                    c.dibrResY = c.dibrResX;   // always square
                    ResetBtn("dibrResX", c.dibrResX, kDefaults.dibrResX);
                    c.dibrResY = c.dibrResX;
                    if (c.dibrResX != prevRes)
                    {
                        MELEVR::Config::SaveToIni();
                        int wx = 0, wy = 0;
                        MELEVR::Config::ApplyResolutionForCurrentMode(&wx, &wy);
                        if (wx > 0) { s_modeResNote = wx; s_modeResNoteY = wy; }
                    }
                    ImGui::TextDisabled("Restart to apply (writes GamerSettings.ini).");
                }

                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Depth strength", &c.depthWarpGain, 0.0f, 5.0f, "%.2f");
                ResetBtn("depthWarpGain", c.depthWarpGain, kDefaults.depthWarpGain);

                {
                    ImGui::Checkbox("Auto-convergence (track subject)", &c.dibrAutoConverge);
                    ResetBtn("dibrAutoConverge", c.dibrAutoConverge, kDefaults.dibrAutoConverge);
                    if (!c.dibrAutoConverge)
                    {
                        ImGui::SetNextItemWidth(ControlWidth());
                        ImGui::SliderFloat("Convergence plane", &c.depthWarpConv, 0.90f, 1.00f, "%.4f");
                        ResetBtn("depthWarpConv", c.depthWarpConv, kDefaults.depthWarpConv);
                    }

                    // "Near hold" slider REMOVED 2026-07-07: it was a grid-scatter/far-bg knob; the restored
                    // guard-gather warp (DIBR v2 M0) never reads it - near protection is built into its guard.

                    ImGui::Checkbox("Flip depth", &c.depthWarpFlip);
                    ResetBtn("depthWarpFlip", c.depthWarpFlip, kDefaults.depthWarpFlip);
                }

            }
            ImGui::EndTabItem();
        }

        // ==== TAB: Comfort (menu placement) ====
        if (ImGui::BeginTabItem("Comfort"))
        {
            if (ImGui::CollapsingHeader("Menu comfort"))
            {
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Menu size", &c.menuSizeM, 0.5f, 2.5f, "%.2f");
                ResetBtn("menuSizeM", c.menuSizeM, kDefaults.menuSizeM);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Menu distance (m)", &c.menuDistanceM, 0.8f, 4.0f, "%.2f");
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Menu left/right (m)", &c.menuOffsetXM, -1.5f, 1.5f, "%.2f");
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Menu up/down (m)", &c.menuOffsetYM, -1.0f, 1.0f, "%.2f");
                if (ImGui::Button("Center menu")) { c.menuOffsetXM = kDefaults.menuOffsetXM; c.menuOffsetYM = kDefaults.menuOffsetYM; }
                ImGui::TextDisabled("Drag the MELE VR title bar to move this panel.");
            }
            ImGui::EndTabItem();
        }

        // ==== TAB: HUD (PCHUD Scaleform movie move/scale) - baked defaults ====
        // Every player has a different headset, IPD and comfort zone, so nudging HUD pieces into
        // view is ordinary setup, not an expert feature.
        if (ImGui::BeginTabItem("HUD"))
        {
            // [HUDNOTE] The HUD transforms are applied from the game's own HUD tick, which does
            // not run while this menu owns input, so edits land only once it is closed.
            ImGui::TextDisabled("Close this menu to see HUD size changes.");
            if (ImGui::CollapsingHeader("PCHUD movie", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Checkbox("PCHUD movie enabled", &c.pchudEnabled);
                ResetBtn("pchudEnabled", c.pchudEnabled, kDefaults.pchudEnabled);

                // Per-mode layout: stereo draws the UI per-eye (half width), so it edits its own *Stereo set;
                // every other mode (AER/Mono/DIBR) renders full-width and shares the "normal" set.
                const bool st = c.stereoEnabled;
                const bool di = c.dibrEnabled;
                ImGui::TextDisabled(di ? "Editing: DIBR layout" : (st ? "Editing: STEREO layout" : "Editing: Normal (AER/Mono) layout"));
                float* pScaleX  = di ? &c.pchudScaleXDibr  : (st ? &c.pchudScaleXStereo  : &c.pchudScaleX);
                float* pScaleY  = di ? &c.pchudScaleYDibr  : (st ? &c.pchudScaleYStereo  : &c.pchudScaleY);
                float* pOffsetX = di ? &c.pchudOffsetXDibr : (st ? &c.pchudOffsetXStereo : &c.pchudOffsetX);
                float* pOffsetY = di ? &c.pchudOffsetYDibr : (st ? &c.pchudOffsetYStereo : &c.pchudOffsetY);
                const float dScaleX  = di ? kDefaults.pchudScaleXDibr  : (st ? kDefaults.pchudScaleXStereo  : kDefaults.pchudScaleX);
                const float dScaleY  = di ? kDefaults.pchudScaleYDibr  : (st ? kDefaults.pchudScaleYStereo  : kDefaults.pchudScaleY);
                const float dOffsetX = di ? kDefaults.pchudOffsetXDibr : (st ? kDefaults.pchudOffsetXStereo : kDefaults.pchudOffsetX);
                const float dOffsetY = di ? kDefaults.pchudOffsetYDibr : (st ? kDefaults.pchudOffsetYStereo : kDefaults.pchudOffsetY);

                // Sliders read 0 at the tuned default (the baseline IS the origin). The stored config
                // value stays absolute (that's what drives the movie); the slider only shows/edits the delta
                // from the default, so "0" == the good HUD and you nudge +/- from there.
                float sxD = *pScaleX  - dScaleX;
                float syD = *pScaleY  - dScaleY;
                float oxD = *pOffsetX - dOffsetX;
                float oyD = *pOffsetY - dOffsetY;
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Scale X", &sxD, -0.15f, 1.15f, "%+.2f");
                *pScaleX = dScaleX + sxD;
                ResetBtn("pchudScaleX", *pScaleX, dScaleX);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Scale Y", &syD, -0.15f, 1.15f, "%+.2f");
                *pScaleY = dScaleY + syD;
                ResetBtn("pchudScaleY", *pScaleY, dScaleY);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Offset X", &oxD, -400.0f, 400.0f, "%+.0f");
                *pOffsetX = dOffsetX + oxD;
                ResetBtn("pchudOffsetX", *pOffsetX, dOffsetX);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Offset Y", &oyD, -400.0f, 400.0f, "%+.0f");
                *pOffsetY = dOffsetY + oyD;
                ResetBtn("pchudOffsetY", *pOffsetY, dOffsetY);

                if (ImGui::Button("Reset PCHUD movie"))
                {
                    // Full intended state: enabled + the tuned baseline (all sliders read 0). Resets the
                    // currently-edited set (stereo or normal); the enable bool is shared.
                    c.pchudEnabled = kDefaults.pchudEnabled;  // true
                    *pScaleX  = dScaleX;   // 0.35 (baseline)
                    *pScaleY  = dScaleY;   // 0.45 (baseline)
                    *pOffsetX = dOffsetX;  // 400 (baseline)
                    *pOffsetY = dOffsetY;  // 358 (baseline)
                }
                ImGui::SameLine();
                if (ImGui::Button("Disable PCHUD movie"))
                {
                    // Return the HUD to normal (scale 1/1, offset 0/0) once, THEN turn the feature off. The
                    // next Tick with enabled=true and these values applies the reset; the frame after, enabled
                    // is false. Simpler + safe: push the normalizing values now, drop enabled on the same click
                    // - SetTransform bumps the revision so the reset applies before the feature goes inert.
                    *pScaleX  = 1.0f;
                    *pScaleY  = 1.0f;
                    *pOffsetX = 0.0f;
                    *pOffsetY = 0.0f;
                    MELEVR::Pchud::SetTransform(true, 1.0f, 1.0f, 0.0f, 0.0f);
                    MELEVR::Pchud::Tick();   // apply the normalize once while still enabled
                    c.pchudEnabled = false;  // then the feature goes inert next frame
                }
                ImGui::TextDisabled("Moves/scales the game's HUD movie (bottom/top HUD). Crosshair is untouched.");

                // ---- Individual controls for the two stubborn static elements (2026-07-06): radar +
                // weapon/health bar. Each moves L/R + up/down + resizes AROUND ITSELF (its own registration
                // point), on top of the master transform above. Both are static containers -> safe to touch.
                // (The master scale now center-pivots, so it resizes the whole HUD in place; these two just
                // need independent placement.)
                ImGui::Separator();
                ImGui::TextDisabled("Move/resize these two on their own (on top of the master HUD above):");
                auto ElemControls = [&](const char* title, const char* id,
                                        MELEVR::Config::VrConfig::ElemOverride& e)
                {
                    ImGui::PushID(id);
                    ImGui::Checkbox(title, &e.on);
                    if (e.on)
                    {
                        // [HUDCONSIST 2026-08-22] Independent width/height with a Link button, the
                        // same shape ME2 and ME3 already use. This was one "Size" knob that forced
                        // e.scaleY = e.scaleX every frame; the renderer has always honoured the two
                        // axes separately (pchud.cpp applies syPct * e.scaleY), so the second axis
                        // was available all along and just was not reachable from the menu. Existing
                        // saved configs are unaffected: the forced assignment means every stored
                        // scaleY already equals its scaleX, so nothing moves on load.
                        ImGui::SetNextItemWidth(ControlWidth());
                        ImGui::SliderFloat("Left / right", &e.offX, -800.0f, 800.0f, "%+.0f");
                        ImGui::SetNextItemWidth(ControlWidth());
                        ImGui::SliderFloat("Up / down", &e.offY, -800.0f, 800.0f, "%+.0f");
                        ImGui::SetNextItemWidth(ControlWidth());
                        ImGui::SliderFloat("Size X (width)", &e.scaleX, 0.3f, 3.0f, "%.2f");
                        ImGui::SetNextItemWidth(ControlWidth());
                        ImGui::SliderFloat("Size Y (height)", &e.scaleY, 0.3f, 3.0f, "%.2f");
                        if (ImGui::SmallButton("Link Y to X")) e.scaleY = e.scaleX;
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Reset"))
                        {
                            e = MELEVR::Config::VrConfig::ElemOverride{};
                            e.on = true;
                        }
                    }
                    ImGui::PopID();
                };
                // hudElems[1] = radarMC, hudElems[0] = BottomUI (indices match kHudElemNames). Per-mode:
                // Stereo + DIBR each edit their own set; AER/Mono share the normal set. (di/st from above.)
                auto& hudSet = di ? c.hudElemsDibr : (st ? c.hudElemsStereo : c.hudElems);
                ElemControls("Radar", "elemRadar", hudSet[1]);
                ElemControls("Weapon (equip)", "elemWeapon", hudSet[11]);   // EquipBG = the weapon area (confirmed); per-mode

                // ---- Advanced: move/resize ANY element (tucked away; collapsed by default). Handy for
                // resizing something with no dedicated control. Names match kHudElemNames order.
                ImGui::Separator();
                if (ImGui::CollapsingHeader("Advanced: move/resize any element"))
                {
                    static const char* const kElemNames[] = {
                        "0 BottomUI (bottom bar)", "1 radarMC (radar)", "2 targetMC (name)", "3 topLeft",
                        "4 topRight", "5 leftUI", "6 rightUI", "7 squadMC", "8 weaponAbilityTab",
                        "9 EventHolder", "10 TeamBG", "11 EquipBG", "12 vehiclePause" };
                    const int kElemCount = static_cast<int>(sizeof(kElemNames) / sizeof(kElemNames[0]));
                    static int s_pickElem = 11;
                    if (s_pickElem >= kElemCount) s_pickElem = 0;
                    ImGui::SetNextItemWidth(ControlWidth());
                    ImGui::Combo("Element", &s_pickElem, kElemNames, kElemCount);
                    if (s_pickElem >= 0 && s_pickElem < kElemCount)
                        ElemControls("Move / resize picked element", "elemPick", hudSet[s_pickElem]);
                }
            }

            if (ImGui::CollapsingHeader("Conversation wheel / cutscene UI", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Checkbox("Conversation movie enabled", &c.convoEnabled);
                ResetBtn("convoEnabled", c.convoEnabled, kDefaults.convoEnabled);

                // Per-mode layout: Stereo + DIBR each edit their own set; AER/Mono share the normal set.
                const bool st = c.stereoEnabled;
                const bool di = c.dibrEnabled;
                ImGui::TextDisabled(di ? "Editing: DIBR layout" : (st ? "Editing: STEREO layout" : "Editing: Normal (AER/Mono) layout"));
                float* pScaleX  = di ? &c.convoScaleXDibr  : (st ? &c.convoScaleXStereo  : &c.convoScaleX);
                float* pScaleY  = di ? &c.convoScaleYDibr  : (st ? &c.convoScaleYStereo  : &c.convoScaleY);
                float* pOffsetX = di ? &c.convoOffsetXDibr : (st ? &c.convoOffsetXStereo : &c.convoOffsetX);
                float* pOffsetY = di ? &c.convoOffsetYDibr : (st ? &c.convoOffsetYStereo : &c.convoOffsetY);
                const float dScaleX  = di ? kDefaults.convoScaleXDibr  : (st ? kDefaults.convoScaleXStereo  : kDefaults.convoScaleX);
                const float dScaleY  = di ? kDefaults.convoScaleYDibr  : (st ? kDefaults.convoScaleYStereo  : kDefaults.convoScaleY);
                const float dOffsetX = di ? kDefaults.convoOffsetXDibr : (st ? kDefaults.convoOffsetXStereo : kDefaults.convoOffsetX);
                const float dOffsetY = di ? kDefaults.convoOffsetYDibr : (st ? kDefaults.convoOffsetYStereo : kDefaults.convoOffsetY);

                // Same delta-from-default sliders as PCHUD: 0 == the current baseline, nudge +/- from there.
                // Stretched at the bottom in VR, so it gets scale AND offset (scale un-stretches; offset lifts).
                float cSxD = *pScaleX  - dScaleX;
                float cSyD = *pScaleY  - dScaleY;
                float cOxD = *pOffsetX - dOffsetX;
                float cOyD = *pOffsetY - dOffsetY;
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Scale X##convo", &cSxD, -0.79f, 1.00f, "%+.2f");
                *pScaleX = dScaleX + cSxD;
                ResetBtn("convoScaleX", *pScaleX, dScaleX);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Scale Y##convo", &cSyD, -0.79f, 1.00f, "%+.2f");
                *pScaleY = dScaleY + cSyD;
                ResetBtn("convoScaleY", *pScaleY, dScaleY);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Offset X##convo", &cOxD, -600.0f, 600.0f, "%+.0f");
                *pOffsetX = dOffsetX + cOxD;
                ResetBtn("convoOffsetX", *pOffsetX, dOffsetX);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Offset Y##convo", &cOyD, -600.0f, 600.0f, "%+.0f");
                *pOffsetY = dOffsetY + cOyD;
                ResetBtn("convoOffsetY", *pOffsetY, dOffsetY);

                if (ImGui::Button("Reset conversation movie"))
                {
                    c.convoEnabled = kDefaults.convoEnabled;
                    *pScaleX  = dScaleX;
                    *pScaleY  = dScaleY;
                    *pOffsetX = dOffsetX;
                    *pOffsetY = dOffsetY;
                }
                ImGui::SameLine();
                if (ImGui::Button("Disable conversation movie"))
                {
                    *pScaleX  = 1.0f;
                    *pScaleY  = 1.0f;
                    *pOffsetX = 0.0f;
                    *pOffsetY = 0.0f;
                    MELEVR::Pchud::SetConvoTransform(true, 1.0f, 1.0f, 0.0f, 0.0f);
                    MELEVR::Pchud::Tick();   // normalize once while still enabled
                    c.convoEnabled = false;  // then inert next frame
                }
                ImGui::TextDisabled("Moves/scales the conversation wheel + cutscene dialog UI. Open during a conversation to tune it live.");

                // Conversation per-element controls removed from the UI 2026-07-06 (recalibration): the
                // convoElems backend stays (config + apply path) for the upcoming scaleform-subtitle work
                // (keep the SubtitleTop lane the user likes), but it is not a user-facing surface.
            }

            // ---- Designer UI: mission/boss scripted HUD overlays (charge counters, countdown timers).
            // Discovered 2026-07-11 mid-Saren-fight - a THIRD, independent Scaleform target, not part of the
            // persistent HUD movie the elements above come from. Open the menu WHILE the overlay is on screen
            // (e.g. mid-boss-fight) to tune it live, same as the conversation wheel above.
            if (ImGui::CollapsingHeader("Designer UI (mission/boss overlays)"))
            {
                ImGui::TextDisabled("Charge counters, countdown timers, boss-fight UI (e.g. the Saren fight's "
                                    "charge counter). A separate movie from the main HUD - open this WHILE "
                                    "it's on screen to tune it live.");
                ImGui::Checkbox("Designer UI enabled", &c.designerUiEnabled);
                ResetBtn("designerUiEnabled", c.designerUiEnabled, kDefaults.designerUiEnabled);

                float dSxD = c.designerUiScaleX  - kDefaults.designerUiScaleX;
                float dSyD = c.designerUiScaleY  - kDefaults.designerUiScaleY;
                float dOxD = c.designerUiOffsetX - kDefaults.designerUiOffsetX;
                float dOyD = c.designerUiOffsetY - kDefaults.designerUiOffsetY;
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Scale X##designerUi", &dSxD, -0.79f, 2.00f, "%+.2f");
                c.designerUiScaleX = kDefaults.designerUiScaleX + dSxD;
                ResetBtn("designerUiScaleX", c.designerUiScaleX, kDefaults.designerUiScaleX);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Scale Y##designerUi", &dSyD, -0.79f, 2.00f, "%+.2f");
                c.designerUiScaleY = kDefaults.designerUiScaleY + dSyD;
                ResetBtn("designerUiScaleY", c.designerUiScaleY, kDefaults.designerUiScaleY);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Offset X##designerUi", &dOxD, -900.0f, 900.0f, "%+.0f");
                c.designerUiOffsetX = kDefaults.designerUiOffsetX + dOxD;
                ResetBtn("designerUiOffsetX", c.designerUiOffsetX, kDefaults.designerUiOffsetX);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Offset Y##designerUi", &dOyD, -900.0f, 900.0f, "%+.0f");
                c.designerUiOffsetY = kDefaults.designerUiOffsetY + dOyD;
                ResetBtn("designerUiOffsetY", c.designerUiOffsetY, kDefaults.designerUiOffsetY);

                if (ImGui::Button("Reset Designer UI"))
                {
                    c.designerUiScaleX  = kDefaults.designerUiScaleX;
                    c.designerUiScaleY  = kDefaults.designerUiScaleY;
                    c.designerUiOffsetX = kDefaults.designerUiOffsetX;
                    c.designerUiOffsetY = kDefaults.designerUiOffsetY;
                }
            }

            // [LASERUI] The mining-laser interface (Therum) is the SAME designer-UI panel in a different
            // LAYOUT; the timer-tuned offsets above displaced it off-view. This set applies only while
            // the laser layout is active (auto-detected); default identity = the game's own placement.
            if (ImGui::CollapsingHeader("Mining laser UI (Therum)", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::TextDisabled("Applies only while the mining-laser layout is on screen (auto-detected).");
                ImGui::TextDisabled("Defaults = the game's own placement. Tune live at the laser puzzle.");
                ImGui::Checkbox("Laser UI custom transform", &c.laserUiEnabled);
                ResetBtn("laserUiEnabled", c.laserUiEnabled, kDefaults.laserUiEnabled);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Scale X##laserUi", &c.laserUiScaleX, 0.20f, 2.00f, "%.2f");
                ResetBtn("laserUiScaleX", c.laserUiScaleX, kDefaults.laserUiScaleX);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Scale Y##laserUi", &c.laserUiScaleY, 0.20f, 2.00f, "%.2f");
                ResetBtn("laserUiScaleY", c.laserUiScaleY, kDefaults.laserUiScaleY);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Offset X##laserUi", &c.laserUiOffsetX, -900.0f, 900.0f, "%+.0f");
                ResetBtn("laserUiOffsetX", c.laserUiOffsetX, kDefaults.laserUiOffsetX);
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Offset Y##laserUi", &c.laserUiOffsetY, -900.0f, 900.0f, "%+.0f");
                ResetBtn("laserUiOffsetY", c.laserUiOffsetY, kDefaults.laserUiOffsetY);
                if (ImGui::Button("Reset Laser UI"))
                {
                    c.laserUiScaleX  = kDefaults.laserUiScaleX;
                    c.laserUiScaleY  = kDefaults.laserUiScaleY;
                    c.laserUiOffsetX = kDefaults.laserUiOffsetX;
                    c.laserUiOffsetY = kDefaults.laserUiOffsetY;
                }
            }

            ImGui::EndTabItem();
        }

        // ==== TAB: Subtitles ====
        // Two paths exist: (A) CUSTOM POSITION = the overlay redraw - hides the game's subtitle and redraws
        // the same text with the game's own Canvas.DrawText at any X/Y/size (the ONLY free-position path;
        // native modes are fixed lanes). (B) native fixed-lane fallback (font size + top/bottom lane).
        if (false && ImGui::BeginTabItem("Subtitles"))   // HIDDEN 2026-07-07 (subtitles handled natively now; controls retired). Re-enable by removing 'false &&'.
        {
            ImGui::Checkbox("Custom subtitle position (recommended)", &c.subtitleRedraw);
            if (c.subtitleRedraw)
            {
                // custom position owns the subtitle: hide the game's original, drop the native fixed-lane path
                c.subtitleHideOriginal = true;
                c.nativeSubtitleEnabled = false;
                c.subtitleForce = false;
            }
            if (c.subtitleRedraw)
            {
                // Per-mode layout: Stereo + DIBR each edit their own set; AER/Mono share the normal set.
                const bool st = c.stereoEnabled;
                const bool di = c.dibrEnabled;
                ImGui::TextDisabled(di ? "Editing: DIBR layout" : (st ? "Editing: STEREO layout" : "Editing: Normal (AER/Mono) layout"));
                float* pPosX   = di ? &c.subtitlePosXFracDibr : (st ? &c.subtitlePosXFracStereo : &c.subtitlePosXFrac);
                float* pPosY   = di ? &c.subtitlePosYFracDibr : (st ? &c.subtitlePosYFracStereo : &c.subtitlePosYFrac);
                float* pScaleX = di ? &c.subtitleScaleXDibr   : (st ? &c.subtitleScaleXStereo   : &c.subtitleScaleX);
                float* pScaleY = di ? &c.subtitleScaleYDibr   : (st ? &c.subtitleScaleYStereo   : &c.subtitleScaleY);

                // Height: dragging RIGHT raises the subtitle. Stored as posYFrac (0=top,1=bottom), so
                // height = 1 - posY. Default posY 0.65 -> height 0.35 (lower third, up off the bottom edge).
                float height = 1.0f - *pPosY;
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Height  (drag right = up)", &height, 0.0f, 1.0f, "%.2f");
                *pPosY = 1.0f - height;

                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Center (left / right)", pPosX, 0.0f, 1.0f, "%.2f");

                float size = *pScaleX;
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Size", &size, 0.5f, 4.0f, "%.2f");
                *pScaleX = size;
                *pScaleY = size;

                // Font picker: cycle the game's live font assets until the look matches the native
                // subtitle. The name shows so the winner can be baked in afterwards.
                const int fontCount = MELEVR::Pchud::GetSubtitleFontCount();
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderInt("Font # (-2=matched)", &c.subtitleFontIndex, -2,
                                 fontCount > 0 ? fontCount - 1 : 0);
                const char* fontName = MELEVR::Pchud::GetSubtitleFontName(c.subtitleFontIndex);
                ImGui::TextDisabled("Font: %s", c.subtitleFontIndex == -2
                                        ? "MediumFont (the matched native font)"
                                        : ((fontName != nullptr && fontName[0] != '\0')
                                               ? fontName
                                               : (c.subtitleFontIndex < 0 ? "(plain)" : "(loading...)")));

                if (ImGui::Button("Reset subtitle"))
                {
                    *pPosX   = di ? kDefaults.subtitlePosXFracDibr : (st ? kDefaults.subtitlePosXFracStereo : kDefaults.subtitlePosXFrac);
                    *pPosY   = di ? kDefaults.subtitlePosYFracDibr : (st ? kDefaults.subtitlePosYFracStereo : kDefaults.subtitlePosYFrac);
                    *pScaleX = di ? kDefaults.subtitleScaleXDibr   : (st ? kDefaults.subtitleScaleXStereo   : kDefaults.subtitleScaleX);
                    *pScaleY = di ? kDefaults.subtitleScaleXDibr   : (st ? kDefaults.subtitleScaleXStereo   : kDefaults.subtitleScaleX);   // keep size square
                }
                ImGui::TextDisabled("Redraws the subtitle with the game's own text at your position.");
                ImGui::TextDisabled("Drag Height right to raise it off the bottom edge.");
            }
            else
            {
                ImGui::TextDisabled("Off = the game's native subtitle (fixed lane). Tune it below.");
            }

            // ---- Native fixed-lane fallback (font size proven; position = discrete lanes only) ----
            ImGui::Separator();
            if (ImGui::CollapsingHeader("Native subtitle (fixed lane)"))
            {
                ImGui::SetNextItemWidth(ControlWidth());
                ImGui::SliderFloat("Native font size (0=game)", &c.nativeSubtitleFontSize, 0.0f, 64.0f, "%.0f");
                ResetBtn("nativeSubtitleFontSize", c.nativeSubtitleFontSize, kDefaults.nativeSubtitleFontSize);

                if (ImGui::Checkbox("Force lane", &c.subtitleForce))
                {
                    if (c.subtitleForce) c.subtitleRedraw = false;   // custom-position would hide this draw
                }
                if (c.subtitleForce)
                {
                    const char* kModeNames[] = { "Default (1)", "Top (2)", "Bottom (3)", "Ambient (4)" };
                    int modeIdx = c.subtitleMode - 1;
                    if (modeIdx < 0 || modeIdx > 3) modeIdx = 2;
                    ImGui::SetNextItemWidth(ControlWidth());
                    if (ImGui::Combo("Lane", &modeIdx, kModeNames, 4)) c.subtitleMode = modeIdx + 1;
                }
                ImGui::TextDisabled("Fixed lanes only (top / bottom / default / ambient). For free up/down");
                ImGui::TextDisabled("use 'Custom subtitle position' above instead.");
            }
            ImGui::EndTabItem();
        }

        // ==== First Person tab REMOVED 2026-07-03 (strip-firstperson-livecode) ====
        // The per-state FP eye-offset UI + the "First person enabled (K)" ownership toggle drove live game-object
        // writes every frame (the gameplay hitch). The whole tab, the FpState config, and cameraOwnership are gone.

        // ==== TAB: Profiles (profiles + Save / Reset-ALL footer) ====
        if (ImGui::BeginTabItem("Profiles"))
        {
            // ---- Profiles ----
            if (ImGui::CollapsingHeader("Profiles", ImGuiTreeNodeFlags_DefaultOpen))
            {
                const int active = Config::ActiveProfileIndex();
                ImGui::Text("Active profile: %s", Config::ProfileName(active));

                // A row of selectable buttons - clicking one loads that slot. Gamepad/mouse friendly.
                const int count = Config::ProfileCount();
                for (int i = 0; i < count; ++i)
                {
                    if (i > 0) ImGui::SameLine();
                    const bool isActive = (i == active);
                    // Highlight the active slot so it reads clearly in-headset.
                    if (isActive) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    char btn[64];
                    sprintf_s(btn, "%s##profile%d", Config::ProfileName(i), i);
                    if (ImGui::Button(btn) && !isActive)
                    {
                        Config::LoadProfile(i);
                    }
                    if (isActive) ImGui::PopStyleColor();
                }

                char saveLabel[64];
                sprintf_s(saveLabel, "Save to %s", Config::ProfileName(active));
                if (ImGui::Button(saveLabel))
                {
                    Config::SaveProfile(active);
                    g_savedFlashFrames = 120;
                }
            }

            // ---- Profile hotkeys ----
            // Map a keyboard key to each profile slot; pressing it in-game live-loads that profile. The map
            // is GLOBAL (same whichever profile is active) and lives in the ini's [hotkeys] section. Any
            // rebind/clear writes the ini immediately so the mapping survives a crash or a straight quit.
            if (ImGui::CollapsingHeader("Profile hotkeys", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (ImGui::Checkbox("Enable profile hotkeys", &Config::ProfileHotkeysEnabledRef()))
                    Config::SaveToIni();
                ImGui::TextDisabled("Press a key in-game to instantly switch to that profile.");

                const int count = Config::ProfileCount();
                for (int i = 0; i < count; ++i)
                {
                    char rowLabel[64];
                    sprintf_s(rowLabel, "%s key", Config::ProfileName(i));
                    char rowId[32];
                    sprintf_s(rowId, "profHotkey%d", i);
                    // KeyRebindRow captures the next keypress into the referenced int (see its definition).
                    KeyRebindRow(rowLabel, Config::ProfileHotkeyRef(i), rowId);
                    ImGui::SameLine();
                    ImGui::PushID(rowId);
                    if (ImGui::SmallButton("clear")) Config::ProfileHotkeyRef(i) = 0;
                    ImGui::PopID();
                }
                // Persist when any binding actually changed. A rebind capture is applied a frame LATER (by
                // PollRecenterRebind, off this draw pass), so an inline per-row check would miss it; a
                // snapshot compared across frames catches both the capture result and the clear button.
                {
                    static int s_lastHk[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
                    bool changed = false;
                    for (int i = 0; i < count && i < 8; ++i)
                    {
                        const int cur = Config::ProfileHotkey(i);
                        if (cur != s_lastHk[i]) { changed = true; s_lastHk[i] = cur; }
                    }
                    if (changed) Config::SaveToIni();
                }
            }

            // ---- Save / Reset ALL (moved here from the bottom footer) ----
            ImGui::Separator();
            if (ImGui::Button("Save"))
            {
                Config::SaveToIni();
                g_savedFlashFrames = 120;  // ~2s at 60fps
            }
            ImGui::SameLine();
            if (ImGui::Button("Reset ALL to defaults")) Config::ResetDefaults();
            if (g_savedFlashFrames > 0)
            {
                --g_savedFlashFrames;
                ImGui::SameLine();
                ImGui::TextDisabled("saved");
            }
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }

    ImGui::End();
}
}  // namespace

void Init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height) noexcept
{
    if (g_ready || device == nullptr || context == nullptr) return;  // idempotent
    g_dev = device; g_ctx = context; g_w = width; g_h = height;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = nullptr;  // no imgui.ini
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(2.0f);  // big for headset legibility
    io.FontGlobalScale = 1.8f;
    ImGui_ImplDX11_Init(device, context);

    D3D11_TEXTURE2D_DESC d = {};
    d.Width = static_cast<UINT>(width);
    d.Height = static_cast<UINT>(height);
    d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // alpha so the quad blends over the game
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (SUCCEEDED(device->CreateTexture2D(&d, nullptr, &g_tex)) && g_tex != nullptr &&
        SUCCEEDED(device->CreateRenderTargetView(g_tex, nullptr, &g_rtv)) && g_rtv != nullptr)
    {
        g_ready = true;
    }
}

void Shutdown() noexcept
{
    if (g_mouseLL) { UnhookWindowsHookEx(g_mouseLL); g_mouseLL = nullptr; }
    if (g_keyLL) { UnhookWindowsHookEx(g_keyLL); g_keyLL = nullptr; }
    if (g_llThreadId != 0) PostThreadMessageW(g_llThreadId, WM_QUIT, 0, 0);
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_tex) { g_tex->Release(); g_tex = nullptr; }
    if (g_ready) { ImGui_ImplDX11_Shutdown(); ImGui::DestroyContext(); }
    g_ready = false;
}

bool IsOpen() noexcept { return g_open; }
int  LeftStickMagnitude() noexcept { return g_leftStickMag.load(std::memory_order_relaxed); }
void SetOpen(bool open) noexcept { if (g_open != open) Toggle(); }   // reuse Toggle's cursor/state cleanup + log
void Toggle() noexcept
{
    g_open = !g_open;
    InterlockedExchange(&g_llLBtn, 0);
    InterlockedExchange(&g_llRBtn, 0);
    InterlockedExchange(&g_llWheel, 0);
    ClipCursor(nullptr);
    MELEVR::Logger::LogLine(g_open ? "[MENUINPUT] Insert menu open: game mouse/kbd blocked" : "[MENUINPUT] Insert menu closed");
}

// Redundant now: RenderFrame self-polls input via ApplyGamepadInput(). Kept defined so the parent still
// links, but it's a no-op - the parent no longer needs to push input.
void FeedGamepad(unsigned short /*xinputButtons*/, float /*lx*/, float /*ly*/) noexcept
{
}

void SetGameWindow(void* hwnd) noexcept
{
    HWND h = reinterpret_cast<HWND>(hwnd);
    if (h == nullptr || h == g_gameHwnd) return;  // hook once
    g_gameHwnd = h;
    if (g_origWndProc == nullptr)
    {
        g_origWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&MenuWndProc)));
    }
}

void InstallInputBlock() noexcept
{
    MH_Initialize();  // idempotent (the render hook already initialized MinHook)

    if (!g_llThreadStarted)
    {
        g_llThread = CreateThread(nullptr, 0, MenuLLPumpThread, nullptr, 0, nullptr);
        if (g_llThread != nullptr)
        {
            CloseHandle(g_llThread);
            g_llThreadStarted = true;
        }
    }

    if (!g_setCursorHooked)
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        FARPROC p = user32 ? GetProcAddress(user32, "SetCursorPos") : nullptr;
        void* orig = nullptr;
        if (p != nullptr && MH_CreateHook(reinterpret_cast<void*>(p), reinterpret_cast<void*>(&HookedSetCursorPos), &orig) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void*>(p)) == MH_OK)
        {
            g_origSetCursorPos = reinterpret_cast<SetCursorPos_t>(orig);
            g_setCursorHooked = true;
            MELEVR::Logger::LogLine("[MENUINPUT] SetCursorPos hook ready");
        }
    }

    if (!g_rawInputHooked)
    {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        FARPROC p = user32 ? GetProcAddress(user32, "GetRawInputData") : nullptr;
        void* orig = nullptr;
        if (p != nullptr && MH_CreateHook(reinterpret_cast<void*>(p), reinterpret_cast<void*>(&HookedGetRawInputData), &orig) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void*>(p)) == MH_OK)
        {
            g_origGetRawInputData = reinterpret_cast<GetRawInputData_t>(orig);
            g_rawInputHooked = true;
            MELEVR::Logger::LogLine("[MOUSEDECOUPLE] GetRawInputData hook ready");
        }
    }

    if (!g_directInputHooked)
    {
        HMODULE dinput = GetModuleHandleW(L"dinput8.dll");
        if (dinput == nullptr) dinput = LoadLibraryW(L"dinput8.dll");
        FARPROC p = dinput ? GetProcAddress(dinput, "DirectInput8Create") : nullptr;
        void* orig = nullptr;
        if (p != nullptr && MH_CreateHook(reinterpret_cast<void*>(p), reinterpret_cast<void*>(&HookedDirectInput8Create), &orig) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void*>(p)) == MH_OK)
        {
            g_origDirectInput8Create = reinterpret_cast<DirectInput8Create_t>(orig);
            g_directInputHooked = true;
            MELEVR::Logger::LogLine("[MENUINPUT] DirectInput8Create hook ready");
        }
    }

    const char* dlls[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll", "xinput1_2.dll", "xinput1_1.dll" };
    for (const char* d : dlls)
    {
        HMODULE m = GetModuleHandleA(d);
        if (m == nullptr) continue;  // only hook XInput dlls the GAME already loaded
        FARPROC p = GetProcAddress(m, "XInputGetState");
        if (p == nullptr) continue;
        void* orig = nullptr;
        if (MH_CreateHook(reinterpret_cast<void*>(p), reinterpret_cast<void*>(&HookedXInputGetState), &orig) == MH_OK &&
            MH_EnableHook(reinterpret_cast<void*>(p)) == MH_OK)
        {
            if (g_origXInput == nullptr) g_origXInput = reinterpret_cast<XInputGetState_t>(orig);  // one real-read trampoline
            MELEVR::Logger::LogLine(std::string("[MENUINPUT] XInput hook ready: ") + d);
        }

        // XInputGetStateEx - ordinal 100, no named export (xinput1_3/1_4; absent from 9_1_0). Hook it too so the
        // menu freeze holds for engines that poll the pad through this entry (the ControlRotation pin is gone).
        FARPROC pex = GetProcAddress(m, reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(100)));
        if (pex != nullptr && pex != p)
        {
            void* origEx = nullptr;
            if (MH_CreateHook(reinterpret_cast<void*>(pex), reinterpret_cast<void*>(&HookedXInputGetStateEx), &origEx) == MH_OK &&
                MH_EnableHook(reinterpret_cast<void*>(pex)) == MH_OK)
            {
                if (g_origXInputEx == nullptr) g_origXInputEx = reinterpret_cast<XInputGetState_t>(origEx);
                MELEVR::Logger::LogLine(std::string("[MENUINPUT] XInputGetStateEx (ordinal 100) hook ready: ") + d);
            }
        }
    }
}

ID3D11Texture2D* RenderFrame() noexcept
{
    if (!g_ready || !g_open) return nullptr;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_w), static_cast<float>(g_h));
    io.DeltaTime = 1.0f / 72.0f;
    ApplyGamepadInput();
    PollRecenterRebind();

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    g_flatRender = false;
    BuildUI();
    ImGui::Render();

    // ImGui_ImplDX11_RenderDrawData backs up + restores the device-context state, so it won't corrupt the
    // game's pipeline. This points it at a dedicated RTV first and clears it transparent.
    ID3D11RenderTargetView* rtvs[1] = { g_rtv };
    g_ctx->OMSetRenderTargets(1, rtvs, nullptr);
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };  // transparent
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(g_w), static_cast<float>(g_h), 0.0f, 1.0f };
    g_ctx->RSSetViewports(1, &vp);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    return g_tex;
}

void RenderFlat(ID3D11RenderTargetView* target, int width, int height) noexcept
{
    if (!g_ready || !g_open || target == nullptr || width <= 0 || height <= 0) return;

    g_w = width;
    g_h = height;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(g_w), static_cast<float>(g_h));
    io.DeltaTime = 1.0f / 60.0f;
    ApplyGamepadInput();
    PollRecenterRebind();

    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    g_flatRender = true;
    BuildUI();
    g_flatRender = false;
    ImGui::Render();

    ID3D11RenderTargetView* rtvs[1] = { target };
    g_ctx->OMSetRenderTargets(1, rtvs, nullptr);
    D3D11_VIEWPORT vp = { 0.0f, 0.0f, static_cast<float>(g_w), static_cast<float>(g_h), 0.0f, 1.0f };
    g_ctx->RSSetViewports(1, &vp);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
}

int Width() noexcept { return g_w; }
int Height() noexcept { return g_h; }

bool ConsumeRecenterRequest() noexcept
{
    const bool r = g_recenterRequest;
    g_recenterRequest = false;
    return r;
}

// Drains the L3+R3 gamepad recenter latch (one-shot). Set from the XInput poll hook; consumed here.
bool ConsumeRecenterCombo() noexcept
{
    return g_recenterComboArm.exchange(false, std::memory_order_relaxed);
}
}



