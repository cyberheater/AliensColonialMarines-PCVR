# Aliens: Colonial Marines PCVR

A VR mod for the Steam version of **Aliens: Colonial Marines** (2013): full stereo 3D, head tracking, and motion controllers, played through SteamVR. It has been tested on PlayStation VR2 and Meta Quest 3 (through Steam Link).

The game itself stays untouched. A small launcher starts the normal `ACM.exe` and loads the mod into it.

## Features

- **Stereo 3D** with each eye's own field of view. Lopsided headset views, like the Quest 3's, are handled automatically.
- **Head tracking**, including leaning and moving your head.
- **Motion controllers:**
  - Your right hand aims the gun, and the gun model sits in your hand.
  - The left hand appears when you hold the motion tracker.
- **Walk where you look:** the left stick moves you in the direction you're facing.
- **Temporal anti-aliasing (TAA)** to stop shimmer, plus the game's chromatic aberration, ambient occlusion, motion blur and FXAA turned off. These effects either look wrong or cost too much in VR.
- **Menus, pause and cutscenes** appear on a large virtual screen.
- **Interface sized for VR:** the HUD is shrunk towards the centre of view, and the objectives/scoreboard panel is moved lower and made smaller.
- **Bigger pickup range and width** for the "use" prompt.
- **Launcher with a resolution slider** that reads the headset's resolution from SteamVR.

## Installing

1. Download the latest release zip and extract the `ACMVR` folder into the game folder, so you get
   `...\steamapps\common\Aliens Colonial Marines\ACMVR\`.
2. In Steam, right-click the game > **Properties** > **General** > **Launch Options**, and enter:
   ```
   "C:\Program Files (x86)\Steam\steamapps\common\Aliens Colonial Marines\ACMVR\acmvr_launcher.exe" %command%
   ```
   Use your own path if the game is installed somewhere else, and keep the quotes.
3. **Turn the Steam overlay off for this game** under Properties > General. In VR it takes over the headset and makes the game pause with a "Resume" menu.
4. Start SteamVR, then press Play.

To play without VR again, clear the launch options. [docs/LAUNCHER-README.txt](docs/LAUNCHER-README.txt) has more detail.

### Quest 3 (Steam Link)

- Set Steam Link to **72 or 80 Hz**. At 90 Hz the game sometimes can't keep up, and Steam Link fills in the missing frames, which shows as warping.
- Make sure SteamVR is using the Quest and not another headset's add-on.

## Controls (Sony Sense / Touch controllers)

| Input | Action |
|---|---|
| Right trigger | Fire |
| Left trigger | Aim down sights (better accuracy, slower movement) |
| Left stick | Move, towards where you look |
| Right stick left/right | Snap turn |
| Right stick up / down | Flashlight / sidearm |
| Right stick click | Melee |
| Hold Y / Triangle | Weapon wheel: pick with the right stick, release Y to select |
| The button the on-screen "X" prompt shows | Use / pick up |
| Square + Triangle (or X + Y) | Start (pause) |
| Cross + Circle (or A + B) | Back (objectives / scoreboard) |
| Both stick clicks + right stick up/down | Adjust your height |

**Keyboard:**

| Key | Action |
|---|---|
| F1 | Toggle TAA |
| Ctrl | Capture frames (for bug reports) |

## Settings

Settings live in `ACMVR\acmvr.ini`. [config/acmvr.ini](config/acmvr.ini) is a commented template. Some useful ones:

| Section | Setting | Default | Effect |
|---|---|---|---|
| `[VR]` | `HUDScale` | 0.7 | HUD size |
| `[VR]` | `HUDCentreX`, `HUDCentreY` | 640, 360 | Point the HUD shrinks towards |
| `[VR]` | `ObjectivesScale` | 0.7 | Objectives panel size |
| `[VR]` | `ObjectivesMoveDown` | 200 | Moves the objectives panel down |
| `[VR]` | `ObjectivesMoveRight` | 110 | Keeps the objectives panel centred |
| `[VR]` | `ObjectivesOnScreen` | 0 | 1 = show objectives on the flat virtual screen instead |
| `[Controls]` | `MoveWithHead` | 1 | 0 = walk where the gun points |
| `[Graphics]` | `DisablePostEffects` | | Names of extra effects to turn off, e.g. `AmbientOcclusion,MotionBlur,FXAA` |

`acmvr.log`, in the same folder, records what the mod is doing. Include it with any bug report.

## Building

The mod and launcher are 32-bit Windows binaries, cross-compiled on Linux with clang/lld:

```
SYSROOT=/path/to/mingw-w64-i686-sysroot DEPS=/path/to/deps ./build.sh
SYSROOT=/path/to/mingw-w64-i686-sysroot DEPS=/path/to/deps ./build_launcher.sh
```

- `SYSROOT` needs mingw-w64 i686 headers in `include` and import libraries in `lib`.
- `DEPS` needs the OpenVR headers in `openvr/headers` and the Vulkan headers in `vulkan`.

The output goes to `build/`: `d3d9.dll`, which ships as `acmvr.dll`, and `acmvr_launcher.exe`.

The release also ships these third-party files:

- `dxvk_d3d9.dll` from [DXVK](https://github.com/doitsujin/dxvk) (zlib licence).
- `openvr_api.dll` from [OpenVR](https://github.com/ValveSoftware/openvr) (BSD 3-clause licence).

## How it works, briefly

- **Hooking:** the launcher starts the game suspended and loads `acmvr.dll` into it. The mod then redirects the game's Direct3D 9 calls to DXVK, and its XInput calls to the VR controllers.
- **Stereo views:** the mod hooks Unreal Engine 3's `CalcSceneView`, so each frame is drawn as two side-by-side views with per-eye projections taken from SteamVR.
- **Sending to the headset:** each eye is copied into Vulkan images and sent to SteamVR from its own thread, tagged with the head position it was drawn for.
- **Game objects:** the arms, weapons, HUD and post effects are found and changed through UE3's runtime reflection (GNames/GObjects). The game's own script functions are called through `ProcessEvent`.

This is an unofficial fan project, not affiliated with SEGA, Gearbox or 20th Century Studios. You need your own copy of the game.
