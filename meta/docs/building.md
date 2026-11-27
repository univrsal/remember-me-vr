# Building

The mod is a 32-bit Windows DLL, cross-compiled on Linux.

Requirements: `mingw-w64-gcc` (i686), CMake ≥ 3.24, Ninja, git. Dependencies (MinHook, Vulkan-Headers, OpenXR-SDK headers, Dear ImGui) are fetched at configure time, pinned to tags.

```sh
cmake --preset mingw-i686-release        # or mingw-i686-debug
cmake --build --preset mingw-i686-release
```

Output: `build/mingw-i686-release/games/rememberme/dinput8.dll` plus `RememberMeVR.ini`. The DLL is self-contained: it only needs KERNEL32 and the Windows CRT.

To build another game module, add `-DVRMOD_GAME=<name>` to the configure step (default `rememberme`).

Next: [testing.md](testing.md), [deploying.md](deploying.md), [packaging.md](packaging.md).
