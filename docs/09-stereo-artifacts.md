# 09 — Per-eye rendering artifacts (investigation log)

Status as of 2026-11-26. Stereo, depth, head tracking and resolution work (M3 passed). What's left is passes that misbehave when two views share one view family. This page records what's known, how it was found, and where to resume.

## Artifact list

| # | Artifact | Eyes | Status |
|---|---|---|---|
| A | Light/fog effect cut by horizontal bands (thickness varies with view angle) | right only | **Fixed** (confirmed in game 2026-11-26, flat test): `DNEResolution` ignored the view offset (below). `[Stereo] FixDNEResolution` |
| B | Character shadow offset horizontally, opposite directions per eye (not under the feet) | both, mirrored | Reproduces in the flat F10 test with `TestFov` when symmetric projection is off. Not yet analysed in a capture |
| C | Blue/shadow wedge on Nilin; puddle reflection band | right | Probably the same cause as A: no water-reflection issues seen with the fix (2026-11-26, brief flat test). Recheck the wedge on Nilin and puddles in VR |
| D | Sensen AR hints: left eye only, misplaced, follow head roll | left | Needs the per-eye Sensen work (see 08) |
| E | Loading screen shows the level around the tips panel | both | Needs loading detection |
| F | Monitor mirror has no UI in stereo frames | — | Open |

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
- `[Debug] DepthDownsampleLog=N` logs that pass per eye (now through the shared device-state tracing).
- The half-res depth 9536 is never cleared; the downsample writes with depth test "always", so that's harmless.

### Frame structure notes (flat stereo capture)

- Per eye, the post chain runs once over the full target with the eye's viewport: half-res colour downsample (pass 75 / 94, `fs.758ca32`, writes only its half), blurs at 1186×660 (78–83, 86–91 / 97–102, 105–110), apply (92 / 111, `fs.2e09260`, full-width draw that only changes its own half), then 93 / 112 (`fs.08983d1`, per-half viewport).
- Light/fog cones at half res (pass 74, `[vs.bbbd8f4]`, 8 draws per eye into 9572, depth test ≤ against 9536) have per-eye viewports and scissors, both correct.

### Next steps

1. ~~Test `FixDNEResolution=1`~~: bands gone in the flat test. Still to do: a longer VR run.
2. Recheck artifact C (wedge on Nilin, puddle band) in VR; no reflection issues seen so far.
3. Optional: find the engine function that fills `DNEResolution` (it reads the material parameter at +0x6 of the struct bound in `FUN_0096DB10`) and add the view offset there instead.

## Artifact B: notes

- Shadow offset is visible in the flat test with `TestFov` and symmetric projection **off**; `SymmetricProjection=1` is the diagnostic (result of an in-VR comparison not recorded yet).
- Shadow depth maps are 2048² (passes ~50–58 in the capture: `R32_SFLOAT` + `D32`), rendered with viewport 1024×512 at (6,6) for the spot shadows. The shadow projection into the light-attenuation buffer happens in the full-res passes (BGRA8 target, per eye viewport), so the likely culprit is per-view data (projection or view-projection constants) used when projecting the shadow back onto the scene, not the shadow maps.
- To do: capture the shadow spot in the flat test (`TestFov` on) and compare the projective-shadow draws between the eyes (viewports, constants if readable).
