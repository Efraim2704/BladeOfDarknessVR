# BladeVR

*[Leer en español](README.es.md)*

A virtual reality mod for **Severance: Blade of Darkness** (the 2021 DirectX 11 remaster on Steam).
It adds **full 3D stereo** (a separate image for each eye) and a **6DOF head-tracked camera**
through SteamVR / OpenVR. Developed and tested with a Meta Quest 2 over SteamVR.

The game files are not modified. The mod is a DLL that is loaded when `Blade.exe` starts
(through a `dxgi.dll` proxy) and hooks DirectX 11 plus a handful of engine functions.

**New in 2.0: [combat with motion controllers](#motion-controllers-in-the-game).** Your hands
are the character's hands: swing the weapon to strike, raise the shield to block, take an
arrow from your back and draw the bow, punch with empty hands, bring food to your mouth to
eat. The Quest controllers are also a complete gamepad for everything else, and there are
four combat modes, from full motion controls to the classic game with the controllers used
as an Xbox gamepad.

Since 1.6: [diorama mode](#diorama-mode). With one key the whole level becomes a miniature
model on a virtual table that you keep playing on.

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
* **Motion-controller combat.** In first person the character's arms follow your hands and
  its body stays under the headset. Weapons hit where they actually touch (the engine's own
  hit detection, with sparks, blood and cuts), at the same rate and stamina cost as the
  game's basic attack. Shield, two-handed weapons, bow, punches, throwing, drawing and
  sheathing weapons, food and potions all work with gestures.
* **The Quest controllers as a gamepad.** They reach the game as one more Xbox controller,
  so menus, combos, inventory and everything else work as with a real gamepad. You can use a
  real gamepad and the keyboard at the same time.
* **"Walk where I look" mode.** The character turns to follow the headset so you walk in the
  direction you are looking (on by default with motion controls). The turn is delivered only
  to the game, so the Windows mouse cursor is never moved.
* The monitor shows the left eye centred (with black bars at the sides) instead of the
  split image; SteamVR's own "VR View" window is also available.
* **Diorama mode.** The whole level as a model on a table, with cuts, a view cone, three
  model modes, cinematics and a mixed-reality background (see below).
* Works from the main menu; no injector or launcher needed.

## Requirements

* Blade of Darkness (2021 remaster, Steam, 64-bit).
* SteamVR and a compatible headset (tested with Quest 2 via Link / Air Link and Virtual Desktop).
* Windows 10/11 x64.
* Motion controllers: Quest (Touch) controllers over SteamVR. They reach the game through
  Steam Input, which is the game's default: leave it on.
* Diorama mode with a gamepad: Steam Input. Mixed reality: Virtual Desktop (optional).

## Installation

Copy these three files into the game's `bin\bin` folder (the one that contains `Blade.exe`,
usually `...\steamapps\common\Blade of Darkness\bin\bin`):

* `dxgi.dll` — proxy that loads the mod
* `BladeVR.dll` — the mod
* `openvr_api.dll` — OpenVR runtime (looked up next to `BladeVR.dll`)

Start SteamVR first, then the game. If SteamVR is not running or no headset is connected,
the game runs normally without VR. When upgrading, just overwrite the three files.

In the same folder the mod creates the folder `BladeVR_bindings` (the controller bindings it
gives SteamVR) and, the first time you change a setting, `BladeVR.ini`: last combat mode,
calibration, stamina on or off, eye height, and whether you left the game in diorama mode
(then the next game starts in it). To uninstall, delete the three files, `BladeVR_bindings`
and `BladeVR.ini` (and `BladeVR_logs` and `BladeVR_debug.txt` if you made a debug log).

If the SteamVR dashboard opens on its own when the game starts, open the game's
*Properties* in Steam and turn off *Use Desktop Game Theatre while SteamVR is active*.

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
| **Ctrl + Page Up / Page Down** | Eyes further forward / back on the character's head, 1 cm per step (default 10 cm). |
| **Shift + Page Up / Page Down** | Floating-hands mode only: eyes higher / lower, 5 cm per step (default 0; saved). |
| **End** | Size of the virtual screen used for menus and HUD (53° / 75° / 90°). |
| **Insert** | Head tracking on (default) / off. Turning it on re-centres the view. |
| **Home** | Re-centre position: the camera goes back to the character's eyes. |
| **Delete** | Toggle between *free look* (the character does not turn with your head) and *walk where I look* (the character turns to follow the headset). It starts on *walk where I look* in the motion-controller modes and on *free look* in mode 0. |
| **Shift + F8** | Swings and punches with the controllers use stamina (default) / do not. Saved. |
| **Ctrl + F8** | Calibrate your grip and arm length (optional, see below). |
| **F5** | Diorama mode on / off (see below). |

## Motion controllers in the game

### Combat modes

| Mode | What it does |
|---|---|
| **1 · Motion controllers** (default) | The character's arms follow yours. You attack by moving the weapon; A does not attack. |
| **3 · Floating hands** | Like 1, but without the body: only the forearms, hands and weapons, which follow the controllers with no arm-length limit. Your eyes can be raised (Shift + Page Up / Page Down). |
| **2 · Motion controllers + combos** | A and the stick directions perform the game's combos (the arms follow the combo animation), and moving the weapon does the basic attack. |
| **0 · Basic VR** | No motion controls: the Quest controllers are an Xbox gamepad and you play exactly as in the flat game. *Free look* by default. |

**Right thumb on the thumb rest + left stick click held for 1 second** goes to the next
mode, in the order 1 → 3 → 2 → 0 → 1. A notice on the headset shows the new mode, and the
last one you chose is remembered. Changing to a motion-controller mode puts the view in
first person; changing to mode 0 re-centres the view as Insert pressed twice does.

### What your hands do (modes 1, 3 and 2)

* **Attack**: a quick swing with the weapon. It hits where the blade actually touches, with
  sparks against walls. Pulling the arm back towards your body does not hit.
* **Punch**: with empty hands, the same quick swing with your fist.
* **Block**: raise the shield in front of you, at shoulder height or higher, with the back of
  your left hand facing forward. With a two-handed weapon, hold it with both hands and raise
  it.
* **Two-handed weapons**: bring your left hand to the handle and hold the left grip to take
  the weapon with your left hand too; release the grip to let go.
* **Draw and sheathe weapons**: reach over your shoulder, behind your head, and press that
  hand's grip.
* **Throw** (modes 1 and 3): hold the right grip for half a second and bring your right arm
  down quickly from head height. In mode 2, as in the game: right grip held + A.
* **Bow** (modes 1 and 3, bow in the left hand): take an arrow from your back with the right
  grip, press the right trigger with your right hand next to the bow to take the string,
  pull your hands apart to draw and release the trigger to shoot. The left grip at your back
  puts the bow away.
* **Lock on an enemy** with the right trigger. With your right hand empty it locks on
  without drawing your weapons. **Y** releases the enemy.
* **Food and potions** stay in your hand: bring them to your mouth to eat or drink.
* The game's vibration reaches the controllers.

### Quest controller buttons

| Control | Modes 1, 3 and 2 | Mode 0 (Xbox gamepad) |
|---|---|---|
| **Left stick** | Walk (never runs on its own). Locked on an enemy, it moves relative to the enemy, as in the game. | Walk; fully pushed, run. |
| **Left stick click** | Sprint, in the direction of the stick, until you let it go or click again. | L3 (switches the gamepad's diorama model layer, as on an Xbox gamepad). |
| **Right stick** | Camera. | Camera. |
| **Right stick click** | Re-centre the view. | R3. |
| **A** | Mode 2 and menus: A. Modes 1 and 3: A does not attack while your arms move the character's arms (you attack with the weapon). | A (attack). |
| **B** | Jump, only with the left stick pushed forward or back (locked on an enemy, any direction: dodges). With the stick at rest or pushed sideways, select nearby objects. | B. |
| **X** | Use. | X (use). |
| **Y** | Release the locked enemy. | Y (turn around). |
| **Left grip** | Hold a two-handed weapon with the left hand. | LB (block). |
| **Right grip** | RB (throw). | RB (throw). |
| **Left / right trigger** | LT / RT (RT locks on an enemy). | LT / RT. |

In menus, A, B, Y and the left grip work as on a gamepad. With the **left thumb on the thumb
rest** (all modes): the right stick is the D-pad (the camera stays still), A is Start and B is
Back. **Left thumb on the thumb rest + right stick
click** re-centres the view (in mode 0 this is the way to re-centre); held for 1 second, it
enters diorama mode.

### Settings

* **Calibration (Ctrl + F8)** is optional: the default grip fits most players. Use it only
  if the arms feel too long or too short. Pressing it re-centres the view and starts a
  5-second countdown (one vibration per second); before it ends, stand with both arms
  stretched forward, thumbs up, looking straight ahead: it measures then. It is saved; to go
  back to the default grip, delete the `agarre_*` and `alcance_*` lines from `BladeVR.ini`.
* **Stamina (Shift + F8)**: swings and punches with the controllers use stamina like the
  game's basic attack; with too little you get a clumsy attack, with none, nothing.
* A real gamepad and the keyboard and mouse work at the same time as the Quest controllers:
  the one you are using is the one that counts.
* When a level loads, the view goes to first person. The arms follow the controllers only
  in first person.

## Diorama mode

Press **F5**, or with the Quest controllers **left thumb on the thumb rest + right stick click
held for 1 second** (or **R3** with a gamepad in the model layer). The whole level appears as a
miniature model on a virtual table 45 cm in front of you, a little below your eyes, with your
character in the centre — and you keep playing on it. Look at it from any side, lean in, walk
around it. Press F5 again (Quest: left thumb on the thumb rest + right stick click) to go back
to the normal VR view.

* **The whole map** at once (1:10 to start, from 1:1 to 1:500): every room, walls seen from
  both sides, ceilings included, no fog. Water reflections, object shadows, moving torch
  light, doors, walls that break down and the sky all work on the model. The level is drawn
  on the GPU in this mode, so the game keeps its 60 fps with the whole map.
* **Model modes** (LB, or left stick click on the motion controllers): *fixed* (the model
  stays where you put it), *attached* (default: the model moves with the character, who stays
  in the centre of the table) and *from behind* (attached, and the model slowly turns so you
  see the character from behind, like the game camera).
* **Seeing inside:**
  * **Height cut** — removes everything above a height.
  * **Vertical cut** — removes everything between you and a vertical plane in front of the
    character, on the side you are looking at. It is straight, along the walls of the map, and
    stays on its side when you turn the model. In the *from behind* mode it turns with the
    model, so you always see the model cut from the front; hold **A** to switch to a straight
    cut that smoothly moves to the new side of the map when the model turns past half way.
  * **Visible area** — only a square of the map around the character is drawn.
  * **View cone** — a round hole between your eyes and the character that removes the walls,
    ceilings and upper floors in the way (off at start, three widths).
  * **Characters** stay always visible (default) or are cut like the rest.
* **Move relative to your view** (Delete toggles, on by default): pushing the left stick
  forward walks towards where you are looking on the model. Combat and combos work as usual.
  In the *from behind* mode the game's own control is used.
* **Play with the Quest controllers on the model**: right thumb on the thumb rest + left stick
  click held for 1 second, and the controllers drive the character like an Xbox gamepad,
  without gestures, in any combat mode: left stick to walk (click: sprint), A attacks, B jumps
  or selects, X uses, Y turns around, left grip blocks, right grip throws; the right stick
  click re-centres the model. The same again gives them back to the model.
* **Cinematics** are followed from the game's camera. Only the zoom, the cone, the cuts, the
  area and the background work, and the zoom makes the world grow or shrink like a model.
  Re-centring (Home or X) goes back to the cinematic camera keeping the size. When it ends,
  the model comes back as it was.
* A short notice on the headset shows every change.

### Keyboard

| Key | Function |
|---|---|
| **F5** | Diorama on / off. |
| **Home** | Put the model in front of you, centred on the character, at 1:15 to find your character quickly (in a cinematic: back to its camera). |
| **Page Up / Page Down** | Zoom: model bigger / smaller. |
| **F4** | View cone: off → small → medium → large. |
| **F6** | Visible area: whole map → 60 → 30 → 15 m. |
| **F7** | Background: the game's own → magenta → green (mixed reality). |
| **Pause** | Characters in the cuts: always visible ↔ cut. |
| **Delete** | Move the character relative to your view of the model on / off. |

### Gamepad (Xbox layout, through Steam Input)

**L3** switches the whole gamepad between the game and the model, also outside diorama mode.
While the gamepad controls the model, the game receives nothing but Start and Back and the
character stands still; press L3 again to play. In combat mode 0 the left stick click of the
Quest controllers is this L3.

| Button | Function |
|---|---|
| **R3** | Diorama on / off. Held: characters in the cuts. |
| **X** | Put the model in front of you at 1:15 (in a cinematic: back to its camera). |
| **LB** | Model mode: fixed → attached → from behind. |
| **RB** | Visible area: whole map → 50 → 20 m (back to the whole map also removes the cuts). |
| **D-pad left / right** | View cone on / off. |
| **D-pad up / down** | View cone wider / narrower. |
| **A** | Vertical cut 1.5 m in front of the character on / off. Held: in the *from behind* mode, cut turning with the model ↔ straight cut that changes side. |
| **B** | Height cut 1.5 m above the character on / off. |
| **Y** | Passthrough background on / off. |
| **Right stick** | Up / down: zoom. Left / right: turn the model around the table (not in the *from behind* mode). |
| **RT / LT** | Zoom: model bigger / smaller. |
| **Left stick** | Move the table sideways and forward / back. |

### Motion controllers (Quest over SteamVR)

| Control | Function |
|---|---|
| **One grip** | The model follows your hand: move, raise, lower. |
| **Both grips** | Spread or close your hands to scale; turn the line between your hands to turn the model. |
| **One trigger** | Move your hand up or down: height cut. Towards or away from the model: vertical cut. The first direction you move in decides which. |
| **Both triggers** | Spread or close your hands: size of the visible area. |
| **Left stick click** | Model mode. Held: view cone off → small → medium → large. |
| **Right stick click** | Visible area. Held: characters in the cuts. |
| **Left stick** | Move the table sideways and forward / back. |
| **Right stick** | Left / right: turn the model. Up / down: zoom. |
| **A (right)** | Vertical cut 1.5 m in front of the character on / off. Held: cut turning with the model ↔ straight (*from behind* mode). |
| **B (right)** | Height cut 1.5 m above the character on / off. |
| **X (left)** | Put the model in front of you at 1:15. |
| **Y (left)** | Passthrough background on / off. |

### Mixed reality with Virtual Desktop

In Virtual Desktop go to *Streaming → VR Passthrough → Environment* and set a colour key
(magenta or green, with similarity, smoothing and opacity to taste). In the game press **F7**
(or **Y**) to draw the background in that colour: the model appears over your room. Link and
Air Link have no passthrough for SteamVR games.

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
import table. Standing still, it is turned by setting its angle directly, so it does not take
steps.

**Motion controllers** (`VrInput.cpp`, `VrPadHook.cpp`, `CombatVRHook.cpp`, `CombatVRIk.h`).
The controllers are read through SteamVR's action input (the mod writes its own action
manifest and default bindings, which also give the thumb rests). The game reads gamepads
through Steam Input; seven entries of that interface are replaced so the game sees one more
Xbox controller, fed from the Quest controllers, and treats it exactly like a real one. The
arms are posed by inverse kinematics after the engine evaluates the skeleton each frame, with
whatever the hand holds attached to it. Swings are measured from the real controller and
switch on the weapon's own hit detection while it moves fast, so the engine itself decides
where it hits; the stamina cost comes from the game's own formula. Blocking with the shield
or a two-handed weapon goes through the engine's blocking test, and food, potions and the bow
use the game's own Python scripts.

**Diorama mode** (`DioramaHook.cpp`, `DioramaGpuLevel.cpp`). The camera written by the game is
replaced by the head pose carried to the table (anchor + rotation · (head − table) · scale),
and the eye separation is multiplied by the scale, which is what makes the world look like a
small model. The world is drawn once from the centre of the head and every draw is issued
twice with an off-axis eye offset; water reflections, which the engine clips with cones
through the eye, are drawn once per eye. Portal visibility, back-face culling and fog are
turned off so the whole map is drawn. With the engine's CPU transform the whole map was too
slow, so on entering a level the faces the engine sends are captured once and drawn by the GPU
with the engine's own lighting; doors, breakable walls and flowing liquids, which change
during play, are still drawn by the engine. Cuts and the view cone are applied per pixel.

**Game addresses.** The `Blade.exe` offsets (the `k...` constants at the top of the hook
files) match the Steam build of the remaster; they will need checking if the game is updated.

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

The mod writes no log by default. To get one, create an empty file named
`BladeVR_debug.txt` next to `Blade.exe`; the mod will then write
`bin\bin\BladeVR_logs\BladeVR_<pid>_<date>.log` on every run: the modules it installs, the
SteamVR handshake, the controllers, the per-eye frustums, changes of combat mode and a short
summary every minute (frame rate, worst frame, whether the world is being drawn per eye,
what was done with the controllers). Please attach it when reporting a problem.

## Layout

```
hookdll/                 BladeVR.dll
  dllmain.cpp            entry point, module installation order
  Dx11Hook.*             Present/ResizeBuffers/CreateSwapChain, submission to SteamVR
  ProjectionHook.*       capture of the projection matrix (VS slot 0)
  StereoHook.*           side-by-side stereo, flat screen, anchored UI, keys
  FovHook.*              minimum field of view (character selection, cinematics)
  OpenVRHook.*           SteamVR: init, poses, frustums, Submit, notices on the headset
  VrInput.*              motion controllers through SteamVR actions (manifest, bindings)
  VrPadHook.*            the Quest controllers as a Steam Input gamepad, gestures
  CombatVRHook.*         motion-controller combat: arms, swings, shield, bow, food, modes
  CombatVRIk.h           inverse kinematics of the arms
  BladeVRIni.*           BladeVR.ini (settings that are remembered)
  HeadTrackHook.*        6DOF head tracking and collision
  SceneCullingRootHook.* world drawn once from each eye; culling kept in sync
  DioramaHook.*          diorama mode: model camera, cuts, cone, controls, cinematics
  DioramaGpuLevel.*      diorama mode: level captured and drawn on the GPU
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
* In combat modes 1, 3 and 2 three actions of the game have no button: blocking with a
  one-handed weapon and no shield, throwing what you hold in the left hand (except by
  raising the shield) and jumping on the spot. Use mode 0, a gamepad or the keyboard for them.
* The arms follow the controllers only in first person; in third person the game's own
  animations are shown.
* Diorama mode: very small effects (the ripples of footsteps in water) can be smaller than a
  pixel when the model is small, and particle effects (such as waterfalls) are not reflected
  in the water of the model.

## Version history

* **2.0** — Combat with motion controllers: arms that follow your hands, attacks with the
  weapon's own hit detection and the game's rate and stamina cost, shield and two-handed
  blocking, VR bow with arrows from your back, punches, throwing, drawing and sheathing
  weapons with a gesture, food and potions to your mouth. The Quest controllers as a full
  gamepad, usable together with a real gamepad and the keyboard. Four combat modes (motion
  controllers, floating hands, motion controllers with the game's combos, basic VR),
  remembered in `BladeVR.ini`. *Walk where I look* on by default and without steps when
  standing still. Diorama mode: enter and leave it with the Quest controllers, play on the
  model with them, and A / B now give the vertical and height cuts.
* **1.6** — Diorama mode: the whole level as a miniature model on a table, with height and
  vertical cuts, visible area, view cone, three model modes, movement relative to your view,
  cinematics, mixed-reality background for Virtual Desktop, motion controllers and a gamepad
  layer for the model, and notices on the headset.
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
