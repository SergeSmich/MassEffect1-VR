#pragma once

// MELE VR (clean rebuild) runtime config - Milestone 1.
//
// Minimal, single-source-of-truth struct for the tunables the in-headset menu exposes. Every field is a
// RUNTIME value (not a #define) so the Insert-menu sliders/checkboxes edit it live; "Save" persists it to
// MELEVR.ini next to the dll, "Reset" restores the struct defaults. No locking - read/written only on the
// render/Present thread.
//
// Read pattern: consumers call MELEVR::Config::Get() each frame (cheap, returns the live struct).

namespace MELEVR::Config
{
// First-person camera ownership + the per-state FpState eye-offset machinery were REMOVED 2026-07-03
// (strip-firstperson-livecode): they wrote live game objects every frame and hitched gameplay. The mod is
// now read-only on game state. Only render-side view/stereo tunables remain below.

struct VrConfig
{
    int   recenterKey      = 'R';   // VK code, rebindable in the menu
    bool  recenterKeyEnabled = true;// off = the recenter hotkey is disabled (that key behaves normally in-game)
    int   menuKey          = 0x2D;  // VK_INSERT: the menu-open key, rebindable (no disable: that would lock you out)
    bool  headLookEnabled  = true;  // HMD camera/view rotation (CalcSceneView)
    bool  invertLookYaw    = false;
    bool  invertLookPitch  = false;
    float lookSensitivity  = 1.0f;  // 0.25..2.0 scale on the head delta
    float headLookSmoothing = 0.4f; // low-pass on the head angle; kills the held-look TAA blur (0=raw/sharpest, ->1=smoother/laggier)
    // Pose-tag delay (THE anti-drag fix, 2026-07-12): SetHeadLook armed at Present N rotates the frame captured
    // at Present N+1, so the submitted image is rendered with the PREVIOUS present's head sample. Tag it with
    // that sample (one-present delay) instead of the current pose - "tag the pose it was RENDERED with". The
    // old current-pose tag claimed the image was a frame fresher than it was, so the compositor under-corrected
    // by one frame's head delta = the world dragged with the head at turn-start then settled (+ micro-shake on
    // small movements). AER always did this via per-eye history tags; this gives mono/stereo the same rule.
    bool  poseTagDelay = true;
    // [POSEEXACT 2026-08-11] Tag with the pose the render ACTUALLY consumed instead of guessing its
    // age via poseTagDelayFrames. The render stamps which head arm it latched; the submit looks that
    // arm up. In the gm=5 conversation pipeline this stays correct across resolution/framerate;
    // poseTagDelayFrames is only right where it was hand-dialled. The gm=6 scripted-cutscene
    // pipeline empirically needs that delayed model instead. Ring misses also fall back to delay.
    // [CINETAG-AUTO 2026-08-12] ME1 switches pipelines inside apparently continuous scenes:
    // exact is correct for gm=5 conversations, while the dialled delay is correct for gm=6
    // scripted cutscenes. On = route automatically by the latched engine context. Off = force
    // the old hand-tuned delayed behaviour everywhere, retained as an A/B override.
    bool  poseTagExact = true;
    // How many presents back the tag reaches (fractional, blended between stored samples). 1.0 = the assumed
    // arm->capture depth; reported "almost gone but still there in motion" at 1.0, which smells like the
    // real pipeline depth is deeper/fractional (DXGI render-ahead; 60fps game shown 2x per 120Hz cycle). Dial
    // in-headset until the world locks during motion; where it locks = the true depth. 0 = no delay (old bug).
    float poseTagDelayFrames = 2.0f;  // DEFAULT 2.0 2026-07-12 (in-headset dial-in): 1.0 left a motion
                                      // residual, 2.0 confirmed smooth - the real arm->capture pipeline depth.
    // Late-latch (2026-07-12, superseded same day): re-locate just before xrEndFrame and tag FRESHER. That's
    // the OPPOSITE correction to the render's one-frame arm lag, and made the shake slightly worse in-headset.
    // Kept as an A/B toggle for diagnostics; poseTagDelay wins when both are on. Default OFF.
    bool  lateLatchPose = false;
    bool  combatHeadTracking = true;  // weapon drawn -> HMD still drives the VR camera/view
    bool  combatHeadAim    = true;  // weapon drawn -> HMD also drives game aim/ControlRotation
    bool  invertAimYaw     = false;
    bool  invertAimPitch   = false;
    // Camera hold (EXPERIMENTAL, 2026-07-12, user request): weapon-out head aim normally moves the game
    // camera too (ControlRotation orbits the over-shoulder cam around Shepard = "world spins around him").
    // With this on, the head still drives ControlRotation (aim/body/fire follow the head) but the RENDER
    // view is counter-rotated by the same delta so the camera ORIENTATION holds still. NOTE: this holds the
    // look direction only, not the camera POSITION (which still arcs around Shepard) - render-side rotation
    // pivots at the camera, it can't un-orbit the boom. Step-1 experiment to see the reticle behaviour +
    // how the residual position arc feels; full position-hold is the follow-up if this has legs. Adv-only.
    bool  combatCamHold    = false;
    bool  combatCamHoldInvert = false;  // flip the position-cancel direction if the camera swings the wrong way
    float combatCamHoldGain = 1.5f;     // ~boom radius (m): how far to slide the camera per radian of head aim to
                                        // cancel the arc. Tuned in-headset (0 = no hold; too high = over-corrects).
    // Decoupled pitch (2026-07-11, user-requested x2): when on, the right stick's VERTICAL axis (sThumbRY)
    // is zeroed before the game reads it, so ONLY head movement tilts the view up/down; the stick still turns
    // (yaw) sideways. Fixes accidental stick-nudge pitch losing orientation. Zeroed at the XInput hook (vr_menu
    // .cpp), so it also parks the Mako cannon's stick-elevation - flagged in the menu. Off by default.
    bool  decoupledPitch   = false;
    // Decoupled yaw (sibling of decoupledPitch): zeroes the right stick's HORIZONTAL axis (sThumbRX), so ONLY
    // head movement turns you left/right. Head-only turning is limiting for seated players (can't spin), so
    // it leans on recenter (R) to re-forward you - which is why the recenter re-latch fix had to land first.
    // Off by default. Enabling both = full head-only aim (stick does neither pitch nor yaw).
    bool  decoupledYaw     = false;
    // [MOVEFIX] Move follows head (2026-07-16, the FP-storm inversion fix): whenever render-side head-LOOK
    // owns the view (unarmed, and sprints/storms after the storm aim-exclusion), the view is rotated away
    // from the game's movement basis (ControlRotation) - so running felt inverted: look left, the character
    // keeps its stick heading, which now reads as "moving right" in view. With this on, the XInput hook
    // rotates the LEFT stick vector by the exact applied head-look yaw (RenderHook::HeadLookYawUU), so the
    // character runs where the head points. No-op when head-aim owns the view (yaw=0) or in menus/cine/Mako.
    bool  moveFollowsHead  = true;
    // Mako head-aim (EXPERIMENTAL): head drives the cannon turret directly (its own appearance-object field,
    // not ControlRotation) while the chase camera stays put. Default OFF; live write-experiment, units TBD.
    bool  makoHeadAim      = true;   // Baked ON (cannon follows head)

