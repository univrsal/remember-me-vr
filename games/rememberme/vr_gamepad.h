#pragma once

#include "vrcore/compositor.h"
#include "vrcore/config.h"

#include <cstdint>

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
// PlayerInput.bUsingGamepad (offset of its bool word, bit mask), from UE3
// reflection. The game drops gamepad stick input while it is clear and only
// sets it for a real gamepad, so it is set whenever a VR stick is pushed
// (mouse / keyboard input clears it again as usual).
void SetUsingGamepadFlag(int32_t offset, uint32_t mask);
// early: from DllMain (xinput1_3 must already be loaded, no LoadLibrary);
// otherwise loads it if needed. Installs once.
void Install(bool early = false);

}  // namespace rm::vr_gamepad
