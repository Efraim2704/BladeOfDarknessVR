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

* **Real stereo 3D.** The engine draws the world twice per frame, once from each eye, with
  the headset's own per-eye projection. Each eye gets its own portal clipping, shadows,
  reflections and lighting, so there are no seams at the edges of doorways or columns. The
  world, characters, weapons, shadows, water and the sky all have correct depth. Menus and the
  HUD are shown on a comfortable virtual screen.
* **6DOF head tracking.** The camera follows the rotation *and* position of the headset,
  anchored to the game's own camera, in both first and third person. Leaning into walls or
  objects is prevented using the engine's own collision.
* **"Walk where I look" mode.** Optional: the character turns to follow the headset so you
  walk in the direction you are looking. The turn is delivered only to the game, so the
  Windows mouse cursor is never moved.
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
* **Edge smoothing (anti-aliasing): off**. This one is not optional: with it enabled the
  stereo image is lost.
* **Motion blur: off**. It smears the image in the headset.
* **Bloom: off** (it is off by default, and only becomes available with HDR on). The glow is
  computed over the whole frame, which holds both eyes side by side, so the halo of a torch
  bleeds from one eye's image into the other's.
* **Ambient occlusion: off** is recommended. It is also computed in screen space, so it is
  miscalculated where the two eyes meet, and it costs frame time that VR needs.
* **HDR: as you like.** It only changes the precision of the lighting, not how the image is
  composed, so it is safe either way.
* **Enable the frame-rate limit** in the game options. Without it the camera moves in
  jerks instead of smoothly.
* **Field of view: leave it alone.** The mod forces at least 145 degrees while it runs,
  whatever the menu says, so that no black margins appear at the edges of the view.

## Keys

| Key | Function |
|---|---|
| **Page Up / Page Down** | Eye separation +4 / −4 game units (default 58). Adjust until the world feels the right size. |
| **End** | Size of the virtual screen used for menus and HUD (53° / 75° / 90°). |
| **Insert** | Head tracking on (default) / off. Turning it on re-centres the view. |
| **Home** | Re-centre position: the camera goes back to the character's eyes. |
| **Delete** | Toggle between *free look* (the character does not turn with your head) and *walk where I look* (the character turns to follow the headset). |

## How it works

**Loading.** Windows searches the executable's folder before System32, so the game loads our
`dxgi.dll`. It forwards every export to the real system DXGI (loaded by absolute path) and
loads `BladeVR.dll` from its `DllMain`. It also intercepts `CreateDXGIFactory*` to hand the
factory to the mod, which hooks `IDXGIFactory::CreateSwapChain` and learns the game's D3D11
device before the first `Present`.

**Stereo** (`SceneCullingRootHook.cpp`, `StereoHook.cpp`). The engine transforms vertices on
the CPU and clips the level against portals from its camera; the GPU only receives
camera-space geometry plus a single projection matrix in vertex-shader constant buffer slot 0
(`ProjectionHook.cpp` captures it). The engine's world render (`B_Map::Render`) is called
twice per frame, with the camera moved half the eye separation to the left and then to the
right. Everything the engine caches per frame (portal clipping, vertex transform, character
poses, shadows, lights) is keyed to a frame counter that this function increments itself, so
the second pass is rebuilt from the other eye. The engine batches its draws and sends them to
bgfx at the end of the frame; the batches of each pass are routed to copies of the engine's
bgfx views whose viewport is the left or right half of the render target. `StereoHook.cpp`
recognises those draws by their viewport and draws them once with that eye's asymmetric
frustum. Flickering lights (torches) draw their random intensity only in the left pass, so
both eyes see the same flame. Anything 3D drawn outside the world render is issued twice,
once per eye, with an off-axis eye offset. The back buffer ends up side-by-side and is
submitted to SteamVR with texture bounds `0..0.5` / `0.5..1`. The UI is drawn on a virtual
screen 1.8 m away.

**Head tracking** (`HeadTrackHook.cpp`). The engine's global view matrix (`fromWorld`, 4x4
doubles) is written by one function once per frame. That function is hooked and, after the
last call of each frame, the whole matrix is rewritten from the headset pose: the full
rotation of the headset (with yaw anchored to the game camera's yaw) and the headset position
added to the camera position, converted to game units and clamped with the engine's own ray
cast so the camera never goes through walls or objects. The culling camera block is kept in
sync with the rewritten view (`SceneCullingRootHook.cpp`) so shadows and entities are visible
in every direction. The pose used for rendering is attached to the `Submit`
(`Submit_TextureWithPose`) so the compositor reprojects correctly. In *walk where I look*
mode the character is turned with mouse movement that only the game sees: a raw-input message
posted to its window, whose contents are supplied by `GetRawInputData`, hooked in the game's
import table.

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
`bin\bin\BladeVR_logs\BladeVR_<pid>_<date>.log` on every run: the modules it installs, the
SteamVR handshake, the per-eye frustums and a short summary every minute (frame rate, worst
frame, whether the world is being drawn per eye). Please attach it when reporting a problem.

## Layout

```
hookdll/                 BladeVR.dll
  dllmain.cpp            entry point, module installation order
  Dx11Hook.*             Present/ResizeBuffers/CreateSwapChain, submission to SteamVR
  ProjectionHook.*       capture of the projection matrix (VS slot 0)
  StereoHook.*           side-by-side stereo, flat screen, anchored UI, keys
  FovHook.*              minimum field of view (character selection, cinematics)
  OpenVRHook.*           SteamVR: init, poses, frustums, Submit
  HeadTrackHook.*        6DOF head tracking and collision
  SceneCullingRootHook.* world drawn once from each eye; culling kept in sync
  FromWorldLocator.*     finds the global view matrix in memory
  HookLogger.*           optional log
proxydll/                dxgi.dll proxy (ProxyMain.cpp + MASM thunks)
ThirdParty/openvr/       openvr.h, openvr_api.lib, openvr_api.dll (OpenVR SDK)
```

## Known limitations

* The glow of torches and other lights does not fade towards the edges of your view as it
  does in the flat game. The game fades it over its own frame, which in VR is far wider than
  what each eye sees, so a torch you look at out of the corner of your eye keeps its full
  halo instead of shrinking.
* The character sheet in the character selection screen appears to drift slightly when you
  turn your head.
* Weapons and the shield clip through walls and objects as in the original game, and in third
  person the camera can enter the character.
* The mirror on your monitor does not show the screens anchored in front of you (the F1 combo
  list and the character sheet): those go straight to the SteamVR overlay and never pass
  through the game's own image.
* VR motion controllers are not supported: you play with keyboard and mouse or with a
  gamepad, as in the flat game.

## Version history

* **1.2** — The world is now drawn by the engine from each eye. No more black slivers at the
  edges of doorways and columns; shadows, water reflections and torch light are correct in
  both eyes. *Walk where I look* (Delete) now turns the character without moving the
  Windows mouse. Default eye separation 58.
* **1.0** — First release: stereo 3D, 6DOF head tracking, flat screen for videos and menus,
  minimum field of view.

## Licence

BladeVR is released under the [MIT licence](LICENSE), copyright (c) 2026 Efraim27.
You may use, modify and redistribute it freely as long as the copyright notice and
the licence text are kept, i.e. the author is credited.

## Third-party

* [MinHook](https://github.com/TsudaKageyu/minhook) (2-clause BSD licence).
* [OpenVR SDK](https://github.com/ValveSoftware/openvr) (3-clause BSD licence).
