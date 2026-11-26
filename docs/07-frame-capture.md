# 07 — Frame capture analysis

Source: `ingame_capture.rdc` (RenderDoc 1.46, Vulkan via DXVK under Proton). The capture is local only and gitignored.

Method: `renderdoccmd convert -c xml`, then a script that lists each `vkCmdBeginRendering` with its attachments, formats, load ops and draw count. Debug labels in the capture come from DXVK (`[vs.hash] [fs.hash]` per draw), **not** from the game: UE3's `SCOPED_DRAW_EVENT` names are compiled out of this shipping build. Passes have to be identified by their attachments and shaders.

## Settings at capture time

From `Documents/My Games/UnrealEngine3/RememberMeGame/Config/ExampleAdriftGfxConfig.ini`:

```
ResWinWidth=1280  ResWinHeight=720   Fullscreen=false
SuperSampleCount=2   AntiAliasing=2   VSync=false
```

So the swapchain is **1280×720**, while the scene renders at **2560×1440**: the game's own 2×2 supersampling, downsampled at the end.

**Takeaway:** the engine already decouples scene render-target size from the backbuffer. For VR we can choose the eye resolution freely.

## Frame structure (94 render passes, 857 draws)

| Passes | Size | Target(s) | Interpretation |
|---|---|---|---|
| 0–6 | 256² → 4² | FP16, mip chain | Downsampled mip chain, probably DNE reflection/cubemap filtering |
| 7–42 | 128² → 4², ×6 | BGRA8 mip chains | 6 faces × 6 mips: a cubemap blend (`DNEBlendCubemap`). **View-independent.** |
| 43 | 2560×1440 | FP16 + D32S8 (clear) | First scene pass (146 draws), depth established here |
| 44 | 2560×1440 | BGRA8 + depth | 24 draws, 1 shader: likely a velocity/mask buffer |
| 45–46 | 2560×1440 | FP16 scene colour #55079 + depth | Base pass (175 draws, ~40 shaders) |
| 47, 59, 62… | 2560×1440 | FP16 #55078 | Single full-screen draws: copies or resolves of scene colour |
| 49–58 | 2048² | R32F + D32 | **Shadow depth maps** (3 shadow passes) |
| 51, 53, 57 | 2560×1440 | BGRA8 #55080 | Shadow projection into a light-attenuation buffer |
| 54, 58, 65 | 2560×1440 | FP16 scene colour | Additive dynamic-light passes (UE3 forward multi-pass lighting) |
| 60, 69–71 | 1280×720 | FP16/BGRA8 + half-res depth | Half-res passes (DNE translucency/DOF, downsampled depth) |
| 61 | 2560×1440 | scene colour | 135 draws, 2 shaders: translucency/particles or decals |
| 72 | 256×64 | BGRA8 | Luminance/eye adaptation |
| 73–86 | 642×362 | RGBA16 UNORM | Quarter-res bloom/DOF chains (640+2 border) |
| 87 | 2560×1440 | BGRA8 #55172 | Uber post-process output (LDR) |
| 92 | 1280×720 | BGRA8 #55062 | Supersample downsample into the D3D9 backbuffer |
| 93 | 1280×720 | swapchain | DXVK's present blit |

No UI draws are visible in this frame; the HUD was hidden at capture time. The UI captures below cover it.

## What this means for stereo

1. **CPU cost is small.** 857 draws per frame; doubling that is no problem for the render thread.
2. **GPU cost.** Measured mono baseline (2026-11-26): **~180 fps at 1880×2090** (3.9 MP), `SuperSampleCount=1`, windowed, RX 6700 XT, one gameplay scene. That's ≈5.6 ms per eye-sized frame. Rough estimate for S1 (whole frame drawn twice, scaling linearly with frame cost):

   | Per-eye resolution | Two-eye frame time | ≈ fps |
   |---|---|---|
   | 1880×2090 (measured size) | ~11.1 ms | ~90 |
   | 2016×2240 | ~12.8 ms | ~78 |

   That baseline was at **maximum settings** on a mid-range CPU/GPU under Proton, so lowering settings (shadow and effect quality, DNE effects) should give real headroom. Re-measure at lower settings.

   So 72/80 Hz looks comfortably reachable. 90 Hz is borderline at full resolution and will need a slightly lower eye resolution, cheaper DNE effects, or S2, which shares shadows and the cubemap between eyes.
   - This is one scene. Heavier areas (Neo-Paris exteriors, combat with many effects) need checking.
   - It isn't known whether the 180 fps run was GPU-bound. Check with `DXVK_HUD=gpuload`.
