# 09 — Per-eye rendering artifacts (investigation log)

Status as of 2026-11-26. Stereo, depth, head tracking and resolution work (M3 passed). What's left is passes that misbehave when two views share one view family. This page records what's known, how it was found, and where to resume.

## Artifact list

| # | Artifact | Eyes | Status |
|---|---|---|---|
| A | Light/fog effect cut by horizontal bands (thickness varies with view angle) | right only | **Fixed** (confirmed in game 2026-11-26, flat test): `DNEResolution` ignored the view offset (below). `[Stereo] FixDNEResolution` |
| B | Character shadow offset horizontally, opposite directions per eye (not under the feet) | both, mirrored | Reproduces in the flat F10 test with `TestFov` when symmetric projection is off. Not yet analysed in a capture |
| C | Blue/shadow wedge on Nilin; puddle reflection band | right | Probably the same cause as A: no water-reflection issues seen with the fix (2026-11-26, brief flat test). Recheck the wedge on Nilin and puddles in VR |
| D | Sensen AR hints: left eye only, misplaced, follow head roll | left | **Fixed** (confirmed in game 2026-11-26, flat test): per-eye redraw from the game camera, both views composited (`[Stereo] SensenStereo`), see "Artifact D" below |
| E | Loading screen shows the level around the tips panel | both | **Fixed** (confirmed in game 2026-11-26): mono (flat panel) while `ExamplePlayerController.bOnLoadingScreen` is set (`[VR] FlatLoadingScreens`), see below |
| F | Monitor mirror has no UI in stereo frames | — | **Fixed** (confirmed in game 2026-11-26): UI drawn into the window too, window shows the left eye in VR (`[VR] MirrorUi`, `MirrorView`), see below |
| H | Pause/Sensen menu backdrop is a white disc in blue; Combo Lab UI on an opaque black panel (VR only) | both | **Fixed** (confirmed in game 2026-11-26): menus on the flat panel (`[VR] FlatMenus`), see below |
| G | Smoke particles cut off, depth fog with a hard mask | right | **Fixed** (confirmed in game 2026-11-26, flat test, f90a807): per-view scene-colour copy sampled the wrong region (below). `[Stereo] FixQuadTextureSize` |

## Reproduce and capture without VR

RenderDoc crashes with the OpenXR interop active, so capture the **flat F10 test** instead:

1. Log the headset's FOV: start in VR with `[VR] SymmetricProjection=0` and read the `eye FOV (left,right,up,down deg)` log line (the compositor's own line prints only horizontal angles and is no use). Index values seen: `L -57.0,46.5,54.7,-54.6  R -46.4,57.0,54.6,-54.6`.
2. Close SteamVR, set `[Stereo] TestFov=-57.0,46.5,54.7,-54.6` (left eye; right is mirrored; keep the sign of `down`), start the game, press F10.
3. Capture in RenderDoc. Both eyes render side by side into one target (4736×2632 at window 2368×1314, `SuperSampleCount=2`).

Convert for scripting: `renderdoccmd convert -f x.rdc -c xml -o $CLAUDE_JOB_DIR/tmp/x.xml` (takes seconds), then `re/scripts/passes.py` and `re/scripts/viewports.py`. The Arch `renderdoc` package ships no standalone Python module, but `qrenderdoc --python` runs scripts in its embedded one with the full replay API (pixel history, shader debugging, texture reads). `tools/renderdoc/rdrun.sh <script.py> <capture.rdc> [args]` wraps it: the script gets `rd`, `ctl` (ReplayController) and `args`, its output goes to the terminal, and qrenderdoc exits before its window opens (needs the display session and the GPU; the 1.1 GB capture replays in ~2 s). Scripts: `summary.py` (find draws by name), `passes.py` (one line per draw: pass, shaders, target, viewport, scissor), `describe.py`/`drawinfo.py` (state of draws), `usage.py` (every event using a resource), `pixeltrace.py` (full-frame pixel history with pass/shader labels), `history.py`, `rejectmap.py`, `valuemap.py`, `halfstats.py`, `column.py` (texture value maps and per-half statistics), `d3d9consts.py` (a DXVK draw's D3D9 `c[]` constants, read through the push-data buffer address), `apicalls.py` (raw API call parameters), `savetex.py` (texture to PNG), `debugpixel.py` (shader debugging; doesn't work on DXVK shaders with RenderDoc 1.46). Event IDs are RenderDoc's, not the pass numbers.

