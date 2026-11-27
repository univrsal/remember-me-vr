#pragma once

#include "vrcore/compositor.h"
#include "vrcore/config.h"

#include <cstdint>
#include <string>

namespace rm::vr_gamepad {

// VR controllers as the game's gamepad: the game reads Xbox controllers through
// xinput1_3 (XInputGetState / XInputSetState, imported by ordinal 2 / 3). Player
// 0 is reported as connected from the start (the game only reads the sticks of
// a gamepad it found connected); while the VR controllers are active they are
// player 0's input, merged with a real gamepad if one is connected; rumble
// goes to the controllers' haptics. [Input] VRControllers.
//
// Index mapping (Touch analogous: X/Y/A/B, menu = Start):
//   thumbsticks (+click) -> sticks (+LS/RS), triggers -> LT/RT,
//   grips -> LB/RB, right A/B -> A/B, left A/B -> X/Y,
//   right trackpad press -> Start, left trackpad press: centre -> Back,
//   edges -> D-pad.
void Configure(const vrcore::Config& config, vrcore::Compositor* compositor);
// Diagnostics: hooks the engine's per-controller XInput processing
// (FUN_00C75600) to log the stick axes and key events it hands to the
// viewport client and whether the client consumed them. `nameOf` turns an
// FName index into text. Known build only.
void InstallInputTrace(std::string (*nameOf)(int32_t index));
// Diagnostics: hooks UObject::ProcessEvent and logs script events whose name
// suggests controller / binding / pause handling, with their first
// parameters. `fullName` gives an object's "Function Pkg.Class.Name".
void InstallEventTrace(std::string (*fullName)(uintptr_t object));
// early: from DllMain (xinput1_3 must already be loaded, no LoadLibrary);
// otherwise loads it if needed. Installs once.
void Install(bool early = false);

}  // namespace rm::vr_gamepad
