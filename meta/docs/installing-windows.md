# Installing on Windows

The mod was developed on Linux under Proton. It also runs on Windows (tested 2026-11-27: Windows 10, SteamVR/OpenXR 2.18.4, Valve Index, RX 6700 XT, DXVK's `d3d9.dll`): stereo, head tracking, controllers, the HUD panel, Sensen hints and the window mirror work. Please report problems with the log attached.

## Requirements

- Remember Me (Steam).
- An OpenXR runtime with Vulkan support that is set as active (SteamVR, or your headset vendor's). Start it before the game: the mod asks it for the headset's resolution at startup (otherwise the game renders at the window size until the VR session starts, then switches).
- **DXVK's 32-bit `d3d9.dll`** (from <https://github.com/doitsujin/dxvk/releases>, the `x32` folder), placed in `Binaries/Win32` next to the game exe. The mod gets the Vulkan image behind the game's D3D9 textures through DXVK; with Windows' native D3D9 it only logs and does nothing else.

## Install

1. Extract the release zip into the game's install folder (the one that contains `Binaries/`, `Engine/` and `ExampleGame/`). The files go to:
   ```
   Binaries/Win32/dinput8.dll
   Binaries/Win32/RememberMeVR.ini
   ```
2. Copy DXVK's 32-bit `d3d9.dll` to `Binaries/Win32/`.
3. Start your OpenXR runtime (SteamVR), then launch the game.
4. No graphics settings needed: in VR the mod starts the game windowed, at the headset's resolution, without vsync (see [configuration.md](configuration.md#resolution-and-window-size)). Keep the game's `SuperSampleCount` at 1 (its default).

The log is `Binaries/Win32/RememberMeVR.log`. `[Core] Enabled=0` in the INI turns the mod into a plain pass-through.

## What the mod does differently on Windows

Nothing to configure; for troubleshooting:

- **Vulkan extensions.** OpenXR needs some Vulkan extensions on DXVK's instance and device. Under Proton, DXVK gets them from Wine's `wineopenxr`; on Windows it doesn't ask, so the mod adds them itself (hooks on `vulkan-1.dll`'s `vkCreateInstance`/`vkCreateDevice`, only those the driver supports). The log shows `vkCreateInstance: added OpenXR extensions …`. A `DXVK's VkInstance lacks …` warning means that didn't work.
- **GPU queue.** SteamVR on Windows submits work to the Vulkan queue the mod hands it, which is DXVK's. The mod holds DXVK's submission lock around those OpenXR calls; without it the game froze on the first stereo frame.
- **Freezes.** If the game stops presenting for 5 seconds, the log gets every thread's call chain (`hang: …` lines) and the OpenXR call in progress. `[Debug] HangDumpSeconds` changes the delay (0 = off). Send those lines with a report.

## Uninstall

Delete `dinput8.dll`, `RememberMeVR.ini`, `RememberMeVR.log` and DXVK's `d3d9.dll` from `Binaries/Win32`.
