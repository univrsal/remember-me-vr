# 09 — Per-eye rendering artifacts (investigation log)

Status as of 2026-11-26. Stereo, depth, head tracking and resolution work (M3 passed). What's left is passes that misbehave when two views share one view family. This page records what's known, how it was found, and where to resume.

## Artifact list

| # | Artifact | Eyes | Status |
|---|---|---|---|
| A | Light/fog effect cut by horizontal bands (thickness varies with view angle) | right only | **Localised to the half-res depth downsample (below)**, cause not found |
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

Remaining suspect: **the pixel-shader constants of the right-eye draw** (the two draws use different constant buffers, 64 bytes apart; their contents aren't in the capture). `ProjectionScaleBias` and `MinZ_MaxZRatio` come from the view's projection matrix in UE3, so a right-eye matrix with unexpected terms would convert depth wrongly in that eye only. `SourceTexelOffsets*` come from buffer sizes (shared). Not proven.

### Where the code is (Ghidra, `0x00400000` base)

- Shader class constructor `FUN_00a2b7e0` (vtable `0x013a0580`, size 0xC0, binds `ProjectionScaleBias`, `SourceTexelOffsets01/23`, plus the base `FUN_00a28c10`: `SceneColorTexture`, `MinZ_MaxZRatio`, `ScreenPositionScaleBias`, …). It's created by `FUN_00a36d90` (registered from static init code around `0x0120284b`).
- **Not yet found:** the `SetParameters` function that fills the constants, and the caller that draws the quad. The function neighbours of `0xa2bbb0..0xa2c800` scanned so far aren't it.

### Next steps

1. Find the `SetParameters` function (e.g. xref the shader type global from the static-init code, or find who calls `FUN_005d5e30` with the class's parameter fields). Log the four constants per draw for both eyes in the flat test; check whether the right eye's differ.
2. If the right eye's `ProjectionScaleBias`/`MinZ_MaxZRatio` are wrong: find which view/matrix the game reads (e.g. View[0] instead of the eye's own, or terms our `FPerspectiveMatrix` hook perturbs: it rewrites m[0], m[5], m[8], m[9]) and patch it in the hook.
3. Once A is understood, re-check C (wedge, puddle band) against the same pass.

## Artifact B: notes

- Shadow offset is visible in the flat test with `TestFov` and symmetric projection **off**; `SymmetricProjection=1` is the diagnostic (result of an in-VR comparison not recorded yet).
- Shadow depth maps are 2048² (passes ~50–58 in the capture: `R32_SFLOAT` + `D32`), rendered with viewport 1024×512 at (6,6) for the spot shadows. The shadow projection into the light-attenuation buffer happens in the full-res passes (BGRA8 target, per eye viewport), so the likely culprit is per-view data (projection or view-projection constants) used when projecting the shadow back onto the scene, not the shadow maps.
- To do: capture the shadow spot in the flat test (`TestFov` on) and compare the projective-shadow draws between the eyes (viewports, constants if readable).