3. **Work that could be shared between eyes:** the cubemap blend (passes 0–42) and the 2048² shadow depths (if they're spot/per-light shadows, not view-fitted cascades). If both eyes are rendered as two views in one view family, UE3 does each of these once. If we render the whole viewport twice, they're done twice.
4. **Per-view temporal state:** eye adaptation (pass 72), and probably occlusion queries (`vkCmdBeginQuery` ×135 in the frame). Each eye needs its own `FSceneViewState`, or eye adaptation and occlusion results will leak between eyes.
5. **Downsample point:** pass 87 → 92 is where the full-res, post-processed image becomes the backbuffer. With `SuperSampleCount=1` and the window set to the eye resolution, the backbuffer *is* the eye image. That's the simplest point to capture it.

## UI, menus and cutscenes (second capture set, 2026-11-26)

Captures (all at 1880×2089 window, `SuperSampleCount=1`, maximum settings):

| Capture | Passes | Draws |
|---|---|---|
| `main_menu` | 49 | 369 |
| `pause_menu` | 91 | 776 |
| `hud_combat` | 101 | 925 |
| `comobo_lab_ui` (Combo Lab) | 79 | 401 |
| `in_world_item_pickut` | 88 | 268 |
| `voice_comm_overlay_ingame` | 100 | 992 |
| `cutscene` (in-engine) | 87 | 944 |

The scene renders at 1880×2096 (height rounded up to a multiple of 16). The D3D9 backbuffer is 1880×2089.

**Every frame ends the same way:**

```
… post-process → LDR scene image (1880×2096)
[A] 1 draw   vs.83471f9 / fs.1de2ff3   copy scene → backbuffer          ← always present
[B] N draws  4 shader pairs, scene depth-stencil bound                   ← Scaleform GFx UI
             vs.74cd077/fs.4020d7e  vs.22a5c61/fs.8446e7b
             vs.22a5c61/fs.127d52f  vs.e16bfa9/fs.099af47
[C] N draws  vs.83471f9 / fs.c63bd4a  no depth                           ← 2D canvas quads (some screens)
    DXVK present blit
```

(Hashes are DXVK's shader labels. Our D3D9 hook will identify the same shaders by hashing the bytecode passed to `CreateVertexShader`/`CreatePixelShader`.)

- **[A] is a clean split point.** Everything rendered before the copy is the 3D scene, with no UI. Everything after it is UI. The mod can take the eye image from the scene target (or the backbuffer right after [A]), then redirect [B] and [C] into a transparent HUD render target for an OpenXR quad layer.
- **[B] (GFx) runs with depth test on** (`LESS_OR_EQUAL`, no depth writes, stencil off) against the scene depth buffer. This is probably just leftover state, with GFx drawing at a constant depth that always passes, but it needs confirming. If any UI elements are actually placed in the world (e.g. markers that geometry hides), they'd need different handling. When redirecting, bind no depth buffer or a cleared one.
- **[C]** uses the same vertex shader as the copy (a screen-space quad), with a different pixel shader. It appears in combat, the Combo Lab and the voice-comm overlay: probably UE3 `Canvas` tiles (e.g. the comm portrait) drawn outside Scaleform. It goes to the same HUD layer.
- **Main menu** renders a full 3D scene behind the UI (49 passes), so it can be shown in stereo with the menu on a quad.
- **Cutscenes are in-engine** (944 draws, full scene pipeline) with no UI pass at all, so they can be rendered in stereo like gameplay. Only the Bink `.bik` movies need cinema mode.
- Draw counts stay under ~1000 in every capture, so the CPU cost of drawing twice remains small.

## Depth convention (preliminary)

Over a whole frame, `LESS_OR_EQUAL` dominates the depth compare ops (19× vs 1× `GREATER_OR_EQUAL`, 22× `ALWAYS`). Depth clears use both 0 and 1 (different targets: scene vs. shadow maps vs. masks). This points to **standard (non-reversed) depth** for the scene. Confirm in the RenderDoc UI by inspecting the base-pass projection constants before writing the projection override.

## Remaining capture tasks

- [x] Gameplay with HUD visible
- [x] Pause menu, main menu, Combo Lab, item pickup, voice-comm overlay
- [x] In-engine cutscene
- [x] Baseline pass list with `SuperSampleCount=1`
- [ ] A Bink FMV playing, to fingerprint `BinkShaders`
- [ ] A Memory Remix sequence: special rewind/scrub UI and camera
