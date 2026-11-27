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

- [x] Two views per family via the `CalcSceneView` hook (S2), with per-eye viewport half and own view state (side-by-side test, M2)
- [ ] **Stereo in VR (built 2026-11-26, untested in the headset):**
  - `vrcore::Compositor` replaces `VirtualScreen`. The game thread calls `BeginStereoFrame` (OpenXR eye poses + FOVs predicted one display period after the last `xrWaitFrame`), renders, then `SubmitStereoFrame`. The next `Present` submits the backbuffer halves as a projection layer with exactly those poses. Frames without stereo data (loading screens, videos) fall back to the flat panel.
  - Per eye, `CalcSceneView` runs with its viewport half, view state, eye pose (`eventGetPlayerViewPoint` hook: game camera, yaw only by default, plus tracked eye pose; world scale configurable) and asymmetric projection (`FPerspectiveMatrix` hook, x/y terms replaced, depth terms kept).
  - Conversions are in `vrcore/ue3_space` with a host unit test (`tools/space-test.sh`, run in CI).
- [x] First headset test (2026-11-26, Proton): stereo depth correct; smooth enough. Found:
  - **Resolution tied to the window** (recommended 2368×2628 per eye = a 4736×2628 window, larger than the monitor). Fix: stereo frames now come from the game's scene image (scene tap: texture sampled by the first draw into the backbuffer), so resolution = window × `SuperSampleCount`.
  - **In game only head roll worked.** When the PlayerCamera has a target, CalcSceneView calls DNE's look-at (`0x008091C0`), and the view matrix is then built from **(target − location)** (`0x0040DC60`), with only roll taken from the rotation (both eyes also toed in toward the target). Restoring the rotation alone wasn't enough (second test: still roll only). Fix: in the look-at hook (CalcSceneView call site only, return address `0x008625E9`, during our eye calls), move the target onto the eye's forward axis at the game's target distance, and restore the rotation.
  - Second test: resolution via the scene image works (4736×2628 at `SuperSampleCount=2`, window 2368×1314, ~140 fps).
  - **Right-eye puddle reflections** show a flat band (`puddle_red_band_stereo.rdc`); not yet located (see 08).
  - Sensen AR hints follow head roll and are misplaced (they use the game camera; part of the per-eye Sensen work).
- [x] Third test: **head tracking works in game**. Stereo, depth, resolution OK. Some choppiness on Linux (seen in other games there too); recheck on Windows. Mod-side suspects if it persists: the per-frame GPU wait before the copy, and the one-period pose prediction.
- **M3 passed (2026-11-26): synchronous stereo works.**
- [ ] Tune defaults: world scale, camera orientation mode
- **Exit criteria:** correct, comfortable stereo while walking and fighting in one chapter, at the HMD's refresh rate or with stable reprojection. Both eyes always from the same tick.
- **Go/no-go:** S2 (two views per family) is implemented; if it proves unusable in the headset, fall back to S1 (draw the viewport twice per tick). Shader-level stereo is the very last thing to consider (see 03).

## M4 — Usable mod