### RenderDoc UI numbering
DXVK labels render passes "Render pass N (0: Image (x), DS: Image (y))". That N is **one higher** than the pass index printed by our scripts (the init clear counts as pass 0). Image ids in the UI (871, 891, …) also differ from the `#nnnn` ids in the XML. Match by attachments and the `[vs.xxx] [fs.xxx]` labels instead.

### Dumping DXVK shaders
Launch option `DXVK_SHADER_DUMP_PATH=/home/usr/dxvk-dump %command%` writes `<type>.<hash>.sm3_dxbc` and `.spv` for every shader (the label hash is the first 7 hex digits). It keeps writing for as long as the game runs, so it fills quickly. Disassemble with `spirv-dis`. The DXBC contains a constant table (`strings` shows the parameter names).

## Artifact A: cause (2026-11-26)

**DONTNOD's material parameter `DNEResolution` doesn't include the view's offset in the render target.** It is (2/SizeX, −2/SizeY, −1, 1), and material pixel shaders turn the pixel position into clip space with `ndc = (vPos − 0.5) · DNEResolution.xy + DNEResolution.zw`, then into a scene-texture UV with `ScreenPositionScaleBias`. For the left eye (X = 0) that's right. For the right eye (X = SizeX) `ndc.x` runs 1…3 instead of −1…1, the UV lands at 1.0–1.5, and the clamp sampler returns the last column of the scene colour. Depth-faded effects then read one depth value per row: horizontal bands that move with the view, right eye only.

