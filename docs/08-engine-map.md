# 08 — Engine map

What we know about Remember Me's UE3 build (engineVersion 8636, Steam build with PE file-header timestamp `0x69d8a358`).

Sources:
- `re/ghidra/` (Ghidra 12.2 analysis of the Steamless-unpacked exe; `tools/ghidra/query.sh` for terminal queries).
- The mod's runtime discovery (`vrcore::ue3`), with dumps written to `RememberMeVR_objects.txt` and `RememberMeVR_types.txt` in `Binaries/Win32`. Press F9 in game to refresh them.

All code/data addresses are VAs at the preferred image base `0x00400000`. The exe is ASLR-enabled, so at runtime use `base + (VA - 0x400000)` (`ExeInfo::FromPreferredVA`).

## Reflection (runtime-discovered)

| Item | Value |
|---|---|
| `GNames` (TArray<FNameEntry*>) | `0x016232C8`; 50,711 names; ANSI string at entry `+0x8` |
| `GObjects` (TArray<UObject*>) | `0x0160EEBC`; ~60k objects at startup, ~103k in a level |
| `GNatives` (bytecode dispatch table) | `0x015EE1E0` |
| UObject | vtable `+0x00`, `Index +0x20`, `Outer +0x28`, `Name +0x2C` (FName: index, number), `Class +0x34`, `ObjectArchetype +0x38`; size `0x3C` |
| UField | `Next +0x3C` |
| UStruct | `SuperField +0x48`, `Children +0x4C`, `PropertySize +0x50` |
| UProperty | `ArrayDim +0x40`, `ElementSize +0x44`, `Offset +0x60` |
| UObjectProperty / UStructProperty | `PropertyClass +0x80` / `Struct +0x80` |

