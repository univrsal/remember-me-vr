#pragma once

#include "vrcore/config.h"

namespace rm::startup {

// Skips the intro: the engine's startup movies (legal screen, [FullScreenMovie]
// StartupMovies) through UE3's -nostartupmovies switch, added to the command
// line the game sees (GetCommandLineW/A hooked from DllMain, before the exe
// reads it), and the Capcom / Unreal / DONTNOD / NVIDIA logos, which DONTNOD's
// movie player plays from script: BIK_Boot_* and StartUpMovie jump to their
// last frame right after BinkOpen (refusing the open crashes the game).
// Every movie opened is logged (first 40). Story cutscenes are unaffected.
// [Game] SkipIntroMovies.
// Also adds UE3's -HIGHRES, which stops the engine clamping the windowed
// size to the desktop, so a window (and with it the render size) can exceed
// the monitor. [Game] AllowOversizedWindow. Call Configure from Game::OnAttach.
void Configure(const vrcore::Config& config);
// If binkw32.dll wasn't loaded at attach time (e.g. from OnDeviceCreated).
void InstallLate();

}  // namespace rm::startup
