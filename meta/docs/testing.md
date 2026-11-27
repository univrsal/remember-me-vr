# Testing

Build first ([building.md](building.md)).

## Unit test (host)

```sh
meta/tools/space-test.sh
```

Compiles and runs `src/tests/space_test.cpp` natively: the OpenXR <-> UE3 coordinate conversions. CI runs this on every push.

## Smoke test without the game

```sh
meta/tools/smoke-test.sh                                   # Wine's own d3d9 (wined3d)
meta/tools/smoke-test.sh --dxvk ~/.local/share/Steam/steamapps/compatdata/228300/pfx/drive_c/windows/syswow64/d3d9.dll
```

This runs `smoke_host.exe`, renamed to `RememberMe.exe`, in a throwaway Wine prefix (`~/.cache/vrmod-smoke`). It checks export forwarding, the D3D9 hook chain, Present/Reset and, with `--dxvk`, the DXVK interop query. It prints `PASS` and the mod's log.

## VR path without the game

With SteamVR running (and the one-time i686 manifest from [installing-linux.md](installing-linux.md) set up):

```sh
meta/tools/smoke-test.sh --proton --frames 900   # colour-cycling panel in the headset for ~10 s
```

This uses Proton Experimental in its own prefix under `~/.cache/vrmod-smoke` and sets `SteamGameId` like a Steam launch; Proton only sets up VR for Steam launches.