Found in `lighting_band_artifact.rdc` (flat F10 test, 4736×2632) with the scripted RenderDoc tools (`tools/renderdoc/`):
- The final image shows a dark band with hard horizontal edges in the right half (y ≈ 280–430, x ≈ 3250–4290). Pixel history of an in-band pixel (3848,355) and one above it (3848,237) on the scene colour: they part at **EID 14136, render pass 70, `[vs.a80b33a] [fs.2392be5]`**, an additive fog/light volume material (VS constants `FogInScattering`, `SharedFogParameter*`, `MeshOrigin`…). It adds ~0.046 above the band and exactly 0 inside it.
- Its PS constants (read from DXVK's constant buffer via the push-data address; the shader can't be stepped: RenderDoc 1.46 doesn't support `SPV_KHR_float_controls2`): `c0 DNEResolution = (1/1184, −1/1314, −1, 1)` for a 2368×2628 view at x=2368; `c1 ScreenPositionScaleBias = (0.25, −0.49924, 0.49943, 0.75011)` (correct for the right eye); `c2 MinZ_MaxZRatio`; `c3 NvStereoEnabled = 0`.
- The SPIR-V (`fs.2392be5…spv`, `spirv-dis`): `ndc = (FragCoord − 0.5)·c0.xy + c0.zw`, `uv = ndc/w · c1.xy + c1.wz`, `sceneDepth = c2.x / (SceneColor(uv).a − c2.y)`, `fade = saturate((sceneDepth − w) / 160)`, output = fog colour · fade, additive.
- `DNEResolution` is bound for every material shader in `FUN_0096DB10` (DONTNOD's additions to the material parameters: `CameraWorldPos` +0x0, `DNEResolution` +0x6, `TemporalAAParameters` +0xC, …). The function that fills it wasn't found yet.

**Fix (confirmed in game 2026-11-26):** `[Stereo] FixDNEResolution=1` (`games/rememberme/device_state_hooks.cpp`). When a vertex- or pixel-shader constant equals (2/W, −2/H, −1, 1) for the current viewport W×H and the viewport doesn't start at 0,0, its z/w become −1 − 2X/W and 1 + 2Y/H. Mono frames and the left eye are untouched. A cleaner fix would patch the engine's setter once it's found.

### Earlier wrong lead: the half-res depth downsample

The previous localisation to pass 61 (`FUN_00A60D30`, `DownsampleSceneDepth`) was wrong. Image 891 (ResourceId 9569) looked banded in its right half at pass 61, but **pass 61 doesn't write colour** (write mask 0); the image holds leftovers from the previous frame's per-eye half-res colour passes 75/94 (`fs.758ca32`). Pixel history shows both eyes' downsample draws write exactly the shader's output into the half-res depth (9536), and the `DepthDownsampleLog` run showed correct per-eye constants. What was learned there:

- `FUN_00A60D30` is UE3's `DownsampleSceneDepth` (renderer in ECX; called from `FUN_00A81590+639`), found via `L"FDownsampleSceneDepthPixelShader"` → static init `0x0120282D` → type object `0x016576C4`. It loops over `TArray<FViewInfo>` at `renderer+0x5C`/`+0x60` (stride `0x1210`) with `SetParameters` inlined: `ProjectionScaleBias` = `View+0xC8`/`+0xD8` (projection matrix at `View+0xA0`, confirmed by the log), buffer-size texel offsets, and `FUN_00A48170` (scene-texture parameters; `View+0x53C != 0` selects sampler `DAT_01657C3C`; always 0 in the log). Viewport `View+0x40..0x4C` (X, Y, SizeX, SizeY).
- `[Debug] DepthDownsampleLog=N` logs that pass per eye (`games/rememberme/renderer_hooks.cpp`, through the shared device-state tracing).
- The half-res depth 9536 is never cleared; the downsample writes with depth test "always", so that's harmless.

### Frame structure notes (flat stereo capture)

- Per eye, the post chain runs once over the full target with the eye's viewport: half-res colour downsample (pass 75 / 94, `fs.758ca32`, writes only its half), blurs at 1186×660 (78–83, 86–91 / 97–102, 105–110), apply (92 / 111, `fs.2e09260`, full-width draw that only changes its own half), then 93 / 112 (`fs.08983d1`, per-half viewport).
- Light/fog cones at half res (pass 74, `[vs.bbbd8f4]`, 8 draws per eye into 9572, depth test ≤ against 9536) have per-eye viewports and scissors, both correct.

### Next steps

1. ~~Test `FixDNEResolution=1`~~: bands gone in the flat test. Still to do: a longer VR run.
2. Recheck artifact C (wedge on Nilin, puddle band) in VR; no reflection issues seen so far.
3. Optional: find the engine function that fills `DNEResolution` (it reads the material parameter at +0x6 of the struct bound in `FUN_0096DB10`) and add the view offset there instead.

## Artifact D: Sensen AR hints (2026-11-26)

From `in_world_hint_stereo.rdc` (flat F10 test, 1880×2096 scene, SuperSampleCount 1):

- **Hint layer, pass 110 (EIDs 6726–6954):** Scaleform draws the hints into an offscreen layer (ResourceId 9535, scene-sized, scene depth attached) with **View[0]'s viewport only** (x=0, w=940). The pass clears the whole layer (`loadOp = Clear` over 1880×2096). Shaders `vs.e16bfa9`/`fs.099af47`, `vs.74cd077`/`fs.4020d7e`, `vs.22a5c61`/`fs.127d52f`; every VS has one constant **`Transform` (c6–c9)**.
- **`Transform` = (movie-to-world affine) × the *game camera's* translated view-projection** (mono: the game's FOV and full-viewport aspect), even in stereo, where it's drawn into View[0]'s viewport. That's why the hint sits at the same *normalised* screen position as in mono ((0.16, 0.55) in the left eye vs (0.18, 0.53) mono in `in_world_hint_stereo_offset.rdc` / `in_world_hint_mono_correct.rdc`), i.e. "way above the ledge" with the wider stereo FOV. Clip-space vertices are nearly identical between the two captures. (A first test, `Transform · inverse(VP_left)` being affine, was misleading: a perspective matrix's w column depends only on the camera's forward axis, so any FOV passes it.) Per eye: `T_eye = T · inverse(TVP_game) · Translate(o_game − o_eye) · TVP_eye`. Checked offline: with the mono capture's VP as the game camera, the corrected hint lands on the ledge in the stereo frame.
- **Composite, pass 111 (EIDs 6983–7004; pass 107, EID 12391, a single tile, in the newer capture):** `vs.83471f9`/`fs.3060a90` (`DNESensenParam1/2`, `DNEOverlayColor`, `NoiseTexture`, the layer bound as `SceneColorTexture`) blends tiles of the layer over the scene (`One`, `InvSrcAlpha`). Tiles are a 6×5 grid over **one view's rect**, chosen by a bitmask of tiles with hint content (`FUN_00EC5D30`, drawn with `DrawDenormalizedQuad` `FUN_00A35FC0`: float4 clip position (with D3D9's −0.5-pixel offset) + float2 UV = pixel / viewport size. Not necessarily a client-memory draw at the D3D9 level). The shader samples by texture coordinate and **discards** pixels with (almost) no hint content, so compositing extra area is harmless.
- Composite entry points: `FUN_00ECBE50`/`FUN_00ECBF70` set a viewport from a rect and call `FUN_00ECB560`/`FUN_00ECBA30`, which set up the Sensen texture shaders and call the tile loop.
- **Screen-space checks build temporary view families.** `FUN_00D50C00` (called from `FUN_00E641E0` "object covers X% of the screen" and `FUN_00D5C1B0` "screen positions of a list of actors") creates a family and calls `CalcSceneView`, then projects a bounding box. So do `FUN_008635C1` (`ULocalPlayer::execDeProject` & co.) and `FUN_006F1189`. **Our hook used to treat these like the per-frame call:** in VR it built two eye views in the temporary family and queued an extra `SubmitStereoFrame`, shifting the poses submitted with later frames (a possible source of the choppiness noted in 06). Fixed: only the call from `UGameViewportClient::Draw` (return address `0x0086DEB5`) starts a stereo frame; the others get one view with the current left eye's pose and FOV.