    // First-run onboarding: false until the user dismisses the welcome / HDR-off notice once (then saved =1
    // so it never nags again). Fresh installs have no key -> defaults false -> the welcome shows once.
    bool  firstRunDone     = false;

    // ---- Conversations / cutscenes ---------------------------------------------------------------
    // REBUILT 2026-08-02 ON THE ME2/ME3 MODEL. Everything from the 2026-07-18 [CINEVR2] era is gone:
    // the camera-class-name detection, the cine-scoped fill pair, the per-context zoom pair, the
    // adaptive cine render scaler, and cine head tracking. They were ME1-only inventions and every
    // one of them produced a defect that then needed its own compensation on top.
    //
    // What ME2 and ME3 actually do, and what ME1 now does:
    //   - the engine mode byte decides the context, in BOTH directions ([CINELATCH]/[GAMEPLAYVR]);
    //   - a dead scene is never rendered in VR ([DEADVETO]);
    //   - the cine window IS the gameplay window, rendered AND declared ([VRCINE FILL]);
    //   - the scene renders at full gameplay resolution (neither mod has a cine scaler);
    //   - head tracking does NOT turn the camera during a cine: the render rotation is PARKED and
    //     the stereo pair is anchored, so a cine is a stable 3D picture you sit inside. This is
    //     ME2's lockedCine path, and it is now the ONLY cine behaviour. 2026-08-02, after
    //     the tracked version shipped: "REMOVE ... THE HEADTRACKING FOR VR CUTSCENES AND
    //     CONVERSATIONS". It also removes the whole pose-mismatch defect class by construction.
    // Both ON by default 2026-08-11: VR cine is the shipped experience now that the letterbox
    // constraint is cleared and cine head tracking is smooth. These were off while cine was broken.
    bool  cineVrConvo = true;          // conversations render in VR (off = flat panel)
    bool  cineVrCutscene = true;       // cutscenes render in VR (off = flat panel)
    float cineZoom = 1.0f;             // ME2's CineScreenZoom: render narrower, declare the window
    // [CINESEP] Cine eye-separation scale. Was a BAKED 0.5 (half gameplay disparity in every VR
    // convo/cutscene, no knob) on the theory that close-range face shots at full separation are an
    // eye workout. Measured against ME2 2026-08-09: ME2 applies NO cine separation scaling at all -
    // its cine renders at the same half-eye as gameplay - and halving ME1's is the reason ME1 cine
    // reads flatter than ME2's side by side. Now a slider, default 1.0 = ME2 parity. Scales the
    // render separation AND the convergence together so the fusion plane stays matched to the IPD.
    float cineSepScale = 1.0f;
    // ME2/ME3 parity: cine head tracking is a TOGGLE, default ON. ON = you look around inside the
    // shot. OFF = ME2's locked cine, the pair anchored to the current head = a stable 3D picture.
    // Tracked cine only behaves because of [FREEZETAG] below it; do not ship one without the other.
    bool  cineHeadTracking = true;
    // [CINEPACE60] pace staged cutscenes to display/2 (60fps at 120Hz). Cutscene cameras animate on
    // the game tick and were authored against the engine's own 60fps cap - uncapping the engine
    // (2026-08-12) made their CONTENT step at ~119fps, which read as head-tracking judder and was
    // immune to every tracking-side fix. Convos and gameplay are not paced by this. Ini-only knob.
    bool  cineFpsCap = true;
    // [WORLDYAW2 A/B 2026-08-12] cine head-yaw about world up read from the camera matrix. Shipped
    // hardcoded-on 08-12 and the broad scene-dependent shake regressed the same day ("it was fixed
    // per say" on the build before it): an ANIMATED director camera moves the derived axis every
    // frame, so the applied head-look sweeps against the tag in proportion to camera motion -
    // static shots immune, moving shots shake, one cutscene flips between the two. Default OFF =
    // the pre-WORLDYAW2 local-up axis. Ini-only A/B knob; [CINEJIT] logs wy= so the run states it.
    bool  cineWorldYaw = false;
    // Cine-scoped tag depth. poseTagDelayFrames (2.0) was dialled in-headset against GAMEPLAY's
    // pipeline. Cine's is measurably different (2026-08-02: pass-1 replay ~430us vs ~130us, and the
    // pass-0 snapshot taken ONCE at backbuffer batch 2 of ~10 instead of re-taken ~3.5x per
    // present), so the compositor is correcting against the wrong render-ahead depth and the error
    // scales with head speed = a lack of smoothness during head turns. Same dial-in procedure that
    // produced the gameplay 2.0: turn your head in a conversation until the world locks.
    float cinePoseTagDelayFrames = 2.0f;
    // Flat-screen widths: used by the default flat presentation, by Bink video (movies +
    // hybrid-cutscene segments), and by the head-tracking-OFF 3D picture above.
    float convoMonoQuadWidth = 2.35f;    // screen width for conversations (baked; rebaked 2026-08-11 from the live ini)
    float cutsceneMonoQuadWidth = 2.35f; // screen width for cutscenes / movies
    bool  movieFlatEnabled = true;  // prerendered movies / loading / main-menu (no 3D scene) -> flat mono (else stereo zooms the 2D video)
    float movieMonoQuadWidth = 2.35f; // head-locked mono screen width for movies
    // Depth of field: when ON, the mod writes DepthOfField=False into GamerSettings.ini [SystemSettings] at
    // boot (engine reads SystemSettings at load, so this takes effect on the NEXT game launch). Off = the
    // mod stops forcing it and writes DepthOfField=True so the engine default returns. See EnsureDepthOfFieldSetting.
    // DEFAULT OFF (= depth of field stays ON) 2026-08-12: tested in both games - forcing DoF
    // off makes the game lose colour and look washed out, in ME1 exactly as in ME2. The blur it removes
    // is not worth that. Users who want it off can still tick the box.
    bool  disableDof = false;
    // Full-screen menus (EGameModes 7 = GUI: pause/inventory/squad/journal/map/options) -> flat mono, every VR
    // mode. Detected via USFXGameModeManager::CurrentMode (menu-exclusive; never gameplay/convo/cutscene).
    bool  menuFlat = true;            // menus -> flat mono head-locked quad (readable) instead of stereo/AER/DIBR
    float menuMonoQuadWidth = 2.35f;  // head-locked mono screen width for menus
    // Galaxy map (EGameModes 9 = Galaxy). Default flat mono (readable), but some prefer it in VR -> when
    // galaxyFlat is off it renders in the active VR mode with a zoom knob (map reads too far otherwise).
    bool  galaxyFlat = true;          // galaxy map -> flat mono head-locked quad instead of stereo/AER/DIBR
    float galaxyMonoQuadWidth = 2.35f;// head-locked mono screen width for the galaxy map (when flat)
    float galaxyVrZoom = 1.0f;        // VR galaxy map: declared-FOV zoom (1 = off; >1 pulls the map closer, <1 pushes back)

