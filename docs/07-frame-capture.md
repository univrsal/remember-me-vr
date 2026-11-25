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

No Scaleform/GFx HUD draws are visible in this frame. Either the HUD was hidden at capture time, or it isn't drawn into the backbuffer in this scene. **We need a second capture with the HUD and a menu visible.**

## What this means for stereo

1. **CPU cost is small.** 857 draws per frame; doubling that is no problem for the render thread.
2. **GPU cost:** the game already runs at 3.7 MP (2560×1440) with supersampling on. Two eyes at ~2000×2200 is ~8.8 MP, about 2.4× the current pixel work, for a 2013 forward renderer on an RX 6700 XT. Mono fps should be measured with `SuperSampleCount=1` to set a baseline.
3. **Work that could be shared between eyes:** the cubemap blend (passes 0–42) and the 2048² shadow depths (if they're spot/per-light shadows, not view-fitted cascades). If both eyes are rendered as two views in one view family, UE3 does each of these once. If we render the whole viewport twice, they're done twice.
4. **Per-view temporal state:** eye adaptation (pass 72), and probably occlusion queries (`vkCmdBeginQuery` ×135 in the frame). Each eye needs its own `FSceneViewState`, or eye adaptation and occlusion results will leak between eyes.
5. **Downsample point:** pass 87 → 92 is where the full-res, post-processed image becomes the backbuffer. With `SuperSampleCount=1` and the window set to the eye resolution, the backbuffer *is* the eye image. That's the simplest point to capture it.

## Next captures to take

- [ ] Gameplay with HUD visible (combat, so health/combo UI is on screen)
- [ ] Pause menu
- [ ] An in-engine cutscene
- [ ] Any scene with `SuperSampleCount=1`, for a baseline pass list