**Fix:** `[Stereo] SensenStereo=1` (`games/rememberme/sensen_stereo.cpp`).
- Game thread: per stereo frame, `CalcSceneView` is called once in matrices-only mode (the unmodified game camera) and the matrices are queued, keyed by that frame's left-eye `ViewMatrix`.
- Render thread: `renderer_hooks` (`DownsampleSceneDepth`, before the hint pass) reads both views (rect, `View+0x60` view matrix, `View+0xA0` projection), finds the frame's game camera and computes `C_left`, `C_right`.
- A draw observer (`vrcore::scene_tap::SetDrawObserver`, called before each non-UI draw; it can replace the draw) sees the hint draws: vertex shader with a `Transform` constant (from the shader's CTAB, parsed at `CreateVertexShader`), View[0]'s viewport, not the backbuffer. Each is drawn once per eye with `Transform · C_eye` and the eye's viewport instead of the game's single draw (flat transforms: same transform, both viewports).
- Composite: the game's tiles are placed where *its* camera sees the hints, so with the hints moved per eye they can miss the left-eye hint (`hint_only_on_right_stereo_view.rdc`: tile at y 526–1051, corrected left hint at y ≈ 1128; the hint appeared only in the right eye until the camera was turned). In stereo frames the first composite draw (pixel shader with `DNESensenParam1`, any draw kind) is replaced by one quad per view (UV = pixel / viewport, stream 0 restored) and the game's other tiles are skipped.
- Limitation: if the game considers a hint off-screen for its own (mono) camera and skips the hint or composite draws, there's nothing to redraw. Only possible near the edges of the view.
- First try (2026-11-26, untested version 1873037): drew only the right eye with `C = inverse(TVP_left)…`, assumed client-memory composite tiles. Result: left hint misplaced (the game's), right hint drawn into the layer but never composited.

Next: test in the flat F10 test (hints should appear in both halves at the right depth) and in VR (placement, head roll); check that DeProject-based things (e.g. aiming/targeting reticles, if any) still line up with the left eye.

## Artifact G: smoke and fog masks (2026-11-26)

`depth_fog_and_smoke_effect_weird_mask_stereo.rdc` (flat F10 test, 4736×2632): vent smoke cut off and a hard-edged mask in the depth fog, right eye only.

- The smoke (`[vs.59e20dc] [fs.122d029]`, left EID 10248, right 11206) has correct per-eye constants (`DNEResolution` corrected, `ScreenPositionScaleBias` bias 0.75, per-eye view-projection and `LocalToWorld`) and the same geometry in both eyes. A scan of all right-eye draws' constants (`tools/renderdoc/scanconsts.py`) found nothing real: its hits were stale registers past each shader's constants (check against the CTAB).
- **The depth it fades against is wrong.** Soft particles and fog read scene depth from the alpha of a scene-colour copy (9540). The scene colour (9541) is copied full-width before translucency (EIDs 9364, 9471); between the eyes' translucency the renderer copies **a sub-rectangle per view** (EIDs 10317 left, 10322 right: the screen bounds of a translucent primitive that needs scene colour, here eye pixels x 0–1904, y 1054–1917). Those copies use `DrawDenormalizedQuad` with **view-relative U/V and the viewport size as the texture size**: u = 0…0.804 (= pixel / 2368), while the texture is 4736 wide. Each copy therefore reads columns 0–3808 of the buffer, stretched over the rect. In the right eye the depth in that rect becomes stretched left-eye content (alpha 0.047 instead of 0.016, i.e. ~200 uu instead of ~650), so the smoke and fog fade against a wall that isn't there. The left eye's smoke ran before its bad copy, so it was unaffected.
- First fix attempt (b153688, hooking `DrawDenormalizedQuad` `FUN_00A35FC0`): didn't apply. `quad_texture_bug.rdc` shows the copies (EIDs 10067/10072) are **batched-element tiles** (48-byte vertices: float4 position, float2 UV, float4 colour, colour), not `DrawDenormalizedQuad`'s 24-byte quads, with u = 0.068…0.818 = (x+1)/2 still. It also hit a legitimate 256→128 downsample (caller `0x00B95D3B`) in mono. Removed.
- Second attempt (0ef2ab6): any client-memory quad into *part* of a render target, sampling a texture of the target's size, with viewport-relative UVs. **Broke the whole image, menus included** (`borked.rdc`): UE3's post-process chains (downsample, bloom, eye adaptation: 256→128→64… into corner viewports of same-size pooled targets) legitimately use viewport-relative UVs and match that rule in mono too.
- **Fix (built, untested in game):** `[Stereo] FixQuadTextureSize=1` (`games/rememberme/draw_fixes.cpp`): the same test, but only for draws whose viewport is **exactly the right view's rect** of a stereo frame (rects recorded by `renderer_hooks` at `DownsampleSceneDepth`, which runs before the copies; cleared at Present). Rewrite: `u' = (vp.X + u·vp.W) / texW`, `v' = (vp.Y + v·vp.H) / texH`. `tools/renderdoc/uvrule.py` lists the draws the rule would rewrite: in `quad_texture_bug.rdc` exactly one, the copy (EID 10072). The log prints "viewport-relative UV fix active" on the first fix.
- That version (3518561) turned F10 into one wide stretched view (`borked_mono.rdc`): the full-screen copies at EIDs 9737/9870 (viewport x=0, w=4736) were rewritten to u = 0.5…1. **`device_state` tracked the viewport only through `SetViewport`, but D3D9's `SetRenderTarget` resets it to the whole new target**, so after a render-target change the "current viewport" was still the right eye's. Fixed: `device_state::CurrentViewport()` reads the device's viewport (`GetViewport`). The `DNEResolution` fix and `sensen_stereo` used the same stale value and now read it too.
- `draw_fixes` is now the game's draw observer and dispatches to `sensen_stereo` first.

## Artifact E: loading screens (2026-11-26)

During loading the game keeps rendering the level being streamed in, and a white tips panel (UI) covers it on a monitor. In VR the level surrounds the player in stereo and the tips are on the UI panel (`debug/loading_screen.jpg`).

- Signal: **`ExamplePlayerController.bOnLoadingScreen`** (packed bool in the word at +0x6E8, with `bOnMainMenu`, `bSignInFromPressStart`, `bIsTutoOpen`, `bShouldTogglePause`). Alternatives seen in the type dump: `PauseMenu.bLoadingScreenMode` / `SensenMenu.bLoadingScreenMode` (+0x240), `GameEngine.bStartedLoadMapMovie`, `WorldInfo.bRequestedBlockOnAsyncLoading`.
- `vrcore::ue3::Reflection::BoolProperty` resolves a bool's word offset and bit (`UBoolProperty::BitMask`, the first field after `UProperty`, like `UObjectProperty::PropertyClass` at +0x80).
- **Fix (confirmed in game 2026-11-26):** `[VR] FlatLoadingScreens=1`. The `CalcSceneView` hook reads the flag from `LocalPlayer.Actor` each frame; while set, the frame renders mono (no eye views, no stereo frame), so the compositor shows its flat panel with the game's normal image (tips panel covering the level, as on a monitor), and the F10 test stays mono. The log prints the flag's location at startup and "loading screen on/off" on every change.

## Artifact F: monitor mirror (2026-11-26)

In stereo frames every UI draw goes to the UI texture (its own VR panel), so the window showed the scene without UI, and in VR it showed both eyes side by side.

**Fix (confirmed in game 2026-11-26):**
- `[VR] MirrorUi=1` (`vrcore::scene_tap::EnableUiMirror`): each redirected UI draw is drawn a second time into the backbuffer, where the game put it (the left view's rect), with the game's own blending (the premultiplied-alpha patch only applies to the UI texture). The F10 flat test shows its UI again too.
- `[VR] MirrorView=left|stereo` (`games/rememberme/mirror.cpp`): while VR runs in stereo (`Compositor::StereoActive()` and a two-view frame), `left` rearranges the backbuffer after the compositor has copied the eyes: the left half (left eye + UI) is copied to a temporary surface, the backbuffer cleared to black and the left half put back centred (pillarboxed; `StretchRect`/`ColorFill` ignore the game's state). `stereo` keeps both eyes. Mono frames and the F10 test are unchanged.

## Artifact H: menus (2026-11-26)

- **Pause/Sensen menu backdrop** (`pause_menu_blue.rdc`): the menu's background is a 3D scene, a white cloudy backdrop object around the camera, rendered per eye, with the blue clear colour wherever it doesn't reach. It fills the screen at the game's FOV; with the headset's much wider FOV the edges show (a white disc in blue). The UI itself is fine.
- **Combo Lab black panel, VR only** (`combo_lab.rdc`, F10 test, but the UI texture the VR panel would get is in the capture): the UI texture's alpha is opaque everywhere but the character silhouette. The cause is tile composites (`[vs.83471f9] [fs.c63bd4a]`, e.g. EID 9461, a full-size layer onto the screen) that blend **`One` / `SrcAlpha`**: their alpha is *transmittance* (1 = see-through). scene_tap's UI-texture alpha patch assumes *coverage* (`A = As + Ad·(1−As)`), so every see-through tile made the texture opaque, with black colour. On the monitor the game's own blending is right. (RenderDoc's `BlendMultiplier` 6 = `SrcAlpha`, 7 = `InvSrcAlpha`.)
- A UI texture that tracks transmittance (`T' = T · f`, f = the draw's destination factor) handles those tiles but not blend-off draws (Scaleform clears its stage with a blend-off full-screen draw writing (0,0,0,0), which needs `T = 1 − As`); each convention misses one case without extra passes. Not done.
- **Fix (confirmed in game 2026-11-26):** `[VR] FlatMenus=1`: while `ExamplePlayerController.bMenuPause` is set, frames render mono (like loading screens: the same mechanism, now a list of controller flags), so menus are on the flat panel as on a monitor and never use the UI texture. The log prints "menu on/off".

## Artifact B: notes

- Shadow offset is visible in the flat test with `TestFov` and symmetric projection **off**; `SymmetricProjection=1` is the diagnostic (result of an in-VR comparison not recorded yet).
- Shadow depth maps are 2048² (passes ~50–58 in the capture: `R32_SFLOAT` + `D32`), rendered with viewport 1024×512 at (6,6) for the spot shadows. The shadow projection into the light-attenuation buffer happens in the full-res passes (BGRA8 target, per eye viewport), so the likely culprit is per-view data (projection or view-projection constants) used when projecting the shadow back onto the scene, not the shadow maps.
- To do: capture the shadow spot in the flat test (`TestFov` on) and compare the projective-shadow draws between the eyes (viewports, constants if readable).
