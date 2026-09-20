# Building MELE VR

## Prerequisites

- Windows 10/11, x64.
- Visual Studio 2022 (Community, Professional, Enterprise, or the standalone Build Tools)
  with the **"Desktop development with C++"** workload installed. `build.bat` locates it
  automatically via `vswhere.exe`. The project does not pin a Windows SDK version; whichever
  is installed is used.
- No other tools or package managers required. MinHook and Dear ImGui are vendored in
  `third_party/` - nothing to fetch or install separately.

## Build

```
scripts\build.bat
```

Output: `builds\dxgi.dll`.

The build runs MSBuild on `src\MELEVRClean.vcxproj` (Release|x64) with the mod's own sources
plus the vendored MinHook and Dear ImGui sources, statically linked (`/MT`), so the resulting DLL
needs no separate vcruntime redistributable in the game folder. You can also open the project in
Visual Studio directly.

## Deploying

Copy these files into `<Steam library>\steamapps\common\Mass Effect Legendary Edition\Game\ME1\Binaries\Win64\`,
next to `MassEffect1.exe`:

- `builds\dxgi.dll` (just built)
- `openxr_loader.dll` - an x64 build of the official [Khronos OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK), not built by this repo (a copy is in `release/`)
- `MELEVR.ini` (see `runtime/MELEVR.ini` for a starting point)

See `release/README.txt` for the installer script and end-user install steps.

## Third-party components

| Component | Location | License |
|---|---|---|
| MinHook | `third_party/minhook` | BSD 2-Clause (see its `LICENSE.txt`) |
| Dear ImGui | `third_party/imgui` | MIT (see its `LICENSE.txt`) |
| OpenXR loader | `release/openxr_loader.dll`, obtained separately | Apache 2.0 |
