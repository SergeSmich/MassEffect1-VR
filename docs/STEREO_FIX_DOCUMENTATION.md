# ME1 VR — How Stereo Was Fixed (Same-Frame Rendering)

This is the "what and why" of the stereo mode as it ships now: the bug that forced a rewrite, the
architecture that replaced it, and the four supporting fixes that made it actually playable.

---

## 1. What was broken (the old SBS stereo)

The original stereo mode ("Stereo 1", now phased out) rendered **side-by-side**: it made the game
draw two half-width views — left eye and right eye — into a single wide backbuffer (the "P1 layout"),
then submitted each half to its eye.

This wasn't a bug in the mod. **UE3-LE mistreats the *secondary* view of a multi-view family at the
per-pixel level.** The engine runs a class of once-per-frame work — deferred lighting resolve,
bloom, light shafts, depth-of-field, and other post-process — and its internal "has this run this
frame?" guards fire for the **first (left) view only**. The second (right) view is treated as
already-done and skipped. In the headset that showed up as:

- **Right eye missing bloom / light shafts / DoF** — the left eye glowed correctly, the right was flat.
- **Conversation characters rendering pure black in the right eye** — the deferred lighting pass
  never ran for the second view, so the characters had no lighting at all.

A full day of composite-level fixes tried to patch this *after* the fact — QPOST, FBCOPY, UBERFILL
(see the `me1-stereo-xoffset-root-cause` memory). Every one was **falsified**: feeding the compositor
byte-identical inputs for both halves still produced different per-half output. That was the proof
that the mistreatment is **engine policy applied during rendering, not a data problem** fixable
downstream. You cannot patch a secondary-view bug on the composite; you have to not have a secondary
view.

---

## 2. The fix — SFR (Same-Frame Rendering)

**Render the entire frame twice per present — two full, single-view, PRIMARY renders, one per eye —
instead of one split two-view render.**

Because each eye is a *primary* render, the engine's once-per-frame lighting/post guards run **fully
for each eye**. There is no secondary view for the engine to skip, so the entire bug family — right-eye
bloom loss, black conversation characters, right-eye DoF loss — **is impossible by construction**.
Each eye gets true mono-quality lighting and post.

This is the same existence proof AER relied on (AER also renders full primary frames), but SFR does
it **same-frame** — both eyes from the same instant — so there is no temporal gap and no alternate-eye
flicker.

It was confirmed sharp and comfortable in-headset. It shipped as "Stereo 2" and on 2026-07-15
became simply **Stereo** (the old SBS path is kept in the tree but is unreachable from the menu).

---

## 3. Architecture (where each piece lives)

Per present, when Stereo is active (gameplay, and optionally convo/cutscene):

1. **Game thread — the double render.** `HookFViewportClientDraw` (detour at `exe+0x4C71A0`,
   RUNTIME_FUNCTION-verified) runs **one extra** `FViewportClient::Draw` after the game's own, SEH-
   guarded (`SfrGuardedReplay`, 3-strike auto-disarm). The game thread only *enqueues* — it must never
   touch D3D here (doing so raced the render thread and killed the process; threadcheck proved the
   FVC-draw thread ≠ the present thread). `render_hook.cpp`.

2. **Per-pass camera offset.** In the CalcSceneView path, `ApplyReplayProofOffsetSEH` shifts the eye:
   pass 0 = −halfEye, replay pass = +halfEye (`t_fvcReplay` thread-local), value = `sfr2HalfEyeUU`
   (the tuned separation). Each Draw builds a fresh `FSceneView`, so the shift never persists into
   game state. `render_hook.cpp`.

3. **Render thread — pass capture.** `d3d_capture.cpp`, `[SFR-CAP]`. All copying is on the render
   thread, self-aligned by markers in the command stream:
   - **pass 0** = the backbuffer captured at the boundary between the two eye renders.
     *Gameplay boundary:* the first scene-sized depth clear after this present's first uber-composite
     draw (PS hash `0x9AA7F54A`).
     *Cine boundary:* the start of the 2nd non-UI backbuffer-target draw batch (`SfrCineNoteDraw`) —
     conversations render on a **deferred context** and never run the gameplay composite shader, so
     the gameplay marker never fires there; this was the two-lock frozen-convo bug.
   - **pass 1** = the backbuffer at Present, which publishes the finished stereo pair (`[PASSCAP]`).

4. **UI.** `[SFR-UI]`. Scaleform draws the HUD **once per present, after both passes**, so the
   boundary-captured pass 0 has no HUD. Every UI draw is mirrored into the held pass-0 texture
   (`SfrMirrorUiDrawIntoPass0`). **CRITICAL: the mirror binds the game's OWN depth-stencil view with
   the swapped color target** — Scaleform clips HUD elements with stencil masks, and a null DSV renders
   masked elements whole (that was the "left eye renders red" vignette bug). Never mirror a draw
   without the DSV.