    // ---- First person (M1, 2026-07-08: camera only, no hides) ----
    // Owns the ACTIVE SFXCameraMode's eye offset (0xB4, X=fwd Y=right Z=up from the pawn) + the game's native
    // bFirstPerson flag, for the four on-foot states only. (35,0,65) is the proven neutral FP eye from the old
    // build. Aim modes (TightAim/Sniper/HipAim), Mako, convo/cutscene stay third-person untouched.
    struct FpStateCfg { bool on; float fwd; float right; float up; };
    bool      fpEnabled      = false;                       // master toggle (off = zero writes, restore-once).
                                                            // STAYS OFF. Briefly defaulted ON on 2026-08-11 and reverted
                                                            // the same day: third person is the out-of-box experience,
                                                            // first person is opt-in via the menu or the K key.
    int       fpToggleKey    = 'K';                         // VK code to toggle first person, rebindable
    bool      fpToggleKeyEnabled = true;                    // off = the FP toggle hotkey is disabled (key behaves normally)
    // 2026-07-08 diagnosis: with bFirstPerson SET the game takes its own internal FP camera path and IGNORES
    // the mode offset (sliders dead). Default OFF: offsets alone drive the eye (the old build's working way).
    bool      fpNativeFlag   = false;                       // also set the game's native bFirstPerson bit
    bool      fpHideHead     = true;                        // hide Shepard's head/hair/visor meshes while FP is active
    float     fpHeadHideDelay = 20.0f;                      // frames to wait after ENTERING fp before hiding the head
                                                            // (lets the cover-exit camera blend finish first; anti-cull
                                                            // applies immediately, only the head hide is delayed)
    FpStateCfg fpExplore      { true, -50.1f,  0.0f, 25.1f }; // SFXCameraMode_Explore (baked default)
    FpStateCfg fpExploreStorm { true, -50.1f,  0.0f, 25.1f }; // SFXCameraMode_ExploreStorm (sprint)
    FpStateCfg fpCombat       { true, -70.0f, 19.9f, 21.0f }; // SFXCameraMode_Combat (weapon out) (rebaked 2026-07-11: lowered from 25.1, was too high)
    FpStateCfg fpCombatStorm  { true, -70.0f, 19.9f, 21.0f }; // SFXCameraMode_CombatStorm (combat sprint) (rebaked with fpCombat)
    FpStateCfg fpCombatAim     { true, -70.0f, 19.8f, 20.0f }; // SFXCameraMode_TightAim (ADS, not in cover)
    // ---- View framing (muVR-style) ----
    bool  cameraOffsetEnabled = false;  // master toggle for the camera up/down/left/right shift
    float cameraOffsetRight   = 0.0f;   // meters, + right / - left
    float cameraOffsetUp      = 0.0f;   // meters, + up / - down
    float flatScreenDistance  = 0.0f;   // meters, + pushes the view farther / - closer
    bool  screenDistanceEnabled = false;  // master toggle for the screen-distance dolly (baked OFF; can cull)
    bool  vrFovFillEnabled    = true;   // widen the game's rendered projection to match the headset view
    float vrFillH             = 1.0f;   // multiplier for the target headset horizontal half-FOV (gameplay)
    float vrFillV             = 1.0f;   // multiplier for the target headset vertical half-FOV (gameplay)
    // [INVMAT] head-locked dark-panel fix (2026-07-12, CONFIRMED same day): refresh the FSceneView
    // inverse / screen-to-world matrix slots after the CalcSceneView edits so fog/light-shaft
    // screen->world reconstruction matches the rendered camera. Root cause log-proven (inv(TVPM)@0x210
    // and inv(VPM)@0x2E0 go stale the moment head pitch is injected) and confirmed the panel
    // dies live with the toggle. Self-validating (only touches slots the runtime scan proved are pure
    // inverses), so default ON.
    bool  invMatrixFix        = true;

    // ---- Lean (positional 6DOF) ----
    bool  leanEnabled = true;   // physical head movement translates the viewpoint (6DOF freedom)
    float leanGain    = 1.0f;   // 0.5 = subtle .. 3.0 = exaggerated

