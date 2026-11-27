#pragma once

#include "vrcore/config.h"

namespace rm::startup {

// Skips the startup movies (legal screen and logos, [FullScreenMovie]
// StartupMovies) by adding UE3's -nostartupmovies switch to the command line
// the game sees: GetCommandLineW/A are hooked from DllMain, before the exe's
// CRT and appCmdLine read it. Story cutscenes are unaffected. [Game]
// SkipIntroMovies. Call from Game::OnAttach.
void Configure(const vrcore::Config& config);

}  // namespace rm::startup