5. **Submit.** `CopyStereoPassCaptureToEyes` (`[SYNCSTEREO]`): pass0→L, pass1→R via
   `CopyTextureToEyeFullFrame` (whole→whole, aspect-correct). `sfr2SwapEyes` flips the mapping.

6. **Gate.** `PublishGameModeForSfr(mode, allowConvo, allowCutscene)` — the double render runs for the
   engine's EGameModes byte 0..4 (gameplay) always, and 5/6 (convo/cutscene) per the VR-cinematics
   toggles. Menu 7 / movie 8 / galaxy 9 never (the main-menu-renders-a-3D-scene crash territory).

### Settled facts (do not re-derive)
- `FViewportClient::Draw` = `exe+0x4C71A0..0x4C9434` (contains the gameplay CalcSceneView call site).
- The 96-byte camera CB record: VP rows `f[0..15]`, pad row `f[16..19]=0,0,0,1`, camera **world
  position** `f[20..22]`, `f[23]=1`.
- `FSceneView` field `@0x320` stores the camera **translation inverse** (−position) — negate it.
- The game records world rendering on a **DEFERRED context**: any CB interception must cover the
  deferred Map/Unmap/UpdateSubresource, not just the immediate ones.

---

## 4. The four supporting fixes (SFR needs all of them to be shippable)

SFR renders **a full frame per eye**, which is a fundamentally different shape from SBS. That changed
the requirements around it:

1. **Square resolution.** SFR fills the whole backbuffer with ONE eye, so it needs a **square** render
   (~1:1, matching one eye), not the wide SBS resolution (which packs two eyes side-by-side). On the
   wide buffer, 3D geometry survived (the full-frame copy preserves NDC) but **screen-space 2D content
   — UI, HUD, conversation/cutscene overlays, menus — was authored at ~2.8:1 and shown in the ~square
   eye FOV = squashed to ~35% width**, and the short buffer was upscaled into the taller eye = blur.
   Fix: `sfr2ResX/sfr2ResY` (square) + an `sfr2Enabled` branch in `ApplyResolutionForCurrentMode`,
   mirroring AER/DIBR. Menu slider + installer ladder go to 10240 square. This is the same
   architecture AER/DIBR already used — SFR just wasn't wired into it at first.

2. **Adaptive frame pacing.** Two full renders per present, free-running against the headset refresh,
   drift in phase → a periodic beat = flicker on sustained head motion. The stereo pacer locks the
   present to a whole number of display periods (120Hz/60fps → 2). The lock is **adaptive**: it
   measures continuously, and when the game can't hold the lock under load it steps to a clean
   3-period cadence (a *regular* 40Hz reprojects smoothly; an *irregular* 2/3 mix flickers), then
   probes back. `StereoLockPeriods` / `PaceDisplayLockedAer` in `xr_session.cpp`.

3. **Reflection capture rate.** Floor reflections are `SceneCaptureReflectComponent` planar captures
   with a stock UE3 FrameRate throttle (60fps cap vs 60fps presents → the capture re-renders on only
   ~60% of frames). Every skipped frame shows a one-frame-stale reflection against a fresh view =
   ~30Hz judder on head turns, mode-independent. Fix: an incremental GObjects scan calls the game's
   own `SetFrameRate(1000)` on every reflect component so it re-renders every frame. `pchud.cpp`,
   `[REFLRATE]`. (Root-caused from a log-only evidence run after seven blind camera-math fixes failed —
   the capture matrix was never wrong, the *cadence* was.)

4. **VR cinematics (optional, experimental).** Per-context toggles let conversations and cutscenes
   render through SFR instead of a flat screen — again killing the black-character bug there by
   construction. Presented in a **fixed cinema window** (one constant 16:9 FOV declared to the
   compositor for every shot, because ME1 cuts lenses on every camera cut and declaring the per-shot
   FOV made the visible rectangle resize per cut). A lens-zoom slider magnifies inside that fixed
   window. Depth-of-field is forced off in `BIOEngine.ini` (`DepthOfField=False`) — it focuses on the
   speaker's face and looked out-of-focus when you turned your head.

---

## 5. Why this was cheap to make "the" stereo mode

Promoting SFR to be the one Stereo mode (2026-07-15) was a **4-file config/menu change, not a
refactor** — precisely because SFR was built as a complete *parallel* path (its own submit, UI
mirror, resolution, pacer, relief, cinematics). Nothing in the SBS render path had to move; the old
`stereoEnabled` flag is simply never set true again, so every SBS-gated code path stays inert while
the code remains in the tree.

---

## 6. The underlying lesson

The bug SFR fixes is an **engine-level policy** of UE3-LE (Mass Effect Legendary Edition's modified
UE3): the secondary view in a multi-view family is denied its own lighting/post pass. Any game on
this same engine that does side-by-side stereo will have the same bug, and the same
construction-proof fix (render each eye as a full primary render) applies.