    // ---- Same-frame stereo (P1 layout) = the primary VR mode ----
    // (AER was removed 2026-06-28: half-rate + cadence flicker, no GPU budget to save on this hardware.)
    // OLD SBS "Stereo" PHASED OUT 2026-07-15: Stereo 2 (SFR, sfr2Enabled) is now THE Stereo
    // mode. This flag defaults OFF and the mode picker never sets it again - the SBS render path stays
    // in the tree (not deleted) but is unreachable. vrMode 2 now maps to sfr2Enabled (see vr_config.cpp).
    bool  stereoEnabled = false;  // legacy SBS stereo; phased out - kept for code, never activated
    float stereoHalfEyeUU = 2.99f; // +/- render-side eye offset in UE units (baked default)
    bool  stereoSwapEyes = false; // flip left/right stereo sign if depth is inverted
    // Display-locked frame pacing for STEREO (2026-07-12): the game free-runs its internal ~60fps limiter,
    // unsynchronized with the headset refresh, so the 2:1 ratio on a 120Hz headset drifts in phase -> a
    // periodic 1-3-2-2 beat = flicker visible during sustained head motion (the left-right flicker report).
    // Same mechanism + same fix as AER's proven cadence lock: pace the present to display/2 so the phase
    // stops drifting. The game already self-caps at ~60, so this adds almost no wait - it aligns, not slows.
    // [MIRRORTHROTTLE] Present the flat mirror only 1 frame in N while the headset is driven (the frame
    // is already submitted to the compositor by then, so that Present is cosmetic). DEFAULT 1 = OFF,
    // present every frame. Added 2026-08-12 chasing a 60fps ceiling that turned out to be the ENGINE's
    // own SmoothFrameRate cap in BIOEngine.ini (MaxSmoothedFrameRate=60), not presentation
    // back-pressure - so this is inert by default and stays only as a lever for a real back-pressure
    // case (ME2 needed it and went 87 -> 119.7fps). Raising it makes the desktop mirror choppy.
    int   mirrorPresentEvery = 1;
    bool  stereoFramePacing = false; // DEFAULT OFF (2026-07-19 call: runs better with it off). Briefly
                                     // flipped ON on 08-11 to chase head-tracking shake and reverted on 08-12:
                                     // the shake was actually cured by scoping the pose-tag lanes (exact tag for
                                     // cine, dialled lock for gameplay), while the pacer's hold repeatedly read
                                     // as an fps cap. The 60fps ceiling behind all of it was the ENGINE's
                                     // SmoothFrameRate in BIOEngine.ini, nothing in here. Toggle stays.
    // [TEARING] Force DXGI_PRESENT_ALLOW_TEARING on the game's flat present so the present rate (and the XR
    // submit driven off it) is NOT bound to the desktop monitor's COMPOSITION rate. Without it, syncInterval=0
    // alone still caps at the monitor's Hz on a flip-model swapchain whenever the game omits the tearing flag
    // itself -> a hard 60 on a 60Hz monitor even with a 120Hz headset (log-proven 2026-07-18: fps=60 at present
    // flags=0x0 vs 85-95 at 0x200). Guarded in d3d_capture so it is only applied when the swapchain was created
    // with the tearing flag. Default ON; the only cost is cosmetic tearing on the flat MIRROR window - the
    // headset image is unaffected (it is fed from copied eye textures, not the flat present).
    bool  forceAllowTearing = true;
    // [LINKFOV] Meta-runtime (Quest Link / Air Link) FOV-quirk fix: Meta's PC compositor assumes its own
    // per-eye asymmetric FOV and does not honor the declared symmetric render window -> constant unfusable
    // double image in EVERY mode (log-proven 2026-07-19, [EYETAG]). When ON and the active runtime is
    // Meta's, each eye's submitted rect is cropped (tan-space) to the intersection with the runtime frustum
    // and exactly that is declared. Has zero effect on VDXR/SteamVR (runtime-gated in xr_session), so ON is
    // safe everywhere; the toggle exists as a kill switch if a Meta runtime update changes behaviour.
    bool  questFovMatch = true;
    // Per-eye FSceneView::State injection (the STEREOGHOST fix, render_hook.cpp TryP1LayoutStereo).
    // HISTORY: default-OFF'd 2026-07-12 as an emergency kill-switch when it was the suspect for the Virmire
    // head-locked panel - but that bug was later ROOT-CAUSED to stale FSceneView inverse slots (the INVMAT
    // fix, log-proven + hardened), NOT this. The kill-switch reason is stale.
    // ON by default 2026-07-13: with the SHARED state, UE3's once-per-frame view-state guards run for the
    // first (left) view only -> right eye missing bloom/light shafts (field reports, SBS
    // screenshot) and convo characters rendering pure black in VR. The regressing sessions all ran with
    // this OFF ([STEREOGHOST] absent from the log); the clean 07-12 sessions ran it ON. ME2 ships the same
    // per-eye state as its proven ghost fix. LoadFromIni force-migrates old inis' saved 0.
    bool  stereoPerEyeViewState = true;
    float stereoUiYShift = 0.0f;  // per-eye UI vertical align: +up/-down (fraction of eye height) so the crosshair meets shots
    // REMOVED 2026-07-14 (the black-character-hunt lever pile, all falsified in headset and stripped so
    // they can't destabilize future work): stereoGlowMirror, cineCharRemap (draw-replay repairs),
    // cineNullViewState (occlusion lever), cineKeepGameViewState (now FIXED POLICY: cine eye views always
    // keep the game's own view state - render_hook TryP1LayoutStereo), cineUnconstrainAspect (live
    // ACamera flag write; restored draws but not the visual, added a vFOV zoom regression). Stale ini
    // keys for these parse as unknown and are dropped on the next save.

    // ---- AER mode (alternate-eye rendering; re-ported 2026-06-29 from MGS2's display-locked model) ----
    // Renders ONE eye's shifted viewpoint per frame, holds the other, alternates. Submit-both-histories model
    // (one fresh + one ~1 frame old), separation baked into the RENDERED pixels (view-space camera-right shift),
    // both submitted eyes share ONE pose tag (latched basis, like stereo) so the orbiting camera doesn't shake.
    // Cadence is display-locked; full refresh alternates eyes on consecutive vsyncs, matching ME2/ME3 AERFULL.
    bool  aerEnabled    = false;  // VR-mode picker = AER (mutually exclusive with stereo/DIBR)
    float aerHalfEyeUU  = 3.13f;  // +/- render-side eye offset in UE units (rebaked 2026-08-11 from the tuned live ini; ~1.6 = 1:1 life-size)
    // [AERSHAKE bake 2026-08-21] FALSE. TRUE only ever compensated the single-slot stamp's
    // constant one-frame label offset (a fixed eye swap); the FIFO stamp removed that offset, so
    // TRUE would now invert depth. ME2 and ME3 are both field-confirmed FALSE after the same
    // change. Briefly set TRUE on 2026-08-22 and reverted the same day. Recovery knob if depth
    // ever reads inverted is the "Invert depth (swap eyes)" checkbox in the AER menu.
    bool  aerSwapEyes   = false;
    float aerFpEyeScale = 0.35f;  // first-person: scale the separation down (close geometry doubles otherwise)
    bool  aerFramePacing   = true; // display-locked cadence = THE flicker fix. Keep ON.
    bool  fullRefreshPacing = true; // AERFULL: 1 display period/present; off = stable legacy display/2 fallback
    int   aerFramePacingHz = 0;    // 0 = auto (runtime FB-refresh-rate, else measured); otherwise pin the headset Hz

