# Remember Me VR

A VR mod for *Remember Me* (DONTNOD, 2013; Unreal Engine 3, Direct3D 9, 32-bit). It plays the game in VR through OpenXR, built on a reusable D3D9 VR core.

## Features

- **Stereo rendering:** the game is rendered once per eye, with head tracking and the headset's own projection.
- **Floating UI:** menus, HUD and subtitles on their own panel; loading screens and menus can be shown on a flat virtual screen.
- **VR controllers as a gamepad:** Index and Touch bindings, haptics as rumble, merged with a real gamepad if connected.
- **In-game config menu** (Dear ImGui; mouse, keyboard, gamepad or a VR laser pointer) for most settings, with save.
- **Free camera**, and options to hide the player, NPCs and Sensen hints.
- **Free mouse in the game's menus**, and the intro movies are skipped.
- **Comfort:** level-horizon camera, motion blur off, adjustable world scale.
- **Monitor mirror:** the game window shows the left eye or both eyes.
- **Per-eye artifact fixes** for the game's depth-based effects, fog and Sensen AR hints.

### Hotkeys

| Key | Action |
|---|---|
| F6 (or both stick clicks) | Open/close the config menu |
| F8 | Recenter the view |
| F9 | Dump the engine's object list (development) |
| F10 | Toggle the flat side-by-side test (development) |

Keys and all other options are in `RememberMeVR.ini`; see [configuration](meta/docs/configuration.md).

## Install

1. Extract the release zip into the game's install folder (it puts `dinput8.dll` and `RememberMeVR.ini` in `Binaries/Win32`).
2. Start SteamVR (or another OpenXR runtime) **before** the game.
3. Linux/Proton also needs a launch option and a one-time OpenXR manifest; Windows needs DXVK's 32-bit `d3d9.dll`.

Details: [Linux / Proton](meta/docs/installing-linux.md) · [Windows](meta/docs/installing-windows.md)

## Development

[Documentation index](meta/docs/README.md): [repository layout](meta/docs/layout.md), [building](meta/docs/building.md), [testing](meta/docs/testing.md), [packaging](meta/docs/packaging.md), [adding another game](meta/docs/adding-a-game.md).

## License

zlib; see [LICENSE](LICENSE). Game files and anything derived from the game binary are not part of this repository.

## Third-party code

- [MinHook](https://github.com/TsudaKageyu/minhook): BSD 2-clause (fetched at build time)
- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers): Apache-2.0 / MIT (fetched at build time)
- [OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) headers: Apache-2.0 (fetched at build time; vrcore negotiates with the runtime itself, no loader is linked)
- `src/core/include/vrcore/dxvk_interop.h` is trimmed from [DXVK](https://github.com/doitsujin/dxvk) (zlib)
