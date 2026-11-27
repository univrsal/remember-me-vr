#pragma once

#include <windows.h>
#include <xinput.h>

#include <cstdint>
#include <string>

#include "engine.h"
#include "vrcore/config.h"

namespace rm::free_cam {

// Free camera: detaches the view from the game camera and flies it around
// while the game keeps running (the player gets no input, so Nilin stands
// still). Toggle: [FreeCam] ToggleKey (F6) or the pad combo PadToggle (both
// stick clicks, which on the Index are the two thumbstick presses).
//
// Controls (flat):  WASD move, E/Q up/down, mouse look, Shift fast, Ctrl slow;
//                   pad: left stick move, right stick look, RT/LT up/down,
//                   RB fast, LB slow.
// Controls (VR):    move toward where the head looks; right stick x turns,
//                   right stick y moves up/down (the head does the pitch).
//
// The view override (scene_view_hooks) only touches the views the game
// renders (GetPlayerViewPoint inside CalcSceneView); gameplay, audio and AI
// still see the game camera. All of this runs on UE3's game thread.

void Configure(const vrcore::Config& config);
bool Enabled();
bool Active();

// FName text lookup for the input hooks ("" until reflection is mapped).
void SetNameLookup(std::string (*nameOf)(int32_t index));

// Once per rendered frame, from the main CalcSceneView call before the views
// are built: toggle (key / pad combo) and movement. `vr`: this frame renders
// the headset's eyes. `head`: the last VR frame's left-eye world rotation
// (movement direction in VR), or null.
void Update(bool vr, const FRotator* head);

// From GetPlayerViewPoint during CalcSceneView: replaces the game camera's
// location/rotation with the free camera's (the first call after toggling on
// starts the free camera there). False if inactive.
bool Apply(FVector* location, FRotator* rotation, bool vr);

// FViewport* (CalcSceneView's argument): hooks the viewport client's
// InputKey / InputAxis once, so keyboard, mouse and gamepad events stop
// reaching the game while the free camera is active (mouse look comes from
// there too).
void InstallInputHooks(void* viewport);

// From the XInputGetState hook: true if the game should see a neutral pad
// (free camera active, or the toggle combo still held).
bool SwallowPad(const XINPUT_GAMEPAD& pad);

}  // namespace rm::free_cam