- [ ] Bink FMV cinema mode; decide how to handle in-engine cutscenes (stereo with the camera locked, or a theatre view)
- [ ] Comfort: no roll or shakes; world scale; recentre; INI config
- [x] Motion blur off in VR (confirmed 2026-11-26, much smoother): `[VR] DisableMotionBlur=1` clears `GSystemSettings.bAllowMotionBlur` (`0x0162EE28 + 0x38`, found through the `[SystemSettings]` name table in `FUN_005F21F0`) every frame; `UberPostProcessEffect`'s proxy creation (`FUN_00641940`) checks it
- [ ] 2D UI on its own panel (built, untested): in stereo frames, every backbuffer draw after the scene copy that doesn't sample the scene image is UI laid out in the left view's rect. It's redirected into a transparent texture of that size (premultiplied alpha via separate alpha blend, restored per draw) and submitted as a quad layer over the stereo projection layer. Mono frames are untouched. Monitor mirror: UI drawn into the window too and the left eye shown in VR (`[VR] MirrorUi`, `MirrorView`; see 09 "Artifact F"; confirmed 2026-11-26)
- [x] Sensen AR hints rendered per eye (`[Stereo] SensenStereo`, see 09 "Artifact D"; confirmed 2026-11-26). Also fixed: temporary view families (screen-bounds checks, DeProject) no longer start extra stereo frames
- [ ] UI panel test (2026-11-26): works for the most part. Open issues:
  - Pause/Sensen menu backdrop (white disc in blue) and Combo Lab UI on a black panel in VR: menus on the flat panel (`[VR] FlatMenus`, see 09 "Artifact H"; confirmed 2026-11-26).
  - ~~Loading screens render the level being streamed in~~: **fixed**, flat panel while `ExamplePlayerController.bOnLoadingScreen` is set (`[VR] FlatLoadingScreens`, see 09 "Artifact E"; confirmed 2026-11-26).
  - ~~Right-eye smoke/fog masks~~: **fixed** (per-view scene-colour copy with view-relative UVs; `[Stereo] FixQuadTextureSize`, see 09 "Artifact G"; confirmed 2026-11-26).
  - ~~Right-eye artifacts~~: light shaft bands **fixed** (`DNEResolution` ignored the right view's offset; `[Stereo] FixDNEResolution`, see [09](09-stereo-artifacts.md)). Water reflections looked fine afterwards; recheck the wedge on Nilin in VR.
  - Character shadow shifted horizontally in opposite directions per eye (left eye left, right eye right). Fits a pass assuming a symmetric projection (our frusta are mirrored-asymmetric). `FSceneView` inverts the projection with a full `D3DXMatrixInverse`, so it's not the view's inverse. Diagnostics added: `[VR] SymmetricProjection` (also a workaround if it fixes it), and `[Stereo] TestFov` to render the flat F10 test with headset-like FOVs for RenderDoc captures (RenderDoc crashes with the OpenXR interop active).

## M5 — Final touches (requested)

- [ ] First-person camera mode
- [x] Free camera mode (confirmed 2026-11-27; since the config menu it's switched on there): `[FreeCam]`, `games/rememberme/free_cam.cpp`. Toggle with F6 or both stick clicks (Index: press both thumbsticks). Starts at the game camera; WASD/E/Q + mouse, or left stick move, right stick look (VR: turn + up/down, movement follows the head), triggers up/down, RB/LB fast/slow. The view point is replaced in `eventGetPlayerViewPoint`, only inside `CalcSceneView` (gameplay, audio and AI keep the game camera), and the look-at fix now applies to any rotation the hook sets (VR eye or free camera), so Nilin as the camera target doesn't pull the view back. The game keeps running but gets no input: the XInput hook hands it a neutral pad, and the viewport client's `InputKey` / `InputAxis` (vtable `+0x14` / `+0x1C` of the client at `UWindowsViewport+0x20` = `FViewport+0x1C`, hooked from `CalcSceneView`'s viewport argument; the first version read `FViewport+0x20`, so the hooks never installed and the free camera got neither mouse look nor kept WASD from the player) swallow everything except key releases; mouse look comes from the swallowed `MouseX` / `MouseY` axes. Confirmed working in game 2026-11-27 apart from the Sensen hints, which moved with the free camera: the game builds them with its own camera, so the game-camera capture for `sensen_stereo` now skips the free camera, and in mono frames with the free camera on the hints are re-projected onto the single view the same way (one view instead of two, game composite tiles replaced by one quad). Hints the game camera can't see are still not drawn (the game culls them), and mono re-projection needs the scene tap (`[VR] Enabled=1`, `StereoSource=scene`). To check in game: level streaming when flying far from Nilin
- [ ] In-game config menu (built 2026-11-27, untested in game; verified under Wine/DXVK with the smoke host: renders, `[Debug] OpenMenuAtStart` / `DumpMenu` / `DumpMenuTab`). Dear ImGui (v1.91.9, D3D9 backend, fetched by CMake; no new DLL imports), `games/rememberme/menu.cpp`. F6 or both stick clicks open/close it (`[Menu]`); the free camera is a checkbox in it (switching it on closes the menu and starts flying). Rendered at Present into a 1280x960 transparent texture (premultiplied alpha), drawn under `scene_tap::Bypass` so the scene/UI hooks ignore it; shown in VR on its own quad layer (`Compositor` `menuTexture`, placed in front of the head at yaw when it opens, `[VR] MenuDistance` / `MenuWidth`) and centred in the game window after the mirror. Input (`input_capture.cpp`, the free camera's hooks generalised): the game gets a neutral pad and no keyboard/mouse while the menu or the free camera has them; after closing, the pad stays neutral until everything is released (Start / trigger don't leak into the game). Mouse moves a software cursor (swallowed `MouseX`/`MouseY`), keyboard arrows/Enter/Escape, gamepad D-pad/A/B, and a laser: `vrcore::xr::Input` now has an aim pose action per hand (located in LOCAL at the frame's display time); the ray is intersected with the menu quad, trigger clicks, thumbstick scrolls. Options: free camera and its speeds; hide player / NPCs / Sensen hints (`[Visibility]`); recenter, world scale, camera orientation, symmetric projection, flat loading screens / menus, HUD, flat panel and menu panel placement; motion blur (the game's value is restored when switched back on), window mirror view and HUD mirror. Save writes them to the INI (`Config::Set*`, WritePrivateProfileString). Headset FOV is not adjustable (it's the headset's)
  - Hide player / NPCs (`hide_actors.cpp`): `PlayerController.HiddenActors` (`+0x604`) is read by `CalcSceneView` (after the controller's vtable `+0x5BC` call) and every component of each listed actor goes into the view's hidden primitives (unless `bIgnoreHiddenActorsMembership`, `+0x140` bit 31); null entries are removed in place. During the per-frame `CalcSceneView` call only, the TArray header is swapped for a copy (game entries + ours, capacity 4096 so in-place edits never reallocate) and restored after. Player = `Controller.Pawn` (`+0x264`) and actors based on / owned by it; NPCs = every other `Engine.Pawn`, rescanned from GObjects once a second, each entry checked against its GObjects slot before use. Shadows may remain
  - Free mouse in the game's menus (`mouse_unlock.cpp`, `[Input] FreeMouseInMenus`): while `bMenuPause` is set or the player controller has no pawn (main menu), and neither the config menu nor the free camera has the input, the cursor is shown and unclipped. Hooks `UWindowsViewport` SetCursorVisible (`0x00C73E60`, ShowCursor loop), UpdateMouseLock (`0x00C73F60`, ClipCursor to the client rect) and UpdateMouseCursor (`0x00C741E0`, cursor type from the viewport client, "none" = `SetCursor(NULL)`); the game's state is restored on leaving the menu. The DirectInput mouse is non-exclusive (cooperative level 6, `FUN_00C73590`, device at `0x0166E498`), so it doesn't confine the cursor
  - Hide Sensen hints: the composite draw (pixel shader with `DNESensenParam1`) is skipped, mono and stereo; needs the scene tap (VR on)
- [x] Valve Index controller support (confirmed 2026-11-27): OpenXR actions (`vrcore::xr::Input`: Index bindings, Touch and simple-controller fallbacks, haptics) presented to the game as player 0's XInput gamepad. The game imports `xinput1_3` by ordinal (2 `XInputGetState`, 3 `XInputSetState`); both are hooked (`games/rememberme/vr_gamepad.cpp`), merged with a real gamepad if connected, rumble goes to the controllers. Mapping: sticks (+click), triggers = LT/RT, grips = LB/RB (`squeeze/force`; `squeeze/value` is finger curl and held them down), right A/B = A/B, left A/B = X/Y, right trackpad press = Start, left trackpad press = Back (centre) / D-pad (edges); Touch menu = Start. `[Input] VRControllers`, `GripThreshold`. The sticks first only worked once a real gamepad was plugged in: the game accepts gameplay stick axes and triggers only from pads its DirectInput enumeration registered as XInput devices (at startup and on every `WM_DEVICECHANGE`), and the VR controllers exist only at the XInput level. Fix: whenever no XInput pad is registered, a virtual Xbox 360 pad is fed to the game's enumeration callback, with its WMI IsXInputDevice check hooked to accept it (details in 08, "Gamepad input"). Controller input only arrives while the headset is worn (SteamVR unfocuses the session otherwise). Assessment: fine for looking around; proper gameplay still wants a gamepad or mouse and keyboard, so the mapping stays as is.

## M6 — Performance (only if needed)

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
