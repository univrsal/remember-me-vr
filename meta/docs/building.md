# Building

The mod is a 32-bit Windows DLL, cross-compiled on Linux or built on Windows with MSYS2.

Requirements: `mingw-w64-gcc` (i686), CMake ≥ 3.24, Ninja, git. Dependencies (MinHook, Vulkan-Headers, OpenXR-SDK headers, Dear ImGui) are fetched at configure time, pinned to tags.

```sh
cmake --preset mingw-i686-release        # or mingw-i686-debug
cmake --build --preset mingw-i686-release
```

Output: `build/mingw-i686-release/games/rememberme/dinput8.dll` plus `RememberMeVR.ini`. The DLL is self-contained: it only needs KERNEL32 and the Windows CRT.

On Windows, install MSYS2's 32-bit toolchain (`pacman -S mingw-w64-i686-gcc`, which provides the `i686-w64-mingw32-*` names the presets use), put `C:\msys64\mingw32\bin` first on `PATH`, and run the same commands (CMake and Ninja from anywhere on `PATH`). PowerShell:

```powershell
$env:PATH = "C:\msys64\mingw32\bin;$env:PATH"
cmake --preset mingw-i686-release
cmake --build --preset mingw-i686-release
```

To build another game module, add `-DVRMOD_GAME=<name>` to the configure step (default `rememberme`).

Next: [testing.md](testing.md), [deploying.md](deploying.md), [packaging.md](packaging.md).
