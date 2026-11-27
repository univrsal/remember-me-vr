# 01 — Game analysis

Findings from inspecting the Steam install (`Remember Me/` symlink → `~/.local/share/Steam/steamapps/common/Remember Me`).

## Layout

```
Binaries/Win32/RememberMe.exe   20 MB, the only game binary (no Win64 build)
Binaries/build.properties       engineVersion=8636, changelist 940782, BVT 2011-06-28
Engine/Shaders/Binaries/*.bin   engine shader sources (compressed .usf)
ExampleGame/CookedPCConsoleFinal/  771 cooked packages (.upk), ~6.7 GB
ExampleGame/Movies/             Bink (.bik) FMVs, ~130 files incl. subtitles
RememberMeGame/Config/Default*.ini
Engine/Config/Base*.ini
```

The game project is still called `ExampleGame` internally, and DONTNOD's engine layer is called "Adrift"/"DNE" (`Adrift.upk`, `DNE*` shaders, `DefaultAdriftGfxConfig.ini`).

## Executable

| Property | Value | Implication |
|---|---|---|
| Format | PE32, i386, GUI | 32-bit only. The mod must be a 32-bit DLL. |
| Characteristics | `0x122` (includes LARGE_ADDRESS_AWARE) | 4 GB address space under WoW64/Proton |
| DllCharacteristics | `0x8140` (DYNAMIC_BASE, NX) | ASLR relocates the image. Hooks must use RVAs or pattern scans, not absolute addresses. |
| Sections | `.text .rdata .data .rsrc .reloc .bind` | `.bind` = **SteamStub DRM** |
| Entry point | RVA `0x1450310` (inside `.bind`) | The stub runs first, decrypts `.text`, then jumps to the real entry point. |
| `.text` entropy | 8.0 bits/byte | Code is **encrypted on disk** (SteamStub Variant 3.1). Unpacked with [Steamless](https://github.com/atom0s/Steamless) to `re/RememberMe.exe.unpacked.exe` (local only, gitignored). After unpacking: entropy 6.6, real entry point RVA `0xCF6354`. |
| `.rdata` entropy | 5.2 | Strings are readable without unpacking |
| MSVC RTTI | none found | No class names from RTTI. Rely on UE3 reflection instead. |

The PE file-header timestamp `0x69d8a358` (2026-04-10) identifies the build; the mod checks it at startup. (The debug directory carries a slightly different stamp, `0x69d8a355`; don't confuse the two.) The date suggests a fairly recent rebuild or repackage by the publisher. Signatures and offsets may change if Steam updates the game again, so prefer pattern scanning to hard-coded offsets.

### Imports of interest

`d3d9.dll`, `d3dx9_43.dll`, `DINPUT8.dll`, `XINPUT1_3.dll`, `binkw32.dll`, `steam_api.dll`, PhysX/APEX, Wwise (statically linked).

`DINPUT8.dll` and `XINPUT1_3.dll` are both good candidates for a proxy DLL. `d3d9.dll` is **not**, because we want to keep DXVK's `d3d9.dll` (see 02).

### Strings of interest

- **Stereo**
  - `AllowNvidiaStereo3d` (set to `True` in `BaseEngine.ini`), `NvStereoEnabled`, `NvStereoFixTexture`, `nvapi.dll`: UE3's built-in NVIDIA 3D Vision support. That's driver-side stereo and doesn't help us, but it means the renderer was written with stereo in mind. HUD depth and post-process passes should behave reasonably.
- **D3D11**
  - `AllowD3D11`, `-d3d9, -d3d11 and -opengl are mutually exclusive`, `PC-D3D-SM5`: the D3D11 RHI is compiled in.
  - The install has only `GlobalShaderCache-PC-D3D-SM3.bin` and `RefShaderCache-PC-D3D-SM3.upk`. Without SM5 caches (and without the cooker to make them), the D3D11 path can't render materials. **Treat the game as D3D9 only.**
- **Camera**
  - Native function names like `AExamplePlayerCameraexecUpdateCameraBehaviour`, `...ApplyCameraModifiers`, `...BlendViewTargets`, `AGamePlayerCameraexecAdjustFOVForViewport`, `AActorexecGetALocalPlayerController`.
  - These are UE3's native function registration names. **Confirmed** in the unpacked exe: `.data` holds `{const char* name, void* fn}` pairs.
    - `AExamplePlayerCameraexecUpdateCameraBehaviour` → `0x00C8E0D0`
    - `AGamePlayerCameraexecAdjustFOVForViewport` → `0x00BBBE60`
    - Both VAs are at the preferred base `0x400000`.

## Cooked packages

All checked packages have file version **832**, licensee **21**. UE Explorer and similar UPK tools handle this version, so the UnrealScript (camera behaviours, HUD, player controller) can be decompiled and read. That's how we'll learn the camera stack without disassembling.

Main script packages: `Core`, `Engine`, `GameFramework`, `GFxUI`, `IpDrv`, `Adrift`, `ExampleGame`, `Startup`.

## Config

- `[Engine.Console] ConsoleKey=Tilde`, and `ConsoleClassName=ExampleGame.ExampleConsole`. We still need to test whether the shipping build honours the console. Shipping UE3 builds often disable it.
- `BaseEngine.ini` `FOVAngle=90`, `AspectRatioAxisConstraint=AspectRatio_MaintainXFOV`.
- System settings (`MotionBlur`, `DepthOfField`, `bAllowTemporalAA`, `ScreenPercentage`, `UseVsync`) can be turned off in INI files. With synchronous stereo, temporal effects can stay on as long as each eye has its own view state (see 04).
- User config (created on first launch): `pfx/drive_c/users/steamuser/Documents/My Games/UnrealEngine3/RememberMeGame/Config/`.
  - `ExampleAdriftGfxConfig.ini` holds the menu settings: window and fullscreen resolution, `SuperSampleCount`, `AntiAliasing`, VSync.
  - `ExampleEngine.ini` holds `[SystemSettings]`.

## Engine generation

Engine version 8636 is a 2011 UE3 fork. Epic's official Rift/stereo support in UDK came in 2013, so **this engine has no built-in HMD or stereo-view code**. Any stereo rendering has to be added by us.

## Host environment

- GPU: AMD RX 6700 XT (RADV). That rules out any NVIDIA-only route (3D Vision, HelixMod, Katanga).
- Proton Experimental (11.0) is configured for the game. Its `d3d9.dll` is DXVK and exports the `ID3D9VkInteropDevice` interface.
- Proton ships `wineopenxr` (i386 and x86_64) with `XR_KHR_vulkan_enable`, `vulkan_enable2`, `D3D11_enable` and `D3D12_enable`. It also ships `vrclient` and `openvr_api_dxvk.dll` for OpenVR.
- The active OpenXR runtime is SteamVR.
