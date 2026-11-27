# Adding another game

1. Create `src/games/<name>/` with:
   - a `game.cpp` implementing `vrcore::Game` and defining `vrcore::GetGame()`;
   - a `CMakeLists.txt` calling `vrcore_add_mod(<Target> PROXY <dll> SOURCES game.cpp)`;
   - a default `.ini`, `deploy.env`, `smoke.env`, `package.env` and `INSTALL.txt` (copy the Remember Me ones).
2. Pick a proxy DLL the game imports, and that isn't `d3d9.dll` (that would replace DXVK). If `src/core/proxy/<dll>/` doesn't exist yet, add one modelled on `src/core/proxy/dinput8/`.
3. Configure with `-DVRMOD_GAME=<name>` and add the game to the CI matrix.

`Game::OnDeviceCreated` is the first safe point to hook game code. On SteamStub-protected executables, `.text` is still encrypted while `DllMain` runs.
