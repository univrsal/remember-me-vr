# 09 — Per-eye rendering artifacts (investigation log)

Status as of 2026-11-26. Stereo, depth, head tracking and resolution work (M3 passed). What's left is passes that misbehave when two views share one view family. This page records what's known, how it was found, and where to resume.

## Artifact list

| # | Artifact | Eyes | Status |
|---|---|---|---|
| A | Light/fog effect cut by horizontal bands (thickness varies with view angle) | right only | **Localised to the half-res depth downsample** (`FUN_00A60D30`, below); per-eye logging added, cause not found |
| B | Character shadow offset horizontally, opposite directions per eye (not under the feet) | both, mirrored | Reproduces in the flat F10 test with `TestFov` when symmetric projection is off. Not yet analysed in a capture |
| C | Blue/shadow wedge on Nilin; puddle reflection band | right | Probably the same family as A; not yet checked against the half-res depth |
| D | Sensen AR hints: left eye only, misplaced, follow head roll | left | Needs the per-eye Sensen work (see 08) |
| E | Loading screen shows the level around the tips panel | both | Needs loading detection |
| F | Monitor mirror has no UI in stereo frames | — | Open |

## Reproduce and capture without VR

RenderDoc crashes with the OpenXR interop active, so capture the **flat F10 test** instead:

1. Log the headset's FOV: start in VR with `[VR] SymmetricProjection=0` and read the `eye FOV (left,right,up,down deg)` log line (the compositor's own line prints only horizontal angles and is no use). Index values seen: `L -57.0,46.5,54.7,-54.6  R -46.4,57.0,54.6,-54.6`.
2. Close SteamVR, set `[Stereo] TestFov=-57.0,46.5,54.7,-54.6` (left eye; right is mirrored; keep the sign of `down`), start the game, press F10.
3. Capture in RenderDoc. Both eyes render side by side into one target (4736×2632 at window 2368×1314, `SuperSampleCount=2`).

Convert for scripting: `renderdoccmd convert -f x.rdc -c xml -o $CLAUDE_JOB_DIR/tmp/x.xml` (takes seconds), then `re/scripts/passes.py` and `re/scripts/viewports.py`. Pixels can't be read from scripts here (the Arch `renderdoc` package ships no Python module), so the texture inspection is done by hand in the RenderDoc UI.

### RenderDoc UI numbering
DXVK labels render passes "Render pass N (0: Image (x), DS: Image (y))". That N is **one higher** than the pass index printed by our scripts (the init clear counts as pass 0). Image ids in the UI (871, 891, …) also differ from the `#nnnn` ids in the XML. Match by attachments and the `[vs.xxx] [fs.xxx]` labels instead.

### Dumping DXVK shaders
Launch option `DXVK_SHADER_DUMP_PATH=/home/usr/dxvk-dump %command%` writes `<type>.<hash>.sm3_dxbc` and `.spv` for every shader (the label hash is the first 7 hex digits). It keeps writing for as long as the game runs, so it fills quickly. Disassemble with `spirv-dis`. The DXBC contains a constant table (`strings` shows the parameter names).

## Artifact A: findings

Frame structure (flat stereo capture, DXVK pass numbers):

