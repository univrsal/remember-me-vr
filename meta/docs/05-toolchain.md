# 05 — Toolchain: Linux/Proton vs Windows

**Short answer:** we can develop, build and test on Linux. Windows is optional.

## Building on Linux

The mod is a 32-bit Windows DLL. Options:

| Option | Notes |
|---|---|
| **mingw-w64 (i686) + CMake or Meson**, recommended | Installed (`i686-w64-mingw32-gcc`). DXVK itself is built this way. MinHook and the OpenXR/Vulkan headers build fine. Produces a DLL that depends only on the system DLLs plus libgcc/libstdc++, which should be linked statically (`-static -static-libgcc -static-libstdc++`). |
| clang-cl + lld-link + `xwin` (MSVC CRT/SDK headers) | Produces a "real" MSVC-ABI DLL, which matters only if we need to interoperate with MSVC C++ objects in the game (we mostly use raw offsets and COM vtables, so it doesn't). `clang`/`lld-link` exist under `/opt/rocm/llvm/bin`, but a regular `clang`/`lld` install would be cleaner. |

Headers and libs to vendor (git submodules or FetchContent):

- MinHook
- OpenXR SDK headers + Windows `openxr_loader.dll` (or load `wineopenxr` via the loader; Proton registers it as the active runtime inside the prefix)
- Vulkan headers
- DXVK's `d3d9_interop.h` (`ID3D9VkInteropDevice`, `ID3D9VkInteropTexture`)
- optionally OpenVR headers for the fallback

## Running and testing under Proton

- Game prefix: `~/.local/share/Steam/steamapps/compatdata/228300/pfx`
- Steam launch options during development:
  ```
  WINEDLLOVERRIDES="dinput8=n,b" DXVK_LOG_LEVEL=info PROTON_LOG=1 %command%
  ```
  `PROTON_LOG=1` writes `~/steam-228300.log`. Our DLL should write its own log file next to the exe as well.
- Use a deploy script that copies the built DLL into `Remember Me/Binaries/Win32/` (via the symlink). Gitignore the game folder; it already is.
- SteamVR must be running before the game starts so the OpenXR/OpenVR bridges find it.
- One-time setup for 32-bit OpenXR on Linux: `meta/tools/setup-openxr-i686.sh` (see 06, M1 findings).
- Testing the VR path without the game: `meta/tools/smoke-test.sh --proton --frames 900` (needs SteamVR running).

## Setting the resolution

**Launch options don't work for this.** The exe still contains UE3's standard command-line parsing (`-ResX=`, `-ResY=`, `-windowed`). But after startup, DONTNOD's port reads `ExampleAdriftGfxConfig.ini` and issues its own `SETRES <w>x<h>` console command from it. That overrides whatever the command line set. (Format strings `SETRES %dx%d` / `SETRES %dx%d@%d` sit next to the config keys in the exe.)

Edit the config instead:
`pfx/drive_c/users/steamuser/Documents/My Games/UnrealEngine3/RememberMeGame/Config/ExampleAdriftGfxConfig.ini`

```ini
[Rendering]
ResWinWidth=2016      ; used when Fullscreen=false
ResWinHeight=2240
ResWidth=1920         ; used when Fullscreen=true
ResHeight=1080
SuperSampleCount=1    ; 2 = render at 2x width and 2x height, then downsample
Fullscreen=false
VSync=false
```

Notes:

- **The physical screen limits the window size.** A requested 2016×2240 window came out at **1880×2090**, clamped to the monitor. To reach a larger render size, use a smaller window with `SuperSampleCount=2` (e.g. 1008×1120 → 2016×2240 scene). With the mod, the window size no longer matters in stereo mode: `[VR] RenderSize` sets the render size (see [configuration.md](configuration.md#resolution-and-window-size)).
- **The in-game graphics menu may rewrite this file.** Check it after changing settings in game.
- **Untested: `GFXINI=` / `DEFGFXINI=`.** The exe also recognises these switches. They probably select an alternative graphics config, which would allow per-profile launch options (e.g. `%command% -GFXINI=VRBaseline.ini`). The path semantics (absolute, or relative to the config folder) are unknown.
- `DXVK_HUD=fps,frametimes,gpuload` in launch options shows the actual backbuffer size and whether a run is GPU-bound.

## Reverse engineering and debugging on Linux

| Task | Tool | Linux? |
|---|---|---|
| Unpack SteamStub for static analysis | Steamless v3.1.0.5 in `meta/tools/steamless/` (gitignored) | **Done.** Runs under mono: `MONO_PATH=meta/tools/steamless/Plugins mono meta/tools/steamless/Steamless.CLI.exe re/RememberMe.exe`. Always work on a copy, never the installed exe. **The unpacked exe stays local; never commit or distribute it.** |
| Disassembly/decompile | Ghidra 12.2 (installed), r2 | Yes. Project in `re/ghidra/` (gitignored). Headless import + analysis + native labelling (needs a bigger heap than the 2 GB default, which runs out on this exe):<br>`GHIDRA_HEADLESS_MAXMEM=12G /opt/ghidra/support/analyzeHeadless re/ghidra RememberMe -import re/RememberMe.exe.unpacked.exe -overwrite -scriptPath meta/tools/ghidra -postScript LabelUE3Natives.java` |
| Script decompile (.upk) | UE Explorer | .NET WinForms. Usually works under Wine; if not, it's a reason to use Windows. |
| Runtime debugger | x32dbg under Wine (mostly works); `winedbg --gdb` + gdb | Yes, a bit rough |
| Frame capture | RenderDoc (installed) on the Vulkan side (DXVK) | Yes. Labels come from DXVK, not the game. For scripted analysis: `renderdoccmd convert -c xml` (see 07). `*.rdc` is gitignored |
| Live memory inspection | Cheat Engine under Wine; or our own in-DLL ImGui overlay | Yes |
| Logging | our DLL log, `PROTON_LOG`, `DXVK_LOG_LEVEL` | Yes |

## When switching to Windows is worth it

- To get a **vorpX baseline** (see 03): a quick way to see how the game looks in geometric stereo before writing code.
- If x32dbg or UE Explorer turn out unusable under Wine.
- For **compatibility testing** before any public release. On Windows the mod expects DXVK's `d3d9.dll` placed next to the exe, so the interop path stays identical. Check that DXVK's OpenXR extension negotiation also works on Windows; if not, the Windows build may need the OpenVR fallback or a D3D9Ex→D3D11 shared-texture path.

None of these block starting on Linux.
