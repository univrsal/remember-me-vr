# Configuration

Settings live in `Binaries/Win32/RememberMeVR.ini`; every option is commented in the file itself. Changes apply on the next game start. `[Core] Enabled=0` turns the mod into a plain pass-through proxy (no hooks, no log).

## Hotkeys

| Key | Action | INI option |
|---|---|---|
| F6, or both stick clicks | Open/close the in-game config menu | `[Menu] ToggleKey`, `PadToggle` |
| F8 | Recenter the view | `[VR] RecenterKey` |
| F9 | Write an object dump (`RememberMeVR_objects.txt`), for development | `[UE3] DumpObjectsKey` |
| F10 | Toggle the flat side-by-side stereo test, for development | `[Stereo] ToggleKey` |

Values are Windows virtual-key codes (`0x77` = F8); `0` disables a key.

## Main `[VR]` options

- `Mode`: `stereo` (default) or `screen` (the game on a flat virtual screen).
- `RenderSize`, `RenderScale`: the resolution the game renders at in stereo, independent of the window (see [Resolution and window size](#resolution-and-window-size)).
- `ForceWindowed`, `DisableVSync` (both on by default; also in the menu's Display tab, applied at the next start): start the game in a window that fits the screen, and without vsync, whatever the game's own graphics settings say.
- `StereoSource`: `scene` (the game's internal scene image; recommended) or `backbuffer`.
- `WorldScale`: Unreal units per metre of head movement. Larger = you feel smaller.
- `CameraOrientation`: `yaw` keeps the horizon level (game camera pitch/roll ignored); `full` follows the game camera.
- `UiPanel`, `UiDistance`, `UiWidth`, `UiHeight`: the game's UI (menus, HUD, subtitles) on its own floating panel in stereo.
- `FlatLoadingScreens`, `FlatMenus`: show loading screens and the pause/Sensen/Combo Lab menus on a flat panel instead of in stereo.
- `ScreenWidth`, `ScreenDistance`: size and distance of that flat panel, in metres.
- `MirrorView`, `MirrorUi`: what the game window on your monitor shows (left eye, or both eyes).
- `DisableMotionBlur`: turn off the game's motion blur (on by default).

## Config menu, free camera, visibility

The config menu (F6, or press both thumbsticks) edits most settings live and can save them back to the INI. Control it with mouse, keyboard, gamepad, or in VR with a laser from either controller. The game gets no input while it's open.

- **Free camera** (a checkbox in the menu, `[FreeCam]`): fly the view while the game keeps running. WASD + mouse, or left stick move and right stick look; triggers up/down, RB/LB fast/slow.
- **`[Visibility]`:** hide the player, NPCs, placed skeletal meshes or the Sensen AR hints from the rendered view.
- **`[Game] SkipIntroMovies`:** skip the intro and boot logos (story cutscenes still play).
- **`[Input] FreeMouseInMenus`:** the mouse cursor is freed in the game's own menus.

## Controllers

`[Input] VRControllers=1` presents the VR controllers to the game as player 0's gamepad, merged with a real gamepad if one is connected: sticks (+click) = sticks, triggers = LT/RT, grips = LB/RB, right A/B = A/B, left A/B = X/Y, right trackpad press = Start, left trackpad press = Back (centre) or D-pad (edges). Touch controllers: menu = Start. `GripThreshold` sets how far a grip must be squeezed to count as LB/RB.

## Resolution and window size

In stereo mode the game renders both eyes side by side at `[VR] RenderSize`, whatever the window size:

- `auto` (default): the headset's recommended eye size (as OpenXR reports it, so SteamVR's resolution setting applies) × `RenderScale`. E.g. Index at 100%: 2 × 2016 × 2240 = 4032 × 2240.
- `WIDTHxHEIGHT`, e.g. `4032x2240`: a fixed size for both eyes together.
- `window`: the old behaviour, where the window size × `SuperSampleCount` is the render size.

See [Performance](#performance) for choosing a size your GPU can handle.

The window just shows that image scaled to fit (letterboxed, scaled with bilinear filtering), so pick any window size that fits your monitor. The mouse is mapped to match. If no headset is found when the game starts, it renders at the window size until the VR session starts, then switches.

The render size only applies to a window (fullscreen renders at the display mode), so with `[VR] ForceWindowed=1` (default) the mod starts the game windowed. That happens if the game is set to fullscreen (its default) or to a window larger than the screen, using a window at the render aspect about 3/4 of the screen's size. The game's own graphics config isn't changed: when the game saves it, the mod writes back your own fullscreen and window settings. `[VR] DisableVSync=1` (default) presents without vsync while VR runs, since the headset paces the game.

So a fresh game install needs no graphics settings changes. To pick the window yourself, set `ForceWindowed=0` and edit the game's graphics config (it ignores launch options):

- Linux: `~/.local/share/Steam/steamapps/compatdata/228300/pfx/drive_c/users/steamuser/Documents/My Games/UnrealEngine3/RememberMeGame/Config/ExampleAdriftGfxConfig.ini`
- Windows: `Documents\My Games\UnrealEngine3\RememberMeGame\Config\ExampleAdriftGfxConfig.ini`

```ini
[Rendering]
ResWinWidth=1600      ; any size; the VR resolution comes from RenderSize
ResWinHeight=900
SuperSampleCount=1    ; keep at 1 with RenderSize (2 would halve the UI resolution)
Fullscreen=false
VSync=false
```

The in-game graphics menu may rewrite this file. More in [05-toolchain.md](05-toolchain.md).

## Performance

The render size is the main performance setting: GPU cost grows with the pixel count, i.e. with the square of `RenderScale`. SteamVR's recommended size is usually well above the panel's native resolution, and higher still when its resolution slider is raised, so `auto` at 1.0 can be far more than a GPU handles in this game.

Example: Valve Index (panel 1440×1600 per eye), SteamVR recommending 2384×2648 per eye, RX 6700 XT:

| `RenderScale` | Per eye | vs. native pixels | Result |
|---|---|---|---|
| 0.5 | 1192×1324 | 0.7× | smooth |
| 0.6 | 1430×1589 | about 1× | about native sharpness |
| 0.7 | 1669×1854 | 1.3× | starts to lag |
| 1.0 | 2384×2648 | 2.7× | far too slow |

On Windows the same setup runs at about 75 fps at `RenderScale` 0.9 (GPU load 65–100%).

If it lags:

- Lower `[VR] RenderScale`, or keep it at 1.0 and lower this game's resolution in SteamVR (per-app video settings; `auto` follows it). The log line `render size hooks installed; …` shows the size in use.
- Run the headset at a lower refresh rate (e.g. 90 or 80 Hz instead of 120/144): each frame then gets more time, which often allows a higher resolution.
- SteamVR's motion smoothing can cover occasional missed frames, but it warps moving objects (the character looks distorted when the game camera turns). Turn it off for this game if that bothers you.
- Lower the game's own effect and shadow quality.

To measure: `[Debug] PerfLogSeconds=5` logs a `perf:` line every 5 seconds with the frame rate, the headset's refresh rate, and average/max milliseconds for the frame, `xrWaitFrame` (near 0 means the game is slower than the headset), the mod's GPU wait and copy, `xrEndFrame`, and the rest of the frame (the game's own work). DXVK's HUD shows the GPU load: put `dxvk.hud = fps,frametimes,gpuload` in a `dxvk.conf` next to `RememberMe.exe` (or set `DXVK_HUD`; on Windows Steam must be restarted to pass it on). It's drawn in the game window, not in the headset. Well below 100% GPU load means the CPU is the limit.

## Log

`Binaries/Win32/RememberMeVR.log`. `[Log] Level` ranges from `trace` to `off`.

`[Debug] HangDumpSeconds` (default 5): if the game stops presenting for that long, the log gets every thread's registers and likely call chain (`hang: …` lines, module+offset), plus the OpenXR call in progress, once per hang. 0 turns it off. The game's offsets are VAs at `0x00400000` + offset; the mod's resolve with `addr2line` against the same build.
