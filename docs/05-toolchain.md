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

## Reverse engineering and debugging on Linux

| Task | Tool | Linux? |
|---|---|---|
| Unpack SteamStub for static analysis | Steamless v3.1.0.5 in `tools/steamless/` (gitignored) | **Done.** Runs under mono: `MONO_PATH=tools/steamless/Plugins mono tools/steamless/Steamless.CLI.exe re/RememberMe.exe`. Always work on a copy, never the installed exe. **The unpacked exe stays local; never commit or distribute it.** |
| Disassembly/decompile | Ghidra (installed), r2 | Yes. Keep Ghidra projects in `re/` (gitignored) |
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