The layout matches the Borderlands 2 era of UE3 (except `FNameEntry`'s string offset), so community UE3 knowledge mostly applies.

## Live objects in a level

| Object | Class | vtable (VA) |
|---|---|---|
| `Transient.AdriftEngine_0` | AdriftEngine | `0x014888D0` |
| `…ExampleGameViewportClient_0` | ExampleGameViewportClient | `0x014882A0`; `FViewportClient` vtable `0x01488228` at `+0x3C`; `FExec` `0x01356C24` at `+0x40` |
| `…AdriftLocalPlayer_0` | AdriftLocalPlayer | `0x01487880` (101 slots) |
| `…ExamplePlayerController_0` | ExamplePlayerController | `0x0148A138` |
| `…ExamplePlayerCamera_0` | ExamplePlayerCamera | `0x01450770` |
| `…ExamplePawn_1` | ExamplePawn | Nilin |

The engine's `GamePlayers` array is at `GEngine + 0x4EC` (`GEngine` = `*(0x01643338)`).

## Key script-visible properties (from the type dump)

| Class.Property | Offset | Notes |
|---|---|---|
| `Player.Actor` | `+0x40` | the PlayerController |
| `LocalPlayer.ViewportClient` | `+0x68` | |
| `LocalPlayer.Origin` / `Size` | `+0x6C` / `+0x74` | viewport fraction (Vector2D each); split-screen rectangles |
| `LocalPlayer.ViewState` | `+0x8C` | native `FSceneViewState*`; per-view temporal state |
| `LocalPlayer.AspectRatioAxisConstraint` | `+0x3B0` | byte |
| `PlayerController.PlayerCamera` | `+0x40C` | (seen in code) |
| `Camera.DefaultFOV` | `+0x2A4` | |
| `Camera.OffAxisYawAngle` / `OffAxisPitchAngle` | `+0x2B8` / `+0x2BC` | **not stock UE3**: DONTNOD off-axis support |
| `Camera.CameraCache.POV` | `+0x474` | TPOV: `Location +0x0`, `Rotation +0xC`, `FOV +0x18`, `TargetLocation +0x1C`, `fRollOverride +0x28` (DNE) |
| `Camera.ViewTarget` | `+0x4D0` | TViewTarget (POV at `+0x8`) |
| `Camera.ModifierList` | `+0x55C` | TArray<CameraModifier*> |
| `Actor.Location` | `+0x54` | |

## Functions

| Function | VA | How identified |
|---|---|---|
| `UObject::ProcessEvent` | `0x00458BA0` | vtable slot 72 (`+0x120`), called with `FindFunctionChecked(FName)` results |
| `FindFunctionChecked(FName)` | `0x00470DC0` | |
| `UGameViewportClient::Draw(FViewport*, FCanvas*)` | `0x0086D7F0` | `FViewportClient` vtable slot 2 (7,813 bytes) |
| `ULocalPlayer::CalcSceneView` | `0x008622B0` | called from Draw's per-player loop; UE3 early-out on `Actor`/`Size` |
| `FSceneView::FSceneView` | `0x00A17F80` | called from CalcSceneView with `this->ViewState`; views are `0x670` bytes |
| `FPerspectiveMatrix(HalfFOVX, HalfFOVY, MultX, MultY, MinZ, MaxZ)` | `0x008092B0` | called with `MinZ == MaxZ` (infinite far plane); near plane global at `0x0154F6B4` |
| `APlayerController::GetPlayerViewPoint` (probably) | `0x006A49F0` | fills view location/rotation in CalcSceneView; **to verify** |
| `AExamplePlayerCamera::UpdateCameraBehaviour` (C++) | camera vtable `+0x494` | from the `exec` thunk |
| `ULocalPlayer::ULocalPlayer` (constructor) | `0x008698F0` | sets the LocalPlayer vtables; `ViewState = AllocateViewState()` |
| `AllocateViewState()` | `0x00A22B90` | `new FSceneViewState` (`0x380` bytes, ctor `0x00A1FEE0`); no args, returns the pointer |
| `eventGetPlayerViewPoint` thunk | `0x006A49F0` | `__thiscall(controller, FVector*, FRotator*)`, `ret 8`; calls the script event via `ProcessEvent` (confirmed) |
| 2,657 `Class::execFunction` natives | | labelled by `tools/ghidra/LabelUE3Natives.java` |

### `ULocalPlayer::CalcSceneView` (`0x008622B0`)

`__thiscall`, `this` = LocalPlayer. Arguments, as read from the decompile (**verify with a logging hook before relying on them**):

1. `FSceneViewFamily*`: its first member is the `Views` TArray (data, num, max).
2. `FVector*` view location (out).
3. `FRotator*` view rotation (out).
4. `FViewport*` (`GetSizeX`/`GetSizeY` via vtable `+0x8`/`+0xC`).
5. `FViewElementDrawer*`.
6. and 7. optional out-pointers. If both are non-null, the projection and view matrices are copied there and the function returns **without** creating a view.

Returns the new `FSceneView*`. Flow:

1. Return if there's no `Actor` or `Size` ≤ 0.
2. Fire a script event on the controller (`ProcessEvent`).
3. Compute the pixel rect from `Origin`/`Size` × viewport size.
4. Get the view point (`0x006A49F0`), or take it from globals when the debug camera is active (`DAT_01646CCC`).
5. FOV from `PlayerCamera.DefaultFOV`. Build the view matrix (UE3 → D3D axis swap). Half-FOV in radians.
6. Aspect-ratio handling via `AspectRatioAxisConstraint`.
7. Projection = `FPerspectiveMatrix(…)` (`0x008092B0`), infinite far plane.
8. Optional override: if `*(0x01647800)+0x1AC → +0x88` is non-null, its vtable slot 10 rewrites the projection matrix (`(proj, FOV, aspect)`). Possibly a stereo/off-axis extension point; investigate.
9. Post-process settings via a controller virtual (`+0x4DC`).
10. `new FSceneView(Family, this->ViewState, …)` (`0x670` bytes, ctor `0x00A17F80`).
11. **`Family->Views.AddItem(View)`**.

## Implications for stereo

**Two views per family through one hook (strategy S2) is now the most direct route.** Hook `CalcSceneView`; for each eye, call the original with:

- `Origin`/`Size` set to that eye's half of the viewport (or a double-wide backbuffer);
- `ViewState` swapped to that eye's own `FSceneViewState` (`this+0x8C`);
- the eye offset and head rotation applied to the view point (hook `0x006A49F0` while inside the call, or adjust the out-params);
- the OpenXR asymmetric projection substituted for `FPerspectiveMatrix` (hook `0x008092B0` while inside the call).

Both views land in the same family, so the renderer draws both eyes in one pass. View-independent work (shadow depths, cubemap blend) is shared, and there's one present per frame. **Open risk:** DONTNOD's custom passes (`DNE*`: half-res translucency/DOF, volume AO, reflections) may assume one full-screen view. That's quick to test: a flat-screen side-by-side image shows it immediately.

**Fallback (strategy S1):** draw the whole viewport twice per tick by hooking `UGameViewportClient::Draw` (or its caller), with the same per-eye overrides in `CalcSceneView`.

## Open questions

- ~~Argument order of `CalcSceneView`~~: confirmed in game by the logging hook (one view per normal frame; `views 0->2` with the side-by-side test).
- ~~How to create the second `FSceneViewState`~~: `AllocateViewState()` at `0x00A22B90`.
- **A null `FSceneView::State` crashes the renderer.** `FSceneView` has `Family +0x0` and `State +0x4`. DONTNOD's motion-blur setup (`0x00A38B60`, reading `State+0x238`) doesn't null-check it, unlike stock UE3 paths, so every view needs a real view state.
- What the projection-override object at `0x01647800+0x1AC+0x88` is, and when it exists.
- ~~Whether any DNE render pass breaks with two views in a family~~: **the 3D scene renders correctly in both views** (side-by-side test, 2026-11-26: lighting, fog, particles, DOF and post-process all per view, ~170 fps at 1880×2090). Only the UI misbehaves:
  - **2D UI (Scaleform menus/HUD) is laid out in the first view's rectangle.** The pause menu was squeezed into the left half. The UI sizes itself from View[0]'s rect, not from `LocalPlayer.Origin/Size` (which we restore after `CalcSceneView`).
  - **World-space AR hints are UI, not scene geometry.** E.g. "Caution: Leapers territory" is drawn in perspective but only in the left view, so it's projected with View[0]'s camera in the UI pass. That's why the GFx pass in the captures (07) runs with depth testing against scene depth.
- Where the UI pass gets its viewport and projection (next: a RenderDoc capture with side-by-side on, then Ghidra).