    // ---- DIBR mode (depth-reprojection stereo; full framerate, edge artifacts) ----
    bool  dibrEnabled = false;    // VR-mode picker = DIBR; drives D3DCapture::SetDepthMapEnabled each frame (persisted)
    bool  sfr2Enabled = true;     // THE Stereo mode (SFR: same-frame double render; every eye is a full
                                  // primary render, so the per-eye bloom/lighting bug family cannot occur)
    float sfr2HalfEyeUU = 3.4f;   // Stereo 2 per-eye half separation (uu). Dialled-in value, 2026-08-12.
    bool  sfr2SwapEyes = true;    // Stereo 2: swap which pass goes to which eye (rebaked 2026-08-11 from the tuned live ini)
    float sfr2Convergence = 0.0120f;// Stereo 2 convergence. Dialled-in value, 2026-08-12. Opposite per-eye horizontal projection shift (off-axis
                                  // frustum) that pulls the fusion plane in from infinity so distant floating
                                  // markers/prompts fuse WITHOUT lowering IPD/world scale (0 = plane at infinity;
                                  // ME2 parity). Applied to proj[8] in the render hook, +pass0/-pass1.
    bool  dibrAutoConverge = true; // DIBR: auto-track convergence to the subject (screen-center) depth -> the thing you look at sits on the zero-disparity plane, doesn't warp, doesn't edge-ghost

    // ---- Resolution watchdog ----
    // MELE-VR.bat writes the resolution it installed here. The game will happily overwrite its own
    // GamerSettings.ini (it flipped BorderlessWindow off by itself on 2026-07-10, and touching the in-game
    // video options rewrites ResX/ResY to something the MONITOR supports - which is exactly the ceiling the
    // mod exists to step around). If the backbuffer comes up smaller than what the bat installed, the user's
    // resolution has been reset and their image is soft for a reason nothing on screen would explain.
    // 0 = the bat never wrote it (old install) -> watchdog silent.
    int expectedResX = 0;
    int expectedResY = 0;

    // ---- Supersample (ULocalPlayer::DynamicResolutionFraction @0x5C4, offset validated 2026-07-09) ----
    // The scene-render-resolution lever that does NOT depend on what the monitor reports. The game clamps its
    // backbuffer to the display mode (proven: asked 5120x2880, got 3840x2160), so ini resolution alone can
    // never sharpen a monitor-capped system. This fraction scales the INTERNAL scene render size.
    // 1.0 = untouched (the field is not written at all). >1.0 = supersample; whether LE1 honours >1 is
    // exactly what this build measures. Clamped to [0.5, 3.0] before any write.
    float dynResFraction = 1.0f;

    // ---- Relief warp ----
    bool  reliefProbe = false;    // M0 dev probe: enable the depth capture in Stereo/AER purely to MEASURE that
                                  // it works there (no warp, no visual change unless the greyscale viz is on)
    // M1: the actual depth-pop warp (stereo). reliefEnabled = the clean A/B toggle (OFF = plain stereo,
    // byte-identical, regardless of strength) so you can flip it on/off without zeroing your tuned strength.
    bool  reliefEnabled = false;
    int   depthPopKey = 'P';          // VK code to toggle depth pop (reliefEnabled) at runtime, rebindable. Depth
                                      // pop boots OFF every launch, so this is the opt-in-per-session hotkey.
    bool  depthPopKeyEnabled = true;  // off = the depth-pop toggle hotkey is disabled (key behaves normally)
    // STEREO's depth-pop tuning. Stereo warps each SBS half once; AER warps a whole frame per eye with the
    // opposite sign, so the same numbers do not produce the same pop. They were shared until 2026-07-10 and
    // that forced a compromise - the tuned AER values want ~2x the strength and ~2x the edge guard.
    float reliefStrength = 0.0286f;   // per-eye UV-shift scale; the "Depth pop" slider (baked default)
    float reliefEdgeGuard = 0.5f;     // 0 = raw gather (most pop, most ghost); 1 = strong silhouette damp (less ghost)
    float reliefNearFreeze = 1.02f;   // freeze the warp for depth nearer than this (protects Shepard from ghosting); >=1.0 = off
    bool  reliefVizManual = false;    // greyscale depth map: manual near/far/gamma window instead of auto-fit (to read the depth range)
    bool  reliefAutoConverge = true;  // track the subject depth as the zero-pop plane (shares the DIBR probe)
    float reliefConvergence = 0.985f; // manual zero-pop depth when auto is off
    bool  reliefFlip = false;         // flip sign if the pop sinks in instead of popping out
    float reliefCurve = 1.0f;         // depth response shaping (1 = linear; >1 emphasises near)

    // AER's own depth-pop tuning (defaults = the 2026-07-10 in-headset AER pass, tuned + confirmed at
    // the Sharp resolution tier - see the Max/flicker warning at reliefStrengthAer's menu slider). The menu
    // edits whichever set matches the ACTIVE mode, so the sliders always mean what you are looking at.
    float reliefStrengthAer = 0.0252f;
    float reliefEdgeGuardAer = 0.35f;
    float reliefNearFreezeAer = 1.0057f;
    bool  reliefAutoConvergeAer = true;
    float reliefConvergenceAer = 0.9849f;
    bool  reliefFlipAer = true;
    float reliefCurveAer = 0.96f;

    // [DEPTH DARKEN] (Luft et al. 2006): soft-darken the
    // BACKGROUND side of every depth edge - a monocular "solidity" cue on top of the stereo depth. It is a
    // color op applied identically to both eyes (no disparity spent, cannot ghost, cannot shrink the world),
    // so ONE shared set serves Stereo/AER/Stereo 2 - unlike the warp values above, which differ per mode.
    // Rides the relief pass: active when reliefEnabled and this strength > 0, even at pop strength 0.
    float reliefDarkStrength = 0.40f; // baked default 2026-07-15, tuned in-headset:
                                      // depth pop ON should look good immediately, no slider safari. 0 = off.
    float reliefDarkRadius   = 8.0f;  // depth-unsharp neighborhood radius in pixels (bigger = wider, softer shadow)

