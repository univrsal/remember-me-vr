# Repository layout

The repository has two code parts:

- **`src/core/` (vrcore):** game-independent plumbing for D3D9 VR mods.
- **`src/games/<name>/`:** one thin module per game.

The core can be reused for other D3D9 games (see [adding-a-game.md](adding-a-game.md)).

```
src/
  core/                   vrcore: shared by every game module
    include/vrcore/         public headers
      game.h                  the vrcore::Game interface a game module implements
      log.h config.h          log file + INI config next to the DLL
      hook.h                  MinHook wrapper (inline + COM vtable-slot hooks)
      process.h               host exe info (base, PE timestamp, VA/RVA helpers)
      dxvk_interop.h          DXVK D3D9<->Vulkan interop interfaces
    src/                    DllMain, D3D9 hooks (Direct3DCreate9 -> CreateDevice -> Present/Reset),
                            OpenXR (xr/), UE3 reflection and space conversions (ue3/)
    proxy/<dll>/            proxy DLL implementations (forwarding exports + .def)
  games/rememberme/       Remember Me module
    game.cpp                vrcore::Game implementation
    RememberMeVR.ini        default config
    INSTALL.txt             shipped in the release zip
    deploy.env smoke.env package.env
                            settings for deploy.sh, smoke-test.sh and package.sh
  tests/
    smoke_host.cpp          stand-in game exe for testing a build under Wine
    space_test.cpp          host unit test for the OpenXR <-> UE3 conversions
meta/
  cmake/                  mingw toolchain file, vrcore_add_mod() helper
  docs/                   these docs, plus the evaluation and engine analysis
  tools/
    deploy.sh               install/uninstall into the game folder
    smoke-test.sh           run the smoke host under Wine with the built mod
    space-test.sh           host unit test (run in CI)
    package.sh              zip a build for release (used by CI)
    setup-openxr-i686.sh    one-time: let 32-bit OpenXR apps use SteamVR on Linux
    ghidra/                 headless Ghidra scripts (native labelling, decompile, queries)
    renderdoc/              RenderDoc replay scripts for frame analysis (rdrun.sh runs them)
.github/workflows/        CI: build + package on push/PR, release on v* tags
CMakeLists.txt, CMakePresets.json
```

## Local-only files

These are gitignored and stay on your machine:

- `Remember Me`: a symlink to the game's install folder (used by `deploy.sh`)
- `re/`: the unpacked exe, Ghidra projects, capture conversions
- `meta/tools/steamless/`
- `*.rdc` captures
- `build/` and `dist/`

Game files and anything derived from the game binary are never part of this repository.
