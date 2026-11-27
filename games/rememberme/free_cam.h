#pragma once

#include <cstdint>

#include "engine.h"
#include "vrcore/config.h"

namespace rm::free_cam {

// Free camera: detaches the view from the game camera and flies it around
// while the game keeps running (the player gets no input, see input_capture,
// so Nilin stands still). Switched on and off in the config menu.
//
// Controls (flat):  WASD move, E/Q up/down, mouse look, Shift fast, Ctrl slow;
//                   pad: left stick move, right stick look, RT/LT up/down,
//                   RB fast, LB slow.
// Controls (VR):    move toward where the head looks; right stick x turns,
//                   right stick y moves up/down (the head does the pitch).
// It doesn't move while the menu is open.
//
// The view override (scene_view_hooks) only touches the views the game
// renders (GetPlayerViewPoint inside CalcSceneView); gameplay, audio and AI
// still see the game camera.

struct Settings {
  float speed = 600.0f;  // uu/s
  float fastMultiplier = 4.0f;
  float slowMultiplier = 0.25f;
  float lookSpeed = 120.0f;       // deg/s at full stick
  float mouseSensitivity = 0.1f;  // deg per mouse count
  bool invertMouseY = false;
  float deadzone = 0.2f;
};

void Configure(const vrcore::Config& config);
void Save(const vrcore::Config& config);
// Live settings (the menu edits them on the render thread; plain values read
// by the game thread).
Settings& Tunables();

bool Active();
// Any thread. Switching on starts the free camera where the game camera is.
void SetActive(bool active);

// Game thread, once per rendered frame, from the main CalcSceneView call
// before the views are built: movement. `vr`: this frame renders the
// headset's eyes. `head`: the last VR frame's left-eye world rotation
// (movement direction in VR), or null.
void Update(bool vr, const FRotator* head);

// From GetPlayerViewPoint during CalcSceneView: replaces the game camera's
// location/rotation with the free camera's (the first call after switching
// on starts the free camera there). False if inactive.
bool Apply(FVector* location, FRotator* rotation, bool vr);

}  // namespace rm::free_cam