    // [UNSHARP POP] (research doc sec 2.3, Didyk 2011 disparity model): LOCAL depth-contrast pop - adds
    // disparity proportional to (pixel depth - neighborhood average) instead of distance-from-convergence.
    // The literature's "pop without shrink" operator: zero-mean, so the global disparity range (= world
    // scale) barely moves while local relief is amplified. Shares the base warp's freeze/edge/seam guards.
    // Shared across modes like the darkening (one more term inside the same warp).
    float reliefUnsharpStrength = 0.02f;  // UV-shift per raw-depth-delta, same units as reliefStrength; 0 = off
    float reliefUnsharpRadius   = 24.0f;  // neighborhood radius in pixels (wider = broader relief features pop)

    // ---- Per-mode render resolution (the "setres per mode" fix) ----
    // Written to the game's GamerSettings.ini (ResX/ResY) when a mode is picked; the game applies it on the NEXT
    // launch. AER/DIBR render the whole frame into ONE eye -> want ~square (match the eye, no stretch). Stereo
    // splits the frame side-by-side -> each half must land ~eye-aspect, so it wants WIDE. Mono = normal flat.
    // This is why one resolution can't serve both; the resolution follows the mode instead of a runtime switch.
    bool applyModeResolution = true;   // master: write the matching res to GamerSettings.ini on mode pick
    int  aerResX = 3072;    int aerResY = 3072;
    int  dibrResX = 3072;   int dibrResY = 3072;
    int  stereoResX = 3840; int stereoResY = 2160;
    int  monoResX = 3840;   int monoResY = 2160;
    // Stereo 2 (SFR) renders a FULL frame per eye, and the target must be NEAR-SQUARE because the
    // headset's per-eye window is (measured declared aspect 0.963). A 16:9 target was tried on
    // 2026-08-09 to fix cine letterboxing and CONFIRMED WRONG the same day, twice: the wide buffer
    // vs square declared FOV makes the whole frame anamorphic - the world survives (FOV-governed)
    // but the UI arrives 1.85x too tall, and both attempted compensations failed (viewport squash
    // moved world-anchored icons off their objects; widening the declared FOV exceeded the
    // runtime's asymmetric per-eye frustum = double vision everywhere). LE1's cine letterbox is
    // fixed at the SOURCE instead: [NOLETTERBOX] clears the cine camera's 16:9 aspect constraint
    // pre-Draw on the game thread, so cine renders full-frame like gameplay - the LE2/LE3 shape.
    // NEAR-square, not square: the height is width/0.964 (the measured per-eye window), because an
    // exactly square target against a 0.964 window still makes every UI element 3.7% too tall.
    // Corrected 2026-08-11 to match the MELE-VR.bat ladder, which had already moved to width/0.964
    // while this default was still 1:1 - a mode switch in the menu writes GamerSettings from THIS
    // value, so leaving it square would quietly push the tall-UI bug back onto a tuned install.
    // Balanced (4096) as the default - 2026-08-12 call: native renders 1:1 into the eye
    // texture, so nothing downsamples and the game's own shimmer shows through; 4096 is the
    // cheapest rung that supersamples, and the downsample IS the anti-aliasing. Ladder goes to 6144.
    // Ratios above ~2x the headset eye texture are handled by [DOWNCHAIN] in xr_session.cpp, which
    // halves the frame until it is within one bilinear step of the eye before the fill blit. Without
    // it, a >2x minification skipped most source pixels (10240 into a 3072-wide eye = 3.3x, ~11
    // source pixels needed per output pixel, 4 actually read) and looked like compression.
    int  sfr2ResX = 4096;   int sfr2ResY = 4250;

    // ---- Enhancements > Depth map (greyscale viz tuning; foundation for AER+depth combine) ----
    // LE1 is reversed-Z: near=high depth, far=low. The window remaps [far..near] -> black..white so the
    // tiny raw range becomes a readable gradient. Tune with the live readout (center=subject, corners=walls).
    float depthMapNear  = 1.000f;  // depth value that maps to white (near). Drag down toward the readout's center value.
    float depthMapFar   = 0.950f;  // depth value that maps to black (far). Drag up toward the readout's corner value.
    bool  depthMapFlip  = false;   // invert near/far if the map reads backwards
    float depthMapGamma = 1.0f;    // <1 lifts far detail, >1 emphasises near

    // ---- Depth-warp effect (the actual stereo the depth map drives, via the grid warp) ----
    float depthWarpGain  = 0.98f;  // depth strength (disparity per unit depth) (baked default)
    float depthWarpConv  = 0.9042f;// depth that fuses at zero disparity (the screen plane) (baked default)
    bool  depthWarpFlip  = false;  // flip depth direction (sign)

    // ---- PCHUD movie (Scaleform HUD move/scale via BioSFPanel.SetVariableFloat) ----
    // Drives the game's own PCHUD Scaleform movie (_root._xscale/_yscale/_x/_y). Moves the bottom/top HUD
    // WITHOUT touching the crosshair/reticule. Off by default; when off the feature makes no engine calls.
    bool  pchudEnabled = true;     // master toggle (tuned default; Reset restores the set below)
    float pchudScaleX  = 0.35f;    // _root._xscale multiplier (1.0 = 100%)
    float pchudScaleY  = 0.30f;    // _root._yscale multiplier (baked default; 1.0 = 100%)
    float pchudOffsetX = 400.0f;   // _root._x (movie pixels)
    float pchudOffsetY = 259.0f;   // _root._y (movie pixels) (baked default)
    // STEREO variant: used when stereo mode is active (UI is drawn per-eye at half width, so the HUD
    // wants its own layout). Same defaults as the normal set above until the user tunes stereo separately.
    float pchudScaleXStereo  = 0.35f;
    float pchudScaleYStereo  = 0.45f;
    float pchudOffsetXStereo = 400.0f;
    float pchudOffsetYStereo = 358.0f;
    float pchudScaleXDibr    = 0.35f;   // DIBR gets its own HUD layout (independent of AER/Mono's normal set)
    float pchudScaleYDibr    = 0.45f;
    float pchudOffsetXDibr   = 400.0f;
    float pchudOffsetYDibr   = 358.0f;

