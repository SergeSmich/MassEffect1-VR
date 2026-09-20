# Mass Effect 1 VR

A 6DOF VR mod for Mass Effect Legendary Edition (ME1), built as a `dxgi.dll` proxy loaded next
to `MassEffect1.exe`. It hooks Direct3D 11 and the game's UE3 render path directly and submits
to any OpenXR runtime (SteamVR, Meta Quest Link, Virtual Desktop, etc.).

Experimental community mod. Not affiliated with BioWare or EA. Use at your own risk.

Licensed under [GPL-3.0](LICENSE).

## What it does

- **Real geometric stereo** - the engine renders the whole frame twice per present, one full
  primary render per eye, which avoids the engine's per-pixel mistreatment of secondary views
  (missing bloom/lighting in one eye). See [`docs/STEREO_FIX_DOCUMENTATION.md`](docs/STEREO_FIX_DOCUMENTATION.md).
  AER and DIBR alternate stereo modes are also available.
- **6DOF head tracking** - position and rotation from OpenXR drive the game's view, with the
  declared FOV to the compositor always matching what the engine actually rendered.
- **First-person camera** - per-camera-mode first-person offsets with weapon/body visibility
  handling.
- **Head aim** - weapon aim and the Mako turret follow the head.
- **Native subtitle and HUD handling** - subtitles are redrawn at a configurable position and
  scale, and HUD elements can be repositioned/resized in the in-headset menu.
- **In-headset menu** - Dear ImGui menu (Insert key) for every setting, with profile slots.


## Building

See [BUILD.md](BUILD.md). Short version: Visual Studio 2022 with the C++ workload, then
`scripts\build.bat`. All dependencies are vendored in `third_party/` - nothing else to install.

## Layout

| Path | What's there |
|---|---|
| `src/` | Mod source (the DXGI proxy, D3D11 capture, UE3 hooks, OpenXR layer, ImGui menu) and its project file |
| `scripts/` | `build.bat` and a deploy helper |
| `third_party/` | Vendored MinHook and Dear ImGui, used by the build |
| `docs/` | Technical notes |
| `release/` | The install/uninstall script and shipped package layout |
| `runtime/` | A starting `MELEVR.ini` |
| `builds/` | A prebuilt `dxgi.dll` |

## License

GPL-3.0 - see [LICENSE](LICENSE). Copyright (C) 2026 Halcyon.

Vendored dependencies keep their own licenses: MinHook (BSD-2-Clause) and Dear ImGui (MIT),
both under `third_party/` alongside their license files. Both are permissive licenses
compatible with GPL-3.0.
