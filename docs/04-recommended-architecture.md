# 04 — Recommended architecture

```
RememberMe.exe (32-bit, UE3-8636)
 │  imports DINPUT8.dll ──────────────► our dinput8.dll (proxy + mod)
 │                                       ├─ forwards DirectInput8Create to system dinput8
 │                                       ├─ deferred init once SteamStub has decrypted .text
 │                                       ├─ engine hooks (UE3)
 │                                       │    viewport draw ×2 per tick, camera POV,
 │                                       │    per-eye projection, per-eye FSceneViewState
 │                                       ├─ D3D9 hooks (device vtable)
 │                                       │    Present (eye capture / suppress), HUD redirect
 │                                       └─ VR backend (OpenXR, Vulkan graphics binding)
 │                                             ▲ VkImage of eye textures
 │  imports d3d9.dll ─────────────────► DXVK d3d9 (Proton builtin, or shipped on Windows)
 │                                       └─ ID3D9VkInteropDevice / ID3D9VkInteropTexture
 ▼
Vulkan ──► wineopenxr (Proton) ──► SteamVR / Monado / WiVRn OpenXR runtime
```

Stereo is **synchronous**: both eyes come from the same tick and are submitted together. There is no AFR mode (see 03).

## 1. Injection

- **Proxy `dinput8.dll`** in `Binaries/Win32/`. The exe imports it directly. On Proton, use the launch option `WINEDLLOVERRIDES="dinput8=n,b" %command%`.
- **Don't proxy `d3d9.dll`**, because that would replace DXVK. Hook the device vtable after `IDirect3D9::CreateDevice` instead.
- **SteamStub** (Variant 3.1, confirmed with Steamless): `.text` is encrypted while our `DllMain` runs. Do all game-code hooking from the `CreateDevice` hook, which runs after the stub has decrypted the code and jumped to the real entry point.
- **ASLR:** compute addresses as `module base + RVA`. The RVAs come from the unpacked exe analysed in Ghidra; back them up with byte-pattern checks so a game update fails loudly instead of crashing.
- Hooking library: **MinHook** (x86).

## 2. Engine layer (UE3)

### Finding functions

- **Native function tables.** `exec*` names are stored as `{const char* name, void* fn}` pairs in `.data`. Confirmed: `AExamplePlayerCameraexecUpdateCameraBehaviour` → `0xC8E0D0`, `AGamePlayerCameraexecAdjustFOVForViewport` → `0xBBBE60` (VAs at the preferred base). That gives us every UnrealScript-callable native for free.
- **Reflection.** `GNames`/`GObjects` let us find live objects (`LocalPlayer`, `GameViewportClient`, `PlayerController`, `Camera`) and their **vtables**. Pure C++ virtuals (`ULocalPlayer::CalcSceneView`, `UGameViewportClient::Draw`, `FViewport::Draw`) are found by vtable index, confirmed in Ghidra, rather than by signature.
- **`ProcessEvent` hook** for observing and overriding UnrealScript events (camera behaviours, HUD).
- Script source from the `.upk` files (UE Explorer) to understand the camera stack.

### Stereo loop (strategy S1)

```
UGameEngine::Tick
  ├─ world tick, camera update                 (once — unchanged)
  └─ viewport draw  ── hooked ──►  for eye in (L, R):
                                     set eye POV      = game camera ⊕ head pose ⊕ ±IPD/2
                                     set eye proj     = OpenXR XrFovf (asymmetric)
                                     swap ViewState   = eyeState[eye]
                                     original draw (eye == R ? present : suppress)
                                   restore camera/view state for the game thread
```

- **Where to hook:**
  - The right level is the call that wraps `BeginDrawingViewport → Client->Draw → EndDrawingViewport`. UE3's `FViewport::Draw` takes a `bShouldPresent` flag in many builds; if this one does too, that solves present suppression directly.
  - Otherwise, count presents in the D3D9 `Present` hook. Presents run on the render thread in order: the first one captures the left eye and is swallowed, the second captures the right eye, submits both, then presents the desktop mirror.