    // ---- Conversation wheel / cutscene dialog movie (BioSFHandler_Conversation) ----
    // Same move/scale mechanism as PCHUD, on the dialog/conversation-wheel Scaleform movie. Defaults are
    // Tuned values (rebaked 2026-07-11: text was too small; scale up, offsets follow).
    bool  convoEnabled = true;     // master toggle (tuned default; on)
    float convoScaleX  = 0.50f;    // _root._xscale multiplier (1.0 = 100%)
    float convoScaleY  = 0.50f;    // _root._yscale multiplier
    float convoOffsetX = 320.0f;   // _root._x (movie pixels)
    float convoOffsetY = 270.0f;   // _root._y (movie pixels)
    // STEREO variant (used when stereo mode is active) - rebaked with the above 2026-07-11.
    float convoScaleXStereo  = 0.50f;
    float convoScaleYStereo  = 0.65f;
    float convoOffsetXStereo = 259.0f;
    float convoOffsetYStereo = 229.0f;
    float convoScaleXDibr    = 0.35f;   // DIBR convo-wheel layout
    float convoScaleYDibr    = 0.50f;
    float convoOffsetXDibr   = 390.0f;
    float convoOffsetYDibr   = 270.0f;

    // ---- Designer UI (BioSFHandler_DesignerUI) ----
    // Scripted/mission HUD overlay system: charge counters, countdown timers, boss-fight UI (e.g. the Saren
    // fight's "N Charges Remaining" + countdown, discovered 2026-07-11 - its Kismet sequence is literally
    // named "SarenMonster_UIElements"/"CountdownTimer"). Same move/scale mechanism as PCHUD/Convo, but its
    // own independent movie - NOT reachable via kHudElemNames (those are the PERSISTENT HUD movie only).
    // One set of values (no per-VR-mode split): this is an occasional overlay, not a persistent layout.
    // Rebaked 2026-07-11 (mid-Saren-fight): the charge-timer panel sat off to the side/edge by
    // default; tuned offset pulls it back into a comfortable view. Scale untouched (1.0 was already right).
    bool  designerUiEnabled = true;
    float designerUiScaleX  = 1.0f;
    float designerUiScaleY  = 1.0f;
    float designerUiOffsetX = -358.0f;   // rebaked 2026-08-11 from the tuned live ini (was -224/-200)
    float designerUiOffsetY = -288.0f;   // baked 2026-07-17: tuned so the mission timer/charge
                                         // counter sits on-screen in Stereo (was -143 = off the top edge)
    // [LASERUI 2026-07-18] Mining-laser designer-UI layout (Therum): SEPARATE transform set, active only
    // while the laser layout is latched. Defaults = identity (the game's own placement) because the
    // timer-tuned master offsets above are what displaced the laser UI off-view in the first place.
    bool  laserUiEnabled = true;   // baked 2026-07-18 (tuned at the Therum laser: 0.8 scale, +129/+86)
    float laserUiScaleX  = 0.8f;
    float laserUiScaleY  = 0.8f;   // log recorded scaleX only; assumed uniform with X
    float laserUiOffsetX = 129.0f;
    float laserUiOffsetY = 86.0f;

    // ---- Combat/squad-bark subtitles (UBioSubtitles render mode; NOT a movie, preset positions only) ----
    // Engine positions are an enum: 1=Default, 2=Top, 3=Bottom, 4=Ambient. Combat barks default to Top (out
    // of view in VR). subtitleForce=false leaves the game untouched.
    bool subtitleForce = false;    // force the render-mode enum each frame (re-testing 2026-07-06: every
                                   // earlier "dead lever" verdict ran with the overlay hiding the native draw)
    int  subtitleMode  = 3;        // ESubtitlesRenderMode (1..4); 3 = Bottom

    // Subtitle overlay redraw: hide the game's subtitle + redraw it on the HUD Canvas at the configured pos/scale.
    // This is the supported fallback when the native spoken-subtitle path is not reliably hookable.
    bool  subtitleRedraw       = true;   // master toggle
    bool  subtitleHideOriginal = true;   // blank the game's original so only the redraw shows
    float subtitlePosXFrac     = 0.50f;  // CENTER X as a fraction of screen width (text centers on this)
    float subtitlePosYFrac     = 0.65f;  // Y as a fraction of screen height (0..1)
    int   subtitleFontIndex    = -2;     // -2 = matched native font (EngineFonts.MediumFont); -1 = plain; 0..N picker
    float subtitleScaleX       = 2.25f;  // Canvas.DrawText X scale
    float subtitleScaleY       = 2.25f;  // Canvas.DrawText Y scale
    // STEREO variant (same defaults as above; used when stereo mode is active).
    float subtitlePosXFracStereo = 0.50f;
    float subtitlePosYFracStereo = 0.65f;
    float subtitleScaleXStereo   = 2.25f;
    float subtitleScaleYStereo   = 2.25f;
    float subtitlePosXFracDibr   = 0.50f;   // DIBR subtitle layout
    float subtitlePosYFracDibr   = 0.65f;
    float subtitleScaleXDibr     = 2.25f;
    float subtitleScaleYDibr     = 2.25f;

    // Native spoken-subtitle region control: override ALL FOUR floats of the engine's GetSubtitleRegion
    // (two FVector2Ds; the old build only wrote MinPos and mislabeled the axes). Applies to the game's REAL
    // subtitle draw. Font size is a separate experimental UBioSubtitles.m_FontSize write (0 = don't touch).
    bool  nativeSubtitleEnabled  = false;
    float nativeSubtitlePosXFrac = 0.0f;  // region LEFT X as a fraction of screen width
    float nativeSubtitlePosYFrac = 0.82f; // region TOP Y as a fraction of screen height
    float nativeSubtitleScaleX   = 1.0f;  // region RIGHT X as a fraction of screen width
    float nativeSubtitleScaleY   = 1.0f;  // region BOTTOM Y as a fraction of screen height
    float nativeSubtitleFontSize = 0.0f;  // experimental m_FontSize write; 0 = leave the game's value

