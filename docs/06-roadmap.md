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
- [x] Same checks in the real game under Proton (2026-11-26): all hooks fire, DXVK interop returns Vulkan handles. Findings from the log:
  - D3D9 calls come from **UE3's render thread**: the device is created on the main thread and presented from another.
  - Device flags `0x142` (FPU_PRESERVE | HARDWARE_VERTEXPROCESSING | DISABLE_DRIVER_MANAGEMENT), **not** `MULTITHREADED`. DXVK's `LockDevice` is therefore a no-op, so all interop and VR submission must run on the render thread, inside our D3D9 hooks.
  - Backbuffer 1880×2089, `D3DFMT_X8R8G8B8`, swap effect `COPY`, 1 back buffer. `Direct3DCreate9` is called twice (same object).
- [x] OpenXR instance and session on DXVK's device; copy the backbuffer into a quad layer (`vrcore::VirtualScreen`). Verified 2026-11-26 with the Proton smoke test: SteamVR/OpenXR 2.18.4, session reaches FOCUSED, `B8G8R8A8_SRGB` swapchain with a bit-exact copy. Findings:
  - **32-bit runtime manifest needed.** A 32-bit game's wineopenxr uses Proton's 32-bit native OpenXR loader, which reads `~/.config/openxr/1/active_runtime.i686.json` first and otherwise falls back to SteamVR's 64-bit runtime ("wrong ELF class"). SteamVR ships a 32-bit `bin/vrclient.so` and `steamxr_linux32.json`; `tools/setup-openxr-i686.sh` writes the i686 manifest with an absolute path.
  - **Proton only sets up VR for Steam launches.** Its `steam.exe` helper writes the `HKCU\Software\Wine\VR` state that wineopenxr waits for only when `SteamGameId` is set. The smoke test sets it.
  - **SteamVR must be running before the game.** DXVK asks wineopenxr for the required Vulkan extensions when it creates its instance.
  - **No OpenXR teardown at process exit.** The D3D9/DXVK device is already gone by then, and destroying the session makes the runtime submit to a dead queue and crash.
  - **Copy stall.** The copy waits for the GPU per frame (`WaitForResource` + fence). Simple and correct; replace with semaphores/timeline later if it costs too much.
- [x] Works in the real game (2026-11-26): world-locked panel in the Index, 1880×2089 sRGB swapchain with bit-exact copy, no extension warnings with SteamVR started first.
  - UE3 **restarts its render thread** (presents moved from thread 564 to 616 at ~17 s, around loading). OpenXR calls need only be externally synchronised, not on a fixed thread, so this is fine, but nothing may assume one render thread for the whole session.
- [ ] Remaining exit checks: 30-minute session, F8 recenter, resolution change (Reset), frame pacing feel
- **Exit criteria:** stable for 30+ minutes, no extension or device-lost errors. ~~Answers whether DXVK + wineopenxr interop works in a 32-bit process.~~ **Answered: yes**, given the i686 runtime manifest.

## M2 — Engine access

- [x] `GNames`/`GObjects` found by shape at runtime; object/class/property dumps (`vrcore::ue3`, F9 in game). See 08.
- [x] Live `LocalPlayer`, `GameViewportClient`, `PlayerController`, `Camera` objects and their vtables identified
- [x] Located in Ghidra: `UGameViewportClient::Draw` `0x0086D7F0`, `ULocalPlayer::CalcSceneView` `0x008622B0`, `ProcessEvent` `0x00458BA0`, `FPerspectiveMatrix` `0x008092B0`, `FSceneView` ctor `0x00A17F80`; `ViewState` at `LocalPlayer+0x8C`
- [x] **Finding:** `CalcSceneView` adds its view to the family itself, so calling it twice per frame gives two views in one family (S2) through a single hook. See 08.
- [x] Logging hook on `CalcSceneView`: argument order confirmed; one call per frame normally
- [x] `AllocateViewState()` found (`0x00A22B90`); the right eye gets its own view state (a null one crashes DONTNOD's motion blur)
- [x] **Side-by-side test works**: two views in one family, the 3D scene renders correctly in both, toggles on/off without problems. The UI only uses View[0] (see 08)
- [ ] `ProcessEvent` hook
- [ ] Prove control: change camera position and projection for one frame (flat screen)

## M3 — Synchronous stereo (go/no-go)

- [ ] Two views per family via the `CalcSceneView` hook (S2), with per-eye viewport half, view state, eye offset and asymmetric projection. If DNE passes break with two views: draw the viewport twice per tick instead (S1)
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
| Game update changes code | Low–Medium | Byte-pattern checks on every RVA; log the build (`changelist 940782`, PE file-header timestamp `0x69d8a358`) |

## Open questions

1. Which console commands survive in the shipping build? The UI console doesn't open, but the exec path is live (see M2).
2. Reversed-Z or standard depth? It affects the projection override. Captures point to standard depth (see 07); confirm from the projection constants.
3. ~~Where is the GFx HUD drawn?~~ Answered: straight into the backbuffer, after one fixed scene-copy draw (see 07). Still open: are any UI elements deliberately depth-tested against the world?
4. Does `FViewport::Draw` have a `bShouldPresent` parameter in this build?
5. Are the 2048² shadow maps per-light (shareable between eyes) or view-fitted cascades?
6. Which OpenXR runtime is the target? SteamVR for now; test Monado/WiVRn later.
