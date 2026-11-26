# 06 — Roadmap, risks, open questions

Each milestone reduces the risk of the next one. **Milestone 3 is the go/no-go gate**: if synchronous stereo can't be made to work, the project stops there. There is no AFR fallback (see 03).

## M0 — Baseline

- [x] Game launched under Proton Experimental; user INIs generated (`Documents/My Games/UnrealEngine3/RememberMeGame/Config/`)
- [x] RenderDoc capture taken and analysed (see 07)
- [x] Exe unpacked with Steamless (Variant 3.1, local only in `re/`)
- [x] Native function `{name, fn}` tables confirmed in the unpacked exe
- [x] Mono fps baseline: ~180 fps at 1880×2090, `SuperSampleCount=1` (window clamped by monitor size; see 05 for resolution setup and 07 for the stereo estimate)
- [ ] Check whether the baseline is GPU-bound (`DXVK_HUD=gpuload`), and measure a heavier scene
- [x] Captures with the HUD visible, menus, Combo Lab, item pickup, voice-comm overlay, cutscene (see 07)
- [ ] Captures of a Bink FMV and a Memory Remix sequence
- [x] Check the `~` console: **does not open** in this shipping build (the INI already binds `ConsoleKey=Tilde`, so the console object probably isn't created; see M2)
- [ ] Re-measure the baseline at lower settings (the 180 fps figure was at maximum settings)
- [ ] Import the unpacked exe into Ghidra; label the native tables with a script
- [ ] Decompile the camera/HUD/LocalPlayer classes from `ExampleGame.upk`, `Adrift.upk` and `Engine.upk` with UE Explorer

## M1 — VR plumbing (test harness, not a product)

- [x] Repo skeleton: CMake + mingw-w64 i686, `dinput8` proxy, logger, INI config, MinHook, deploy script, Wine smoke test. Split into a reusable `core/` and `games/rememberme/` (see the root `README.md`)
- [x] `Direct3DCreate9` → `CreateDevice` → device/swapchain `Present` and `Reset` hooks (verified under Wine with wined3d and with Proton's DXVK `d3d9.dll`)
- [x] `QueryInterface(ID3D9VkInteropDevice)`; log the Vulkan handles (verified with DXVK in the smoke test)
- [ ] Same checks in the real game under Proton (`tools/deploy.sh install`, then read `RememberMeVR.log`)
- [ ] OpenXR instance and session on DXVK's device; copy the backbuffer into a quad layer
- **Exit criteria:** stable for 30+ minutes, no extension or device-lost errors. Answers whether DXVK + wineopenxr interop works in a 32-bit process. If it doesn't, try the OpenVR fallback.

## M2 — Engine access

- [ ] `GNames`/`GObjects` located; object/class/property dump
- [ ] Live `LocalPlayer`, `GameViewportClient`, `PlayerController` and `Camera` objects, and their vtables, identified
- [ ] `ProcessEvent` hook
- [ ] Re-enable the console: construct the console object (`ExampleGame.ExampleConsole`) and assign it to `GameViewportClient.ViewportConsole`, the usual UE3 console-unlocker approach. Independently, call the engine's console-command `Exec` path directly from the mod. The port itself issues `SETRES` through it, so that path is live in the shipping build. Some cheat/debug commands may be compiled out.
- [ ] Locate in Ghidra: the viewport draw call site (`FViewport::Draw` / `UGameViewportClient::Draw`), `ULocalPlayer::CalcSceneView`, `ULocalPlayer::ViewState` offset, and the view-state allocator
- [ ] Prove control: overwrite camera POV and projection for one frame (flat screen)

## M3 — Synchronous stereo (go/no-go)

- [ ] Draw the viewport twice per tick, with per-eye POV, asymmetric projection and view state (S1)
- [ ] Capture both eyes; suppress the first present; submit both eyes in one OpenXR projection layer with matching poses
- [ ] Head tracking applied to the view only
- [ ] Check the side effects of drawing twice (HUD script, counters, scene captures)
- **Exit criteria:** correct, comfortable stereo while walking and fighting in one chapter, at the HMD's refresh rate or with stable reprojection. Both eyes always from the same tick.
- **Go/no-go:** if S1 is blocked by engine internals, try S2 (two views per family) before stopping. Shader-level stereo is the very last thing to consider (see 03).

## M4 — Usable mod

- [ ] HUD and menus redirected to a quad layer
- [ ] Bink FMV cinema mode; decide how to handle in-engine cutscenes (stereo with the camera locked, or a theatre view)
- [ ] Comfort: no roll or shakes; world scale; recentre; INI config
- [ ] Optional: motion-controller → XInput mapping

## M5 — Performance (only if needed)

- [ ] Two views in one family (S2) to share shadows, cubemap and scene setup between eyes
- [ ] Measure against S1 and keep whichever is better

## Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| Synchronous stereo not achievable in this engine | Low–Medium | S1 needs only a small set of engine hooks; S2 as a second attempt. Decide at M3 |
| GPU cost of two full draws (S1) too high for the HMD refresh rate | Medium | Measure the baseline in M0; lower eye resolution or `SuperSampleCount`; turn off costly DNE effects; S2 |
| DXVK's `VkDevice` lacks VR runtime extensions | Medium | Tested first, in M1; OpenVR fallback; separate VR device with external memory |
| Drawing twice triggers game-side effects (HUD script, timers) | Medium | Find them in M2/M3; run them on one eye only |
| Customised engine differs from known UE3 layouts | Medium | Rely on our own Ghidra, reflection and RenderDoc findings, not other games' SDKs |
| Scripted cameras and cutscenes are uncomfortable | High | Comfort toggles; cutscene theatre mode |
| Game update changes code | Low–Medium | Byte-pattern checks on every RVA; log the build (`changelist 940782`, PE timestamp `0x69d8a355`) |

## Open questions

1. Which console commands survive in the shipping build? The UI console doesn't open, but the exec path is live (see M2).
2. Reversed-Z or standard depth? It affects the projection override. Captures point to standard depth (see 07); confirm from the projection constants.
3. ~~Where is the GFx HUD drawn?~~ Answered: straight into the backbuffer, after one fixed scene-copy draw (see 07). Still open: are any UI elements deliberately depth-tested against the world?
4. Does `FViewport::Draw` have a `bShouldPresent` parameter in this build?
5. Are the 2048² shadow maps per-light (shareable between eyes) or view-fitted cascades?
6. Which OpenXR runtime is the target? SteamVR for now; test Monado/WiVRn later.
