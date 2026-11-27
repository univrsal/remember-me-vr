# Documentation

## Using the mod

| Doc | Contents |
|---|---|
| [installing-linux.md](installing-linux.md) | Linux / Proton / SteamVR install, step by step |
| [installing-windows.md](installing-windows.md) | Windows install |
| [configuration.md](configuration.md) | `RememberMeVR.ini` options, hotkeys, render resolution and window size, log |

## Working on the code

| Doc | Contents |
|---|---|
| [layout.md](layout.md) | Repository layout and what lives where |
| [building.md](building.md) | Building with mingw-w64 on Linux |
| [testing.md](testing.md) | Unit test and Wine/Proton smoke tests |
| [deploying.md](deploying.md) | Installing a dev build into the game folder |
| [packaging.md](packaging.md) | Release zips, CI and tagging a release |
| [adding-a-game.md](adding-a-game.md) | Adding a module for another game |

## Research and design notes

[evaluation.md](evaluation.md) summarises the feasibility study. The details:

| Doc | Contents |
|---|---|
| [01-game-analysis.md](01-game-analysis.md) | What the install and executable contain, and why it matters |
| [02-graphics-layer.md](02-graphics-layer.md) | D3D9, DXVK, dgVoodoo2, and the "resolution limit" question |
| [03-vr-options.md](03-vr-options.md) | Every route to VR we considered, compared |
| [04-recommended-architecture.md](04-recommended-architecture.md) | The design for a custom mod |
| [05-toolchain.md](05-toolchain.md) | Building and debugging on Linux/Proton vs Windows |
| [06-roadmap.md](06-roadmap.md) | Milestones, go/no-go gate, risks and open questions |
| [07-frame-capture.md](07-frame-capture.md) | RenderDoc analysis of an in-game frame |
| [08-engine-map.md](08-engine-map.md) | Engine addresses, object/property layouts, `CalcSceneView` and the stereo hook plan |
| [09-stereo-artifacts.md](09-stereo-artifacts.md) | Per-eye rendering artifacts: findings, capture workflow, where to resume |
