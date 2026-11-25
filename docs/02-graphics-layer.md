# 02 — Graphics layer: D3D9, DXVK, dgVoodoo2

## The game renders with D3D9

See 01: the D3D11 RHI is compiled in, but there are no SM5 shader caches, so D3D9 is the only working renderer.

## DXVK vs dgVoodoo2

| | DXVK (d3d9) | dgVoodoo2 |
|---|---|---|
| Translates to | Vulkan | D3D11 / D3D12 |
| On Linux/Proton | **Already in use**: Proton's `d3d9.dll` is DXVK | Would be D3D9→D3D11→(DXVK)→Vulkan, a pointless double translation |
| On Windows | Drop-in `d3d9.dll`, works on AMD/NVIDIA/Intel | Drop-in, Windows-focused |
| Threading | Records D3D calls and runs them on a separate worker thread, so the game's thread does less work | Does much of its work on the calling (game) thread |
| Interop for VR | `ID3D9VkInteropDevice` / `ID3D9VkInteropTexture` give the `VkImage` and `VkDevice` behind D3D9 objects, plus a lock on the submission queue | Can expose its internal `ID3D11Device`, which lets D3D11 tools (3Dmigoto/Geo-11, D3D11 VR submit) run on top |
| Open source | Yes (zlib) | No |

The note you were given is right: DXVK is the better choice. On Linux it isn't even a decision, because it's what Proton already does.

dgVoodoo2 would only make sense on Windows, and only to put a D3D11-only tool on top of the game (e.g. Geo-11, see 03). That isn't our recommended route.

### Why DXVK matters for VR specifically

A VR runtime needs the frame as a texture in an API it accepts. OpenXR and OpenVR accept D3D11, D3D12, Vulkan and OpenGL, but **not D3D9**. The usual D3D9 workarounds are:

- D3D9Ex shared handles → open in D3D11 → submit. This needs the game's device upgraded to D3D9Ex, and shared resources work poorly under Wine.
- A CPU copy, which is too slow.

With DXVK, every D3D9 texture is already a `VkImage`. `ID3D9VkInteropDevice` hands us the `VkInstance`, `VkPhysicalDevice`, `VkDevice`, queue and a lock on the submission queue. We can then copy eye images into an OpenXR Vulkan swapchain with a plain `vkCmdCopyImage` on the same device. No sharing or extra copies across APIs.

DXVK also has built-in hooks that ask OpenVR and OpenXR which Vulkan extensions they need, and enables those when it creates its device. That's why Proton ships `openvr_api_dxvk.dll`. So the `VkDevice` DXVK creates should already satisfy the VR runtime. **This needs to be verified in milestone 1** (see 06).

## "D3D9 resolution limitations"

D3D9 itself is not what limits resolution here:

- **Max texture or render-target size** comes from the device caps. DXVK reports 16384×16384. Even a side-by-side double-wide target at Index/Quest 3 resolutions (e.g. 2 × 2016 × 2240 ≈ 4032 × 2240) fits easily.
- **Address space is the real limit.** The process is 32-bit LAA, which gives it 4 GB. UE3 allocates scene colour (FP16), depth, light-attenuation, bloom/DOF, TAA history and so on at the maximum viewport size. Doubling the viewport area roughly doubles that render-target memory. DXVK helps because it keeps texture data in GPU memory and can unmap staging memory (`d3d9.textureMemory`; check the current `dxvk.conf` reference for the exact option names), so the 32-bit process doesn't have to map everything.
- **CPU cost of rendering twice.** UE3-2011 has one game thread and one render thread. Synchronous stereo (required, see 03) roughly doubles the render thread's work for the scene. The capture shows ~860 draws per frame, so the CPU side is cheap; GPU pixel cost is what to measure (see 07). The PCGamingWiki entry already lists stuttering as a known issue, so we should measure a mono baseline first.
- **VRAM reporting.** Some 32-bit games misbehave if told they have more than 4 GB of VRAM. DXVK caps the reported amount for 32-bit processes (`d3d9.maxAvailableMemory`). Leave that default unless UE3 texture streaming starts thrashing.

## Useful DXVK settings (`dxvk.conf` next to the exe, or via `DXVK_CONFIG`)

```ini
# Present / pacing — the VR runtime should pace, not the game
d3d9.presentInterval = 0          # disable game vsync (or set UseVsync=False in ini)
dxvk.numBackBuffers = 2

# Debug while developing
# DXVK_HUD=fps,frametimes,memory   (env var)
# DXVK_LOG_LEVEL=debug             (env var)
```
