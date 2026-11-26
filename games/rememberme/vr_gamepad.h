#pragma once

#include "vrcore/compositor.h"
#include "vrcore/config.h"

namespace rm::vr_gamepad {

// VR controllers as the game's gamepad: the game reads Xbox controllers through
// xinput1_3 (XInputGetState / XInputSetState, imported by ordinal 2 / 3). While
// the VR controllers are active, player 0's XInputGetState reports them,
// merged with a real gamepad if one is connected; rumble goes to the
// controllers' haptics. [Input] VRControllers.
//
// Index mapping (Touch analogous: X/Y/A/B, menu = Start):
//   thumbsticks (+click) -> sticks (+LS/RS), triggers -> LT/RT,
//   grips -> LB/RB, right A/B -> A/B, left A/B -> X/Y,
//   right trackpad press -> Start, left trackpad press: centre -> Back,
//   edges -> D-pad.
void Configure(const vrcore::Config& config, vrcore::Compositor* compositor);
void Install();

}  // namespace rm::vr_gamepad
