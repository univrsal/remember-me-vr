#pragma once

#include <d3d9.h>

#include <atomic>

#include "scene_view_hooks.h"
#include "vrcore/compositor.h"
#include "vrcore/config.h"

namespace rm::menu {

// In-game config menu (Dear ImGui). Opened and closed with [Menu] ToggleKey
// (F6) or both stick clicks (input_capture); the game gets no input while it's
// open. Rendered on the presenting thread into a transparent texture shown on
// its own panel in VR (placed in front of the head when it opens) and drawn
// centred into the game window.
//
// Input: mouse (a cursor moved by the swallowed mouse movement), keyboard
// arrows/Enter/Escape, gamepad D-pad/A/B (Index: left trackpad edges, A, B),
// and in VR a laser from either controller's aim pose: point at the panel,
// trigger clicks, thumbstick scrolls.
//
// Settings apply live; "Save" writes them to the INI.

struct Targets {
  vrcore::Config config;                      // the INI (Save writes here)
  vrcore::Compositor* compositor = nullptr;   // null without VR
  SceneViewHooks* sceneViews = nullptr;
  std::atomic<bool>* disableMotionBlur = nullptr;
};

void Configure(const vrcore::Config& config, const Targets& targets);

// Presenting thread, before the compositor: builds and renders the menu if
// it's open. Returns the menu texture for the compositor (owned by the menu,
// valid until the next call) or null.
IDirect3DTexture9* Render(IDirect3DDevice9* device);
// Presenting thread, after the compositor and the mirror: draws the menu
// rendered this frame into the game window.
void DrawToWindow(IDirect3DDevice9* device);

// Before a device Reset (D3DPOOL_DEFAULT resources must be released).
void ReleaseDeviceResources();

}  // namespace rm::menu
