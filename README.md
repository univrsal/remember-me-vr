# Remember Me VR

A work-in-progress VR mod for *Remember Me* (DONTNOD, 2013; Unreal Engine 3, Direct3D 9, 32-bit). The evaluation, design and roadmap are in [`docs/`](docs/README.md).

The repository has two parts:
- **`core/` (vrcore):** game-independent plumbing for D3D9 VR mods.
- **`games/<name>/`:** one thin module per game.

The idea is that the core can be reused for other D3D9 games.

## Layout

```
core/                     vrcore: shared by every game module
  include/vrcore/         public headers
    game.h                  the vrcore::Game interface a game module implements
    log.h config.h          log file + INI config next to the DLL
    hook.h                  MinHook wrapper (inline + COM vtable-slot hooks)
    process.h               host exe info (base, PE timestamp, VA/RVA helpers)
    dxvk_interop.h          DXVK D3D9<->Vulkan interop interfaces
  src/                    DllMain, D3D9 hooks (Direct3DCreate9 -> CreateDevice -> Present/Reset)
  proxy/<dll>/            proxy DLL implementations (forwarding exports + .def)
games/rememberme/         Remember Me module
  game.cpp                vrcore::Game implementation
  RememberMeVR.ini        default config
  deploy.env smoke.env    settings for tools/deploy.sh and tools/smoke-test.sh
tests/smoke_host.cpp      stand-in game exe for testing a build under Wine
tools/deploy.sh           install/uninstall into the game folder
tools/smoke-test.sh       run the smoke host under Wine with the built mod
tools/package.sh          zip a build for release (used by CI)
.github/workflows/        CI: build + package on push/PR, release on v* tags
cmake/                    mingw toolchain file, vrcore_add_mod() helper
docs/                     evaluation, architecture, roadmap, capture analysis
```

The following are gitignored and stay local: `re/` (the unpacked exe, Ghidra projects, capture conversions), `tools/steamless/`, `*.rdc` captures, `build/` and `dist/`.

## Building (Linux)

Requirements: `mingw-w64-gcc` (i686), CMake ≥ 3.24, Ninja, git. Dependencies (MinHook, Vulkan-Headers) are fetched at configure time, pinned to tags.

```sh
cmake --preset mingw-i686-release        # or mingw-i686-debug
cmake --build --preset mingw-i686-release
```

Output: `build/mingw-i686-release/games/rememberme/dinput8.dll` plus `RememberMeVR.ini`. The DLL is self-contained: it only needs KERNEL32 and the Windows CRT.

## Testing without the game

```sh
tools/smoke-test.sh                                   # Wine's own d3d9 (wined3d)
tools/smoke-test.sh --dxvk ~/.local/share/Steam/steamapps/compatdata/228300/pfx/drive_c/windows/syswow64/d3d9.dll
```

This runs `tests/smoke_host.exe`, renamed to `RememberMe.exe`, in a throwaway Wine prefix (`~/.cache/vrmod-smoke`). It checks export forwarding, the D3D9 hook chain, Present/Reset and, with `--dxvk`, the DXVK interop query. It prints `PASS` and the mod's log.

## Installing into the game

```sh
tools/deploy.sh install      # copies dinput8.dll (+ default ini if absent) into Binaries/Win32
tools/deploy.sh status       # what's installed + last log lines
tools/deploy.sh uninstall    # removes only what install put there
```

Steam launch options (Proton):

```
WINEDLLOVERRIDES="dinput8=n,b" %command%
```

The log is written to `Binaries/Win32/RememberMeVR.log`. Set `[Core] Enabled=0` in the INI to turn the mod into a plain pass-through proxy.

## Packaging and CI

```sh
tools/package.sh             # -> dist/RememberMeVR-<git describe>.zip
```

The zip mirrors the game folder (`Binaries/Win32/dinput8.dll`, `RememberMeVR.ini`), plus `INSTALL.txt` and `VERSION.txt` at the root, so users extract it straight into the game's install folder. The packaged DLL has its debug info stripped; the copy in `build/` keeps it.

`.github/workflows/build.yml` runs on every push, pull request and manual trigger. It builds with Ubuntu's mingw-w64, packages, and uploads the zip as a workflow artifact.

To publish a release, push a tag starting with `v`:

```sh
git tag v0.1.0 && git push origin v0.1.0
```

That creates a GitHub pre-release named after the tag, with the zip attached and auto-generated notes. To build more games in CI, add them to the workflow's `matrix.game` list.

## Adding another game

1. Create `games/<name>/` with:
   - a `game.cpp` implementing `vrcore::Game` and defining `vrcore::GetGame()`;
   - a `CMakeLists.txt` calling `vrcore_add_mod(<Target> PROXY <dll> SOURCES game.cpp)`;
   - a default `.ini`, `deploy.env`, `smoke.env`, `package.env` and `INSTALL.txt`.
2. Pick a proxy DLL the game imports, and that isn't `d3d9.dll` (that would replace DXVK). If `core/proxy/<dll>/` doesn't exist yet, add one modelled on `core/proxy/dinput8/`.
3. Configure with `-DVRMOD_GAME=<name>`.

`Game::OnDeviceCreated` is the first safe point to hook game code. On SteamStub-protected executables, `.text` is still encrypted while `DllMain` runs.

## Third-party code

- [MinHook](https://github.com/TsudaKageyu/minhook): BSD 2-clause (fetched at build time)
- [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers): Apache-2.0 / MIT (fetched at build time)
- `core/include/vrcore/dxvk_interop.h` is trimmed from [DXVK](https://github.com/doitsujin/dxvk) (zlib)
