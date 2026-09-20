# BladeVR

*[Leer en español](README.es.md)*

A virtual reality mod for **Severance: Blade of Darkness** (the 2021 DirectX 11 remaster on Steam).
It adds **full 3D stereo** (a separate image for each eye) and a **6DOF head-tracked camera**
through SteamVR / OpenVR. Developed and tested with a Meta Quest 2 over SteamVR.

The game files are not modified. The mod is a DLL that is loaded when `Blade.exe` starts
(through a `dxgi.dll` proxy) and hooks DirectX 11 plus a handful of engine functions.

> **Disclaimer.** This mod was written entirely with the help of artificial
> intelligence, directed, supervised and tested by its author over many stages and
> iterations: every change was tried in the game and on the
> headset, and what did not work was discarded. It is a hobby project with no
> affiliation to the game's developers or publisher. Use it at your own risk.

## What works

* **Real stereo 3D.** Every 3D draw call is rendered twice, once per eye, with the headset's
  own per-eye projection. The world, characters, weapons, shadows and the sky all have correct
  depth. Menus and the HUD are shown on a comfortable virtual screen.
* **6DOF head tracking.** The camera follows the rotation *and* position of the headset,
  anchored to the game's own camera, in both first and third person. Leaning into walls or
  objects is prevented using the engine's own collision.
* **"Walk where I look" mode.** Optional: the character turns to follow the headset so you
  walk in the direction you are looking.
* The monitor shows the left eye centred (with black bars at the sides) instead of the
  split image; SteamVR's own "VR View" window is also available.
* Played with **keyboard and mouse or a gamepad**, exactly as the original game. VR
  controllers are not used.
* Works from the main menu; no injector or launcher needed.

## Requirements

* Blade of Darkness (2021 remaster, Steam, 64-bit).
* SteamVR and a compatible headset (tested with Quest 2 via Link / Air Link).
* Windows 10/11 x64.

## Installation

Copy these three files into the game's `bin\bin` folder (the one that contains `Blade.exe`,
usually `...\steamapps\common\Blade of Darkness\bin\bin`):

* `dxgi.dll` — proxy that loads the mod
* `BladeVR.dll` — the mod
* `openvr_api.dll` — OpenVR runtime (looked up next to `BladeVR.dll`)

Start SteamVR first, then the game. If SteamVR is not running or no headset is connected,
the game runs normally without VR. To uninstall, delete the three files.

## Recommended game settings

* **Resolution: the maximum your GPU allows** (e.g. 2560x1440). Each eye gets half of the
  screen width, so the higher the resolution, the sharper the image in the headset.
* **Field of view: 145**. The engine culls geometry on the CPU to the game's FOV; a low FOV
  leaves black areas at the edges of the headset's view.
* **Enable the frame-rate limit** in the game options. Without it the camera moves in
  jerks instead of smoothly.

## Keys

| Key | Function |
|---|---|
| **F5** | Toggle stereo on/off (it turns itself on when the menu appears). |
| **Page Up / Page Down** | Eye separation +4 / −4 game units (default 54). Adjust until the world feels the right size. |
| **End** | Size of the virtual screen used for menus and HUD (53° / 75° / 90°). |
| **Insert** | Head tracking on (default) / off. Turning it on re-centres the view. |
| **Space** | Re-centre position: the camera goes back to the character's eyes. |
| **Delete** | Toggle between *free look* (the character does not turn with your head) and *walk where I look* (the character turns to follow the headset). |

## How it works

**Loading.** Windows searches the executable's folder before System32, so the game loads our
`dxgi.dll`. It forwards every export to the real system DXGI (loaded by absolute path) and
loads `BladeVR.dll` from its `DllMain`. It also intercepts `CreateDXGIFactory*` to hand the
factory to the mod, which hooks `IDXGIFactory::CreateSwapChain` and learns the game's D3D11
device before the first `Present`.

