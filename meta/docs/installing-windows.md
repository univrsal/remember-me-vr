# Installing on Windows

The mod was developed and tested on Linux under Proton. The Windows install follows the same design but has had less testing; please report problems.

## Requirements

- Remember Me (Steam).
- An OpenXR runtime with Vulkan support that is set as active (SteamVR, or your headset vendor's). Start it before the game.
- **DXVK's 32-bit `d3d9.dll`** (from <https://github.com/doitsujin/dxvk/releases>, the `x32` folder), placed in `Binaries/Win32` next to the game exe. The mod gets the Vulkan image behind the game's D3D9 textures through DXVK; with Windows' native D3D9 it only logs and does nothing else.

## Install

1. Extract the release zip into the game's install folder (the one that contains `Binaries/`, `Engine/` and `ExampleGame/`). The files go to:
   ```
   Binaries/Win32/dinput8.dll
   Binaries/Win32/RememberMeVR.ini
   ```
2. Copy DXVK's 32-bit `d3d9.dll` to `Binaries/Win32/`.
3. Start your OpenXR runtime (SteamVR), then launch the game.
4. Set a sensible window size: see [configuration.md](configuration.md#window-size).

The log is `Binaries/Win32/RememberMeVR.log`. `[Core] Enabled=0` in the INI turns the mod into a plain pass-through.

## Uninstall

Delete `dinput8.dll`, `RememberMeVR.ini`, `RememberMeVR.log` and DXVK's `d3d9.dll` from `Binaries/Win32`.
