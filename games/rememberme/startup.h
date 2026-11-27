#pragma once

#include "vrcore/config.h"

namespace rm::startup {

// Skips the intro: the engine's startup movies (legal screen, [FullScreenMovie]
// StartupMovies) through UE3's -nostartupmovies switch, added to the command
// line the game sees (GetCommandLineW/A hooked from DllMain, before the exe
// reads it), and the Capcom / Unreal / DONTNOD / NVIDIA logos, which DONTNOD's
// movie player plays from script: BinkOpen refuses BIK_Boot_* and StartUpMovie.
// Every movie opened is logged (first 40). Story cutscenes are unaffected.
// [Game] SkipIntroMovies. Call Configure from Game::OnAttach.
void Configure(const vrcore::Config& config);
// If binkw32.dll wasn't loaded at attach time (e.g. from OnDeviceCreated).
void InstallLate();

}  // namespace rm::startup
