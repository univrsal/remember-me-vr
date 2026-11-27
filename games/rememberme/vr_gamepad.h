#pragma once

#include <windows.h>
#include <xinput.h>

#include "vrcore/compositor.h"
#include "vrcore/config.h"

#include <cstdint>

namespace rm::vr_gamepad {

// VR controllers as the game's gamepad: the game reads Xbox controllers through
// xinput1_3 (XInputGetState / XInputSetState, imported by ordinal 2 / 3). Player
// 0 is reported as connected from the start and registered with the game's
// device list (see InstallDeviceRegistration); while the VR controllers are
// active they are player 0's input, merged with a real gamepad if one is
// connected; rumble goes to the controllers' haptics. [Input] VRControllers.
//
// Index mapping (Touch analogous: X/Y/A/B, menu = Start):
//   thumbsticks (+click) -> sticks (+LS/RS), triggers -> LT/RT,
//   grips -> LB/RB, right A/B -> A/B, left A/B -> X/Y,
//   right trackpad press -> Start, left trackpad press: centre -> Back,
//   edges -> D-pad.
void Configure(const vrcore::Config& config, vrcore::Compositor* compositor);
// Registers player 0's gamepad with the game's input device list. The game
// only treats a controller as a gamepad for gameplay (stick axes, triggers)
// once DirectInput enumeration found it and its WMI check said it is an
// XInput device (UWindowsClient init and every WM_DEVICECHANGE); the VR
// controllers exist only at the XInput level, so a virtual Xbox 360 pad is
// fed to the game's enumeration callback whenever no XInput pad is
// registered. Known build only; call once the exe is unpacked.
void InstallDeviceRegistration();
// Player 0's pad as the game would get it without the free camera (VR
// controllers merged with a real pad). False if none / not installed.
bool ReadPad(XINPUT_GAMEPAD& pad);
// Installed for the VR controllers and for the free camera (which reads the
// pad and hides it from the game while active).
// early: from DllMain (xinput1_3 must already be loaded, no LoadLibrary);
// otherwise loads it if needed. Installs once.
void Install(bool early = false);

}  // namespace rm::vr_gamepad
