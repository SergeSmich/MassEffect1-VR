#pragma once

// PCHUD movie transform (Scaleform-only HUD move/scale).
//
// Drives the game's own PCHUD Scaleform movie via the engine API BioSFPanel.SetVariableFloat on the active
// BioSFHandler_PCHUD panel (_root._xscale / _yscale / _x / _y). This moves/scales the bottom/top HUD WITHOUT
// touching the crosshair/reticule. Ported from the proven M8NativeSplitXR process_event_hud_probe.
//
// Mechanism: hook the engine's UObject::ProcessEvent (vtable slot 0x250). On each event whose context is a
// BioSFHandler_PCHUD, read the panel pointer at +0x80 and remember it. When enabled and the panel/values
// change, call SetVariableFloat back through the original ProcessEvent to apply the transform. A thread-local
// reentrancy guard makes ProcessEventHook ignore its own SetVariableFloat calls. When disabled, does nothing.

namespace MELEVR::Pchud
{
// Install the ProcessEvent hook (idempotent). Safe to call before the game object table is ready; it simply
// fails quietly and can be retried. Prefer calling lazily from the first Tick().
void Install() noexcept;

// Store the desired transform. enabled=false means the feature is fully inert (no ProcessEvent calls).
// scaleX/scaleY are multipliers (1.0 = 100%); offsetX/offsetY are Scaleform _x/_y (movie pixels).
void SetTransform(bool enabled, float scaleX, float scaleY, float offsetX, float offsetY) noexcept;

// Same, for the conversation-wheel / dialog movie (BioSFHandler_Conversation). Independent enable + values;
// shares the one ProcessEvent hook. Lets the stretched-at-the-bottom cutscene UI be moved/scaled into view.
void SetConvoTransform(bool enabled, float scaleX, float scaleY, float offsetX, float offsetY) noexcept;

// Same, for Designer UI (BioSFHandler_DesignerUI) - the scripted/mission HUD overlay system (charge
// counters, countdown timers, boss-fight UI: e.g. the Saren fight's "N Charges Remaining" + countdown,
// discovered 2026-07-11). A THIRD, independent movie target on the same ProcessEvent hook. One set of
// values (no per-VR-mode split) - this is an occasional overlay, not a persistent per-mode HUD layout.
void SetDesignerUiTransform(bool enabled, float scaleX, float scaleY, float offsetX, float offsetY) noexcept;

// [LASERUI] mining-laser designer-UI layout: separate transform set, active only while the laser
// layout is latched (BioSeqAct_DUISetLaserLayout seen). Defaults = identity (vanilla placement).
void SetLaserUiTransform(bool enabled, float scaleX, float scaleY, float offsetX, float offsetY) noexcept;

// Force the combat/squad-bark subtitle render mode (UBioSubtitles). Not a Scaleform movie - the engine has
// no position field, only a preset enum: 1=Default, 2=Top, 3=Bottom, 4=Ambient. force=false leaves the game
// alone. Applied from Tick() via a crash-safe byte-write to the live, re-validated subtitle object.
// NOTE: proven a DEAD lever for position (mode change doesn't move the draw). Kept for diagnostics only.
void SetSubtitleMode(bool force, int mode) noexcept;

// Subtitle overlay redraw: hides the game's subtitle and redraws it on the HUD Canvas using the game's own
// DrawText path, so it stays visually close to native while giving reliable move/scale sliders.
void SetSubtitleRedraw(bool enabled, bool hideOriginal, float posXFrac, float posYFrac,
                       float scaleX, float scaleY) noexcept;

// Subtitle redraw font picker: index into the live-enumerated Font/MultiFont objects (-1 = canvas default).
// The Coalesced's SubtitleFontName (SmallFont) proved NOT to be the native subtitle's real font; the picker
// lets the user cycle live fonts until the look matches, then the winner gets hardcoded by name.
void SetSubtitleFontIndex(int index) noexcept;
int GetSubtitleFontCount() noexcept;
const char* GetSubtitleFontName(int index) noexcept;   // "" if out of range

// Native spoken-subtitle control: override the engine's real subtitle-region query (all FOUR
// GetSubtitleRegion floats: left/top/right/bottom as screen fractions) for the live UBioSubtitles draw,
// plus an experimental UBioSubtitles.m_FontSize write (fontSize 0 = leave the game's value alone).
// The GetSubtitleRegion hook also passively logs the game's REAL region per lane ([SUBREGION_REAL]).
void SetNativeSubtitleMove(bool enabled, float regionLeftX, float regionTopY, float regionRightX,
                           float regionBottomY, float fontSize) noexcept;

// ---- Per-element HUD/conversation-movie controls (2026-07-06). Element names extracted from the live
// SWF dumps (GUI_SF_HUD.PC_ME_HUD, GUI_SF_Conversation.Conversation): each is a named _root child, moved
// via the same guarded BioSFPanel.SetVariableFloat path as the whole-movie sliders. Offsets are RELATIVE
// to the element's authored position (baseline read once per panel via GetVariableFloat).
constexpr int kHudElemCount = 13;
constexpr int kConvoElemCount = 4;
extern const char* const kHudElemNames[kHudElemCount];       // "BottomUI", "radarMC", "targetMC", ...
extern const char* const kConvoElemNames[kConvoElemCount];   // "SubtitleConversation", "SubtitleTop", ...

struct HudElemState
{
    bool  on = false;      // apply this element's overrides
    bool  hide = false;    // _visible = 0
    float offX = 0.0f;     // added to the authored _x
    float offY = 0.0f;     // added to the authored _y
    float scaleX = 1.0f;   // _xscale/100
    float scaleY = 1.0f;   // _yscale/100
};

void SetHudElements(const HudElemState* elems, int count) noexcept;     // PC_ME_HUD movie children
void SetConvoElements(const HudElemState* elems, int count) noexcept;   // Conversation movie children

// Group controls (the user-facing surface): TOP UI (target name bar + corner clusters) and BOTTOM UI
// (health/radar/weapon/XP). Each group moves and scales AS A UNIT, anchored at top-center / bottom-center
// of the 1280x720 movie, and expands internally into per-element states. squadMC + vehiclePause are
// center-screen overlays and are deliberately left untouched by both groups.
void SetHudGroups(const HudElemState& top, const HudElemState& bottom) noexcept;

// Per-frame driver. Lazily installs on first call, then (if enabled + a panel is resolved + the panel or
// the values changed) applies the transform.
void Tick() noexcept;
}