    // ---- Per-element HUD / conversation-movie overrides (element names from the live SWF dumps).
    // Offsets are relative to the element's authored position; scale 1.0 = authored size.
    struct ElemOverride
    {
        bool  on = false;
        bool  hide = false;
        float offX = 0.0f;
        float offY = 0.0f;
        float scaleX = 1.0f;
        float scaleY = 1.0f;
    };
    static constexpr int kHudElemCount = 13;    // must match MELEVR::Pchud::kHudElemCount
    static constexpr int kConvoElemCount = 4;   // must match MELEVR::Pchud::kConvoElemCount
    // Tuned gameplay HUD layout, BAKED AS DEFAULT 2026-08-11 (was all-identity). These are the
    // values the live MELEVR.ini carried after tuning in-headset, so Reset and a fresh install now both
    // land the tuned layout. Order must match MELEVR::Pchud::kHudElemNames exactly.
    // NOTE this is the set Stereo 2 uses: PushElementConfigs keys the *Stereo array off the LEGACY
    // stereoEnabled flag, which sfr2 leaves false - so the "normal" array below is the live one.
    ElemOverride hudElems[kHudElemCount] = {
        { false, false,    0.0f,   -2.0f, 1.00f, 1.00f },   // BottomUI (override off; offset as tuned)
        { true,  false,    0.0f,  210.0f, 2.00f, 2.00f },   // radarMC
        { true,  false, -109.0f,    0.0f, 1.50f, 1.50f },   // targetMC
        {},                                                 // topLeft
        {},                                                 // topRight
        {},                                                 // leftUI
        {},                                                 // rightUI
        { true,  false, -503.0f, -350.0f, 1.50f, 1.50f },   // squadMC
        {},                                                 // weaponAbilityTab
        {},                                                 // EventHolder
        { true,  false,  -75.0f,  110.0f, 1.75f, 1.75f },   // TeamBG
        { true,  false,    0.0f,  299.0f, 2.00f, 2.00f },   // EquipBG
        {},                                                 // vehiclePause
    };
    ElemOverride hudElemsStereo[kHudElemCount] = {};// STEREO variant (used when stereo mode is active)
    ElemOverride hudElemsDibr[kHudElemCount] = {};  // DIBR variant (used when DIBR mode is active)
    ElemOverride convoElems[kConvoElemCount] = {};  // backend only (subtitle work); no UI

    // Group controls (the user-facing HUD surface): TOP UI + BOTTOM UI, each moved/scaled as a unit.
    ElemOverride hudGroupTop = {};
    ElemOverride hudGroupBottom = {};

    // ---- Menu comfort ----
    float menuDistanceM = 1.54f;   // head-locked menu quad distance in metres (rebaked 2026-08-11; further = less disparity, easier to fuse)
    float menuSizeM = 1.0f;        // menu quad width in metres (resizable regardless of screen/res settings)
    float menuOffsetXM  = 0.0f;    // head-locked menu quad horizontal offset in metres (+ right / - left)
    float menuOffsetYM  = 0.0f;    // head-locked menu quad vertical offset in metres (+ up / - down)
};

// Depth-pop tuning for the ACTIVE mode. Stereo and AER warp differently and keep separate values; every
// consumer should go through these rather than reaching for c.reliefStrength directly and silently getting
// the Stereo number while running AER.
struct ReliefParams
{
    float strength;
    float edgeGuard;
    float nearFreeze;
    float convergence;
    float curve;
    bool  autoConverge;
    bool  flip;
    float darkStrength;   // [DEPTH DARKEN] shared across modes (color op, not a warp)
    float darkRadius;
    float unsharpStrength; // [UNSHARP POP] shared across modes (local depth-contrast warp term)
    float unsharpRadius;
};
ReliefParams ActiveRelief(const VrConfig& c) noexcept;

// The headset's per-eye WINDOW aspect (width/height), measured 0.963-0.964 on this headset class and
// headset-verified 2026-08-09 for UI proportions. The Stereo 2 render target must match it: exactly
// square makes every UI element ~4% too tall, 16:9 makes it 1.85x too tall. One definition so the
// number cannot drift between the installer ladder, the menu slider and the baked defaults.
constexpr float kEyeWindowAspect = 0.964f;
// Aspect-correct height for a given render width, rounded to an even number (drivers dislike odd).
inline int EyeHeightForWidth(int width) noexcept
{
    if (width < 2) return 2;
    int h = static_cast<int>(static_cast<float>(width) / kEyeWindowAspect + 0.5f);
    if (h & 1) ++h;
    return h;
}

VrConfig& Get() noexcept;          // the one live instance (the active profile's values)
void ResetDefaults() noexcept;     // restore the struct defaults (in-memory)
void LoadFromIni() noexcept;       // read MELEVR.ini at boot (loads the [active] profile)
void SaveToIni() noexcept;         // write MELEVR.ini (menu Save button) - persists the active profile

// ---- Profiles (fixed slots, no in-headset text entry) ----
// Slot 0 = "Default", 1..3 = "Custom 1".."Custom 3". The ini keeps one section per slot plus an
// [active] line naming the live one; Get() always returns the live (active) VrConfig.
int         ProfileCount() noexcept;             // 4
const char* ProfileName(int index) noexcept;     // slot label, or "?" if out of range
int         ActiveProfileIndex() noexcept;       // which slot Get() currently reflects
void        LoadProfile(int index) noexcept;     // apply that slot's saved settings to Get() + mark active + persist
void        SaveProfile(int index) noexcept;     // write Get() into that slot + persist the ini

// ---- Profile hotkeys (GLOBAL, not per-profile) ----
// One VK code per slot; pressing it live-loads that profile (same as clicking its menu button). Stored
// outside the per-slot VrConfig (a global [hotkeys] ini section) so the map is the SAME whichever profile
// is active - a per-profile map would change under you the moment you switched. 0 = unmapped.
int   ProfileHotkey(int index) noexcept;         // VK code for slot, or 0 if none / out of range
int&  ProfileHotkeyRef(int index) noexcept;      // menu binding target (KeyRebindRow / reset); dummy if OOR
bool  ProfileHotkeysEnabled() noexcept;          // master gate
bool& ProfileHotkeysEnabledRef() noexcept;       // menu checkbox target

// "setres per mode": write ResX/ResY into the game's GamerSettings.ini so the next launch renders at the aspect
// the chosen VR mode needs (square for AER/DIBR, wide for stereo). Returns true if the file was updated.
// Call it when the mode is picked; the game applies it on restart (no live swapchain teardown = no crash risk).
bool WriteGameResolution(int resX, int resY) noexcept;

// Convenience: resolve + write the resolution that matches the config's currently-selected mode. No-op if
// applyModeResolution is false. Returns the (resX,resY) written via the out params (0,0 if it did nothing).
void ApplyResolutionForCurrentMode(int* outResX, int* outResY) noexcept;
}