- **Camera:**
  - Apply head pose after the game's camera has updated (after `UpdateCameraBehaviour`/`ApplyCameraModifiers`, or by overriding the POV that `CalcSceneView` reads).
  - Head rotation affects only the *view*, never controller aim or movement.
  - Disable roll, shakes and other discomfort-causing modifiers (configurable).
  - World scale: calibrate on Nilin's height; expose as a setting.
- **Projection (required from day one):**
  - Correct stereo needs the OpenXR per-eye asymmetric frustum. A symmetric approximation causes eye strain, so it's not an option even temporarily.
  - Override the projection matrix where the view is built (`ULocalPlayer::CalcSceneView` / `FSceneView` construction). Match this build's depth convention (check in RenderDoc whether it's reversed-Z or infinite far plane).
- **Per-eye view state:**
  - Allocate a second `FSceneViewState`, using the engine's own allocator: the same call `ULocalPlayer` uses at init.
  - Swap the `ULocalPlayer::ViewState` pointer per eye, so occlusion queries, eye adaptation and any temporal history stay per eye.
- **Side effects of drawing twice:**
  - The HUD/Canvas `PostRender` script runs per draw. Run it on one eye only, into the HUD layer (see §3).
  - Check frame counters, scene captures and any game logic triggered in the draw path. Matinee and cutscene logic tick in the world tick, not the draw.
- **Resolution:** set the window to the per-eye resolution (or to the eye aspect plus `SuperSampleCount`). The scene buffers are already decoupled from the backbuffer (see 07).

### Strategy S2 (later optimisation)

Replace "draw twice" with "two views in one family" inside the same hook points: `CalcSceneView` called once per eye with left/right viewport rects in a double-wide target. Only start this after S1 works and is measured (see 03).

## 3. D3D9 layer

Hooks on the `IDirect3DDevice9` vtable:

- **`Present`:** eye capture and suppression (§2).
- **HUD/menus (Scaleform GFx + Canvas)**, per the UI captures in 07:
  - Every frame ends with one fixed "copy scene → backbuffer" draw, then the UI draws.
  - Hash shader bytecode at `CreateVertexShader`/`CreatePixelShader`, and recognise the copy draw by its shader pair.
  - Take the eye image at that point.
  - Redirect all later draws in the frame to a transparent RGBA HUD render target, with no depth buffer (or a cleared one). That becomes an **OpenXR quad layer** at a comfortable distance.
  - Render the UI once per tick, not per eye: skip the UI draws during the first eye's draw.
- **Bink FMVs:** detect the `BinkShaders` pass and show the frame on a quad layer (cinema mode). Pre-rendered video can't be stereo.

## 4. VR backend

**OpenXR** with `XR_KHR_vulkan_enable2`, or `vulkan_enable` as a fallback:

- Get `VkInstance`, `VkPhysicalDevice`, `VkDevice` and the queue from `ID3D9VkInteropDevice` (`GetVulkanHandles`, `GetSubmissionQueue`).
- Wrap every `vkQueueSubmit` we issue in `LockSubmissionQueue` / `ReleaseSubmissionQueue`.
- Read eye textures via `ID3D9VkInteropTexture::GetVulkanImageInfo` plus `TransitionTextureLayout`, and copy into the OpenXR swapchain images.
- Frame loop:
  - `xrWaitFrame` gates the *game tick* (via the engine's frame-rate limiter, or by blocking before the tick);
  - `xrLocateViews` → poses for this tick;
  - after the right eye, `xrEndFrame` with a projection layer containing **both eyes with the poses that were used**, plus the quad layers.
- **Fallback: OpenVR** (`IVRCompositor::Submit` with `VRVulkanTextureData_t`), via Proton's mature `vrclient`. Keep the backend behind a small interface.
- **Open risk:** whether DXVK's `VkDevice` has the extensions the VR runtime needs. DXVK asks OpenVR/OpenXR for them at device creation; milestone 1 tests this first.

## 5. Input

- The gamepad works unchanged (XInput).
- Optional: map motion-controller buttons/sticks to XInput by hooking `XInputGetState`. Motion-controller combat is out of scope.

## 6. Config and UX

- `RememberMeVR.ini`: world scale, IPD override, HUD distance/size, camera mode (third-person / head-locked offset), comfort toggles, log level.
- Hotkeys: recentre; VR/flat toggle for debugging.
