# Remember Me VR — evaluation

Evaluation done 2026-11-25. For building and running the mod itself, see the [root README](../README.md). This looks at whether a VR mod for *Remember Me* (DONTNOD, 2013, Steam app 228300) is feasible, and how to build one.

| Doc | Contents |
|---|---|
| [01-game-analysis.md](01-game-analysis.md) | What the install and executable contain, and why it matters |
| [02-graphics-layer.md](02-graphics-layer.md) | D3D9, DXVK, dgVoodoo2, and the "resolution limit" question |
| [03-vr-options.md](03-vr-options.md) | Every route to VR we considered, compared |
| [04-recommended-architecture.md](04-recommended-architecture.md) | The design for a custom mod |
| [05-toolchain.md](05-toolchain.md) | Building and debugging on Linux/Proton vs Windows |
| [06-roadmap.md](06-roadmap.md) | Milestones, go/no-go gate, risks and open questions |
| [07-frame-capture.md](07-frame-capture.md) | RenderDoc analysis of an in-game frame |

## Summary

- **The game is D3D9 only.** The binary has dormant D3D11 code, but the install ships only SM3 shader caches, so D3D11 can't be used.
- **On Linux we already run DX9 through DXVK.** Proton's `d3d9.dll` *is* DXVK, so the PCGamingWiki tip about a Vulkan wrapper is already in effect under Proton. dgVoodoo2 adds nothing here.
- **D3D9's "resolution limit" isn't the real constraint.** DXVK reports 16384×16384 maximum texture size. The real limits are the 32-bit address space (the exe is Large Address Aware, so it gets 4 GB) and the cost of rendering twice in a 2011-era, mostly single-threaded UE3 renderer.
- **No existing mod gives proper VR.** UEVR only supports UE4 and UE5. 3D Vision and HelixMod need an NVIDIA GPU and are discontinued (this machine has an RX 6700 XT). vorpX has a user-made profile but runs on Windows only and is paid.
- **A custom mod is feasible.** The key enabler: DXVK exposes `ID3D9VkInteropDevice`, which gives us the `VkImage` behind any D3D9 texture. Proton also ships a 32-bit OpenXR bridge (`wineopenxr`) that supports `XR_KHR_vulkan_enable`/`enable2`. Together these give a clean path from D3D9 frame → Vulkan → OpenXR, with no D3D9-to-D3D11 shared-surface tricks. Both pieces were verified in the local Proton Experimental install.
- **The hard part is the engine, not the graphics API.** Stereo needs per-eye camera, projection and view-state control inside a heavily modified UE3 build (engine version 8636, licensee 21).
  - The binary has no MSVC RTTI, but SteamStub (Variant 3.1) unpacks cleanly with Steamless.
  - The unpacked exe has UE3's `{name, function}` native tables, confirmed.
  - UE3's runtime reflection (GNames/GObjects, `ProcessEvent`) makes the rest tractable. Borderlands 2 and BioShock Infinite are 32-bit UE3 games that were modded this way.
- **Stereo must be synchronous.** Alternate-frame rendering is explicitly ruled out: it's nauseating, and an AFR-only mod would be useless. The plan is to let the engine **draw the viewport twice per game tick**, once per eye, with per-eye camera, asymmetric projection and `FSceneViewState` (strategy S1). The renderer only ever sees normal single-view frames. Two views in one view family (S2) is the later performance optimisation.
- **The frame capture supports this** (see 07).
  - A frame is only ~860 draws, so doubling the CPU work is cheap.
  - The game already renders its scene at a different size from the backbuffer (its own supersampling), so we can choose the eye resolution freely.
  - GPU cost is the thing to measure.
- **Recommended plan:** build a `dinput8.dll` proxy mod on Linux (mingw-w64), tested under Proton.
  1. Prove the D3D9 → DXVK → OpenXR path with a flat test screen.
  2. Map the engine.
  3. Implement synchronous stereo. This is the **go/no-go gate**: no fallback to AFR.
  4. Then the HUD, menus and comfort features.
- **Switching to Windows isn't required.** It's only worth it for a vorpX comparison or if Windows debugging tools turn out to be needed (see [05-toolchain.md](05-toolchain.md)).
