# 03 — VR options compared

Ratings are relative to *this* game on *this* machine (Linux, AMD GPU).

## Requirement: synchronous stereo

**Alternate-frame rendering (AFR) is out of scope.** In AFR each eye updates on alternating frames, so every moving object, camera move and animation gives the two eyes images from different moments. This almost always feels bad, and is a common cause of nausea in VR mods. A mod that can only do AFR is considered useless for this project.

Both eyes must be rendered **from the same game state, in the same frame, and submitted together**. If milestone 3 (see 06) can't achieve that, the project stops there rather than falling back to AFR.

## Off-the-shelf tools

| Option | What you get | Linux/Proton | Verdict |
|---|---|---|---|
| **UEVR** (praydog) | Full 6DoF VR for UE4/UE5 | Works under Proton | **Not applicable.** UE4/UE5 only. UE3 internals are completely different. |
| **vorpX** | Geometry 3D (G3D, synchronous) or Z-buffer 3D; head tracking via mouse emulation | Windows only | PCGamingWiki lists a *user-made* G3D profile. Paid. Only useful as a **Windows reference** for how the game behaves in geometric stereo. |
| **NVIDIA 3D Vision + HelixMod**, viewed in VR via Katanga/HelixVision | Geometric stereo in a virtual theatre | No; needs NVIDIA plus an old driver | **Not possible on AMD.** Discontinued in 2019. Community reports say the game worked well in 3D Vision, a good sign that its shaders tolerate stereo. |
| **dgVoodoo2 → Geo-11 (3Dmigoto)** | Shader-patched stereo; SBS/TaB output to a theatre app | Windows only | High effort per shader, and it's "3D cinema", not VR. |
| **ReShade depth-3D / vkBasalt** | Fake stereo from the depth buffer | Yes | Not real stereo. Rejected. |
| **Virtual desktop / theatre** | Flat game on a big screen | Yes | Not VR. Only useful as a test of our submission path (milestone 1). |

No existing tool gives head-tracked, synchronous, properly scaled stereo for this game on Linux, so the answer is **a custom mod**.

## Custom mod: synchronous stereo strategies

### S1. Render the viewport twice per engine tick (recommended first)

The game thread runs one tick (simulation, animation, camera update). We then let the engine **draw the game viewport twice**, once per eye, before advancing to the next tick. Between the two draws we:

- set the camera POV to that eye (head pose + IPD offset);
- set that eye's asymmetric projection;
- swap in that eye's `FSceneViewState`, so occlusion history, eye adaptation and any temporal effects stay per eye.

Each draw is an ordinary mono frame from the renderer's point of view. DONTNOD's custom passes, the supersampling path and post-processing all run unmodified. We capture the backbuffer at the end of each eye's draw on the render thread, and submit both eyes to OpenXR together with the poses used.

- **Pros:**
  - Truly synchronous: both eyes come from the same simulation state.
  - Smallest reverse-engineering surface: the top-level viewport draw, the camera POV, the projection and the view-state pointer.
  - Robust against engine customisations, because the renderer only ever sees single-view frames.
- **Cons:**
  - View-independent work is done twice: cubemap blend, shadow depths, the render-thread scene walk. See 07 for why that should be affordable.
  - Anything with side effects in the draw path runs twice: HUD `PostRender` script, scene captures, frame counters. Each needs checking; the HUD should be drawn on one eye only, or into its own layer.
  - The engine's frame-time and smoothing logic sees two presents per tick. Suppress the first present.

### S2. Two views in one view family (optimisation, later)

UE3 renders split-screen as **one `FSceneViewFamily` with several `FSceneView`s**, each with its own viewport rectangle and view state. Our hook in `ULocalPlayer::CalcSceneView` (or `UGameViewportClient::Draw`) would add a second view for the other eye, in a double-wide render target. This is what Epic's 2013 UDK Rift support did.

- **Pros:**
  - View-independent work (shadow depths, cubemap, scene setup) is done once, so GPU and CPU cost are lower.
  - A single present per tick.
- **Cons:**
  - Needs exact layouts of `FSceneViewFamily`, `FSceneView` and the views array.
  - DONTNOD's custom passes (`DNE*`: half-res translucency/DOF, reflections, volume AO, cubemap blend) may assume a single full-screen view. Every one has to be checked for viewport-rect correctness.
  - The supersampling buffers must be sized for the double-wide family.

**Plan:** get S1 working and measure it. Move to S2 only if S1's frame rate is short of the HMD refresh rate, which depends on the mono baseline. Both are synchronous; most of the work for S1 (camera, projection, view state, frame capture) carries over to S2.

### Rejected

- **AFR:** see the requirement above.
- **SceneCapture2D eyes (UnrealScript only):** captures skip much of the post chain, cost a lot, and still need native code to reach the HMD.
- **Shader-level stereo (3Dmigoto-style draw duplication in our D3D9 hook):** a long tail of per-shader fixes (shadow projection, deferred light passes, half-res translucency, post), and it still needs camera hooks for head tracking. It's only worth reconsidering if engine-level S1 turns out to be impossible.

## Sources

- PCGamingWiki: https://www.pcgamingwiki.com/wiki/Remember_Me (VR/vorpX entry, stuttering issue)
- UEVR overview: https://www.uploadvr.com/praydogs-uevr-mod/
- D3D9 → VR via a translation layer discussion: https://www.vogons.org/viewtopic.php?p=786711
- UE3 and 3D Vision background: https://forums.unrealengine.com/t/ue3-3d-vision-issues/20047
- DX9 HelixMod games in VR (Katanga/HelixVision): https://oneup03.github.io/VRto3D/wiki/HelixVision,-Katanga,-and-DX9