**Stereo** (`StereoHook.cpp`). The engine transforms vertices on the CPU; the GPU only
receives camera-space geometry plus a single projection matrix in vertex-shader constant
buffer slot 0 (`ProjectionHook.cpp` captures it). Each 3D draw call is issued twice with the
game's state untouched, changing only the viewport (left / right half of the render target)
and that constant buffer (per-eye matrix built from the headset's asymmetric frustum plus an
off-axis eye offset). The back buffer ends up side-by-side and is submitted to SteamVR with
texture bounds `0..0.5` / `0.5..1`. The UI is drawn on a virtual screen 1.8 m away; the sky is
drawn without eye offset (at infinity).

**Head tracking** (`HeadTrackHook.cpp`). The engine's global view matrix (`fromWorld`, 4x4
doubles) is written by one function once per frame. That function is hooked and, after the
last call of each frame, the whole matrix is rewritten from the headset pose: the full
rotation of the headset (with yaw anchored to the game camera's yaw) and the headset position
added to the camera position, converted to game units and clamped with the engine's own ray
cast so the camera never goes through walls or objects. The culling camera block is kept in
sync with the rewritten view (`SceneCullingRootHook.cpp`) so shadows and entities are visible
in every direction. The pose used for rendering is attached to the `Submit`
(`Submit_TextureWithPose`) so the compositor reprojects correctly.

**Game addresses.** The `Blade.exe` offsets (the `k...` constants at the top of
`HeadTrackHook.cpp`, `SceneCullingRootHook.cpp` and `FromWorldLocator.cpp`) match the Steam
build of the remaster; they will need checking if the game is updated.

## Building

Requirements: Visual Studio 2019/2022 with the "Desktop development with C++" workload,
CMake 3.16+ and git (MinHook is fetched during configure).

From a console with CMake in the PATH (e.g. the "x64 Native Tools Command Prompt for VS"),
in the repository folder:

```
cmake -B build -A x64
cmake --build build --config Release
```

Then copy the three files from `output\Release\` (`dxgi.dll`, `BladeVR.dll` and
`openvr_api.dll`) next to the game executable, in `Blade of Darkness\bin\bin\`.

## Debug log

The mod writes no files by default. To get a log, create an empty file named
`BladeVR_debug.txt` next to `Blade.exe`; the mod will then write
`bin\bin\BladeVR_logs\BladeVR_<pid>_<date>.log` on every run.

## Layout

```
hookdll/                 BladeVR.dll
  dllmain.cpp            entry point, module installation order
  Dx11Hook.*             Present/ResizeBuffers/CreateSwapChain, submission to SteamVR
  ProjectionHook.*       capture of the projection matrix (VS slot 0)
  StereoHook.*           split-viewport stereo and stereo keys
  OpenVRHook.*           SteamVR: init, poses, frustums, Submit
  HeadTrackHook.*        6DOF head tracking and collision
  SceneCullingRootHook.* culling kept consistent with the rewritten view
  FromWorldLocator.*     finds the global view matrix in memory
  HookLogger.*           optional log
proxydll/                dxgi.dll proxy (ProxyMain.cpp + MASM thunks)
ThirdParty/openvr/       openvr.h, openvr_api.lib, openvr_api.dll (OpenVR SDK)
```

## Known limitations

* The character selection screen and each character's opening cinematic are shown on a
  rectangular, letterboxed screen with black margins (a "box" effect), and some objects
  spill over into the black area.
* The intro video and the first loading screen are shown with wrong separation (stereo turns
  on when the menu appears).
* Thin black slivers can appear at the edges of some portals: the engine clips each sector's
  geometry against the portal from the central camera, not from each eye.
* Weapons and the shield clip through walls and objects as in the original game, and in third
  person the camera can enter the character.

## Licence

BladeVR is released under the [MIT licence](LICENSE), copyright (c) 2026 Efraim27.
You may use, modify and redistribute it freely as long as the copyright notice and
the licence text are kept, i.e. the author is credited.

## Third-party

* [MinHook](https://github.com/TsudaKageyu/minhook) (2-clause BSD licence).
* [OpenVR SDK](https://github.com/ValveSoftware/openvr) (3-clause BSD licence).