- Pass 60: snapshot copy of the scene colour (Image 871, fullscreen `vs.6d4dc7a`/`fs.3f4b750`). Clean.
- **Pass 61: half-res depth downsample, two draws, one per eye** (`[vs.3480169] [fs.2dd476a]`), into a 2368×1316 target (Image 891) with its own depth buffer (Image 870). **Image 891 is banded**, and only in the right half (left half and the plain game are clean).
- Pass 62+: more full-res work; the snapshot copies at passes 72/73 are banded as well.
- The half-res depth then drives a half-res pass (my script's pass 73, 2368×1316 BGRA8, 8 draws per eye), probably fog/light shafts, so the bands propagate from the depth.

`fs.2dd476a` (disassembled): reads the **alpha channel of the scene colour** (scene depth in view space), converts each of four samples with `1/(alpha − MinZ_MaxZRatio.y)`, takes the max, applies `ProjectionScaleBias`, and writes the result as depth (`DepthReplacing`); colour output is 0. Constants: `ProjectionScaleBias`, `MinZ_MaxZRatio`, `SourceTexelOffsets01`, `SourceTexelOffsets23` (c0..c3; this is UE3's `FDownsampleSceneDepthPixelShader`).

Verified **not** the cause:

- Viewports and scissors: per eye 1184×1314 at x=0.5 / x=1184.5, y=1314.5 with negative height (the D3D9 half-pixel offset), identical for both eyes.
- The quad vertices (CSVs from the Mesh Viewer): positions identical; texcoord u runs 0→0.5 (left) and 0.5→1.0 (right), v runs 0→0.99848 (= 2628/2632) in both. Both correct.
- The shader itself (simple, no per-eye logic).
- Scene depth in alpha: visually clean in both eyes (range 0–0.026 saturates, so subtle banding there isn't excluded).
- The projection: bands are present with `SymmetricProjection` on and off.
- The game, unmodified: no bands. Stereo off (F10): gone. Left view in stereo: clean.

~~Remaining suspect: the pixel-shader constants of the right-eye draw~~ **ruled out 2026-11-26** (see "Per-eye constants: result" below). Original reasoning: **the pixel-shader constants of the right-eye draw** (the two draws use different constant buffers, 64 bytes apart; their contents aren't in the capture). `ProjectionScaleBias` and `MinZ_MaxZRatio` come from the view's projection matrix in UE3, so a right-eye matrix with unexpected terms would convert depth wrongly in that eye only. `SourceTexelOffsets*` come from buffer sizes (shared). Not proven.

### Where the code is (Ghidra, `0x00400000` base)

- Shader class constructor `FUN_00a2b7e0` (vtable `0x013a0580`, size 0xC0, binds `ProjectionScaleBias`, `SourceTexelOffsets01/23`, plus the base `FUN_00a28c10`: `SceneColorTexture`, `MinZ_MaxZRatio`, `ScreenPositionScaleBias`, …). It's created by `FUN_00a36d90` (registered from static init code around `0x0120284b`).
- **Found (2026-11-26): `FUN_00A60D30` is `DownsampleSceneDepth`** (renderer in ECX; called from `FUN_00A81590+639`). Route: the name string `L"FDownsampleSceneDepthPixelShader"` → its static init `0x0120282D` → type object `0x016576C4` → the `TShaderMapRef` lookup in `FUN_00A60D30`. It loops over the renderer's views (`TArray<FViewInfo>` at `renderer+0x5C`/`+0x60`, stride `0x1210`) with `SetParameters` **inlined**:
  - `ProjectionScaleBias` = (`View+0xC8`, `View+0xD8`), i.e. `M[2][2]`, `M[3][2]` of the projection matrix if it sits at `View+0xA0` (our `FPerspectiveMatrix` hook doesn't touch those terms).
  - `SourceTexelOffsets01` = (0, 0, 1/W, 0), `SourceTexelOffsets23` = (0, 1/H, 1/W, 1/H) from the global buffer size / downsample factor (`DAT_0165785C`, `DAT_01657860`, `DAT_01657884`): same for both eyes.
  - `FUN_00A48170(View, shader, …)` sets the scene-texture parameters. `MinZ_MaxZRatio` goes through RHI vtable `+0x1C8` with the view. **The scene colour's sampler depends on the view:** `View+0x53C != 0` binds sampler `DAT_01657C3C` instead of the caller's (`&DAT_016578DC`). The ctor and InitViews (`FUN_00A745B0`) zero `+0x53C` per view; no direct `[reg+0x53C]` writer in the renderer was found, so it's set via another base pointer. The `debug/*push_constants.csv` dumps of the two pass-61 draws differ in one word (`0` vs `0x522`) besides the 64-byte constant offset, which may be a different sampler index in DXVK.
  - Viewport: `View+0x40..0x4C` (X, Y, SizeX, SizeY) / factor; quad via `FUN_00A35FC0` (DrawDenormalizedQuad).
- **Diagnostic:** `[Debug] DepthDownsampleLog=N` (`games/rememberme/depth_downsample_diag.cpp`) hooks `FUN_00A60D30` and logs, for the next N two-view calls, each view's rect, projection rows and `+0x53C`, then every `SetPixelShaderConstantF`/`SetSamplerState`/`SetTexture`/`SetViewport`/`SetPixelShader` inside the pass. F10 re-arms it. The UE3 RHI may skip redundant state sets, so a state that appears only between the two `SetViewport` lines differs between the eyes.

### Per-eye constants: result (2026-11-26)

`DepthDownsampleLog=3`, flat F10 test, `TestFov=-57.0,46.5,54.7,-54.6` with `SymmetricProjection=1`: 23 stereo calls logged, all consistent. **Both eyes get correct inputs:**

| D3D9 reg | Parameter | Left | Right |
|---|---|---|---|
| c0 | ProjectionScaleBias | (0.999, −9.99) | identical |
| c2 | MinZ_MaxZRatio | (9.99, 0.000999987, −0.1001001, −0.1) | identical |
| c7, c8 | SourceTexelOffsets01/23 | (0, 0, 1/2368, 0), (0, 1/1316, 1/2368, 1/1316) | identical |
| c1 | ScreenPositionScaleBias | (0.25, −0.49924, 0.49943, **0.25011**) | (…, **0.75011**): correct for x=2368 |
| c6 | camera position | eye | 6.4 uu away (= `EyeSeparation`) |
| s0 | scene colour | 4736×2632 `A16B16G16R16F`, point, clamp | identical |

Viewports: (0,0,1184,1314) then (1184,0,1184,1314). `View+0x53C` is 0 in every view, so the alternate sampler isn't used. The projection rows confirm the matrix at `View+0xA0` (here the symmetric 57°/54.7° frustum). So the depth conversion is the same in both eyes; the push-data difference in `debug/` isn't a constant or sampler difference at the D3D9 level.

What's left is the **data** the right-eye draw reads or the **state** it writes with (depth/stencil test, not logged):
- the right half of the scene colour's alpha (scene depth) could already carry the bands, below the 0–0.026 range checked visually;
- the draw's depth/stencil test could reject rows in the right half, leaving stale/cleared values.

### Next steps

1. ~~Find `SetParameters` and compare the per-eye constants~~: done, identical/correct (above).
2. In `lighting_band_artifact.rdc`, pass 61: **Pixel History** on a banded pixel in the right half of Image 891 (and a clean neighbour). It shows whether the downsample draw wrote it, whether the depth/stencil test rejected it, and the shader output. If the draw writes the bad value, debug that pixel's shader and read the four scene-colour alpha samples; if they're already banded, follow the scene colour's alpha back to the pass that writes it for the right view. If the test rejects it, compare the pass's depth/stencil state and the depth target's contents (Image 870) between the halves.
3. Once A is understood, re-check C (wedge, puddle band) against the same pass.

## Artifact B: notes

- Shadow offset is visible in the flat test with `TestFov` and symmetric projection **off**; `SymmetricProjection=1` is the diagnostic (result of an in-VR comparison not recorded yet).
- Shadow depth maps are 2048² (passes ~50–58 in the capture: `R32_SFLOAT` + `D32`), rendered with viewport 1024×512 at (6,6) for the spot shadows. The shadow projection into the light-attenuation buffer happens in the full-res passes (BGRA8 target, per eye viewport), so the likely culprit is per-view data (projection or view-projection constants) used when projecting the shadow back onto the scene, not the shadow maps.
- To do: capture the shadow spot in the flat test (`TestFov` on) and compare the projective-shadow draws between the eyes (viewports, constants if readable).
