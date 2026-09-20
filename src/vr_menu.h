#pragma once

// In-headset options menu (clean rebuild, Milestone 1): ImGui rendered into an OWNED D3D11 texture, which
// xr_session copies into a head-locked OpenXR quad and submits. Gamepad-nav only (a headset is active - no
// mouse). xr_session feeds XInput each frame via FeedGamepad and toggles open/closed on the Insert edge.
// Checkboxes/sliders bind DIRECTLY to MELEVR::Config::Get(), so edits are live; "Save" persists to ini.

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;
struct ID3D11Texture2D;

namespace MELEVR::Menu
{
// Create the ImGui context + the owned render-target texture on the game's D3D11 device. Idempotent.
void Init(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height) noexcept;
void Shutdown() noexcept;

bool IsOpen() noexcept;
// Live left-stick deflection magnitude (0..32767) from the real pad. Used to detect sprinting during a
// camera-mode transition blend (FP-storm head-look switch), before the CombatStorm mode name settles.
int  LeftStickMagnitude() noexcept;
void Toggle() noexcept;   // Insert edge
void SetOpen(bool open) noexcept;   // force open/closed (first-run welcome pop)

// Give the menu the game's top-level window so it can subclass it and swallow mouse+keyboard messages
// while open (so navigating the menu can't rotate/fire/move the game). Call once with the swapchain's HWND.
void SetGameWindow(void* hwnd) noexcept;

// Hook XInputGetState on the xinput DLLs the game already loaded, so the game sees a NEUTRAL pad while the
// menu is open. The menu reads the real pad via the original trampoline. Call once (MinHook must be init'd).
void InstallInputBlock() noexcept;

// Redundant now: RenderFrame self-polls input. Kept as a no-op so existing callers still link.
void FeedGamepad(unsigned short xinputButtons, float lx, float ly) noexcept;

// If open: NewFrame, build UI, render into the owned texture, return it. If closed: return nullptr.
ID3D11Texture2D* RenderFrame() noexcept;

// Flat/no-headset lab path: if open, render the same ImGui menu directly over the monitor backbuffer.
void RenderFlat(ID3D11RenderTargetView* target, int width, int height) noexcept;

int Width() noexcept;
int Height() noexcept;

// Returns true ONCE after the "Recenter now" button is clicked, then clears the flag.
bool ConsumeRecenterRequest() noexcept;

// Returns true ONCE after R3 is double-clicked on the gamepad, then clears the flag.
bool ConsumeRecenterCombo() noexcept;
}
