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
- `StereoSource`: `scene` (the game's internal scene image, so stereo resolution = window size × `SuperSampleCount`; recommended) or `backbuffer`.
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

## Window size

In stereo mode the game window holds both eyes side by side, so the window size sets the eye resolution. The mod's log suggests a size for 1:1 pixels. The game ignores launch options for this; edit its graphics config instead:

- Linux: `~/.local/share/Steam/steamapps/compatdata/228300/pfx/drive_c/users/steamuser/Documents/My Games/UnrealEngine3/RememberMeGame/Config/ExampleAdriftGfxConfig.ini`
- Windows: `Documents\My Games\UnrealEngine3\RememberMeGame\Config\ExampleAdriftGfxConfig.ini`

```ini
[Rendering]
ResWinWidth=2016
ResWinHeight=2240
SuperSampleCount=1    ; 2 = render at 2x width and 2x height, then downsample
Fullscreen=false
```

The monitor limits the window size; for more pixels, use a smaller window with `SuperSampleCount=2`. The in-game graphics menu may rewrite this file. More in [05-toolchain.md](05-toolchain.md).

## Log

`Binaries/Win32/RememberMeVR.log`. `[Log] Level` ranges from `trace` to `off`.
