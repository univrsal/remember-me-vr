#pragma once

#include <windows.h>

#include <cstdint>

#include "vrcore/config.h"

// The game's render size, independent of its window.
//
// The engine sizes everything (scene buffers, view rects, UI layout and the
// D3D9 backbuffer) from what FViewport::UpdateViewportRHI is given: the window
// size, times SuperSampleCount for the scene. With [VR] RenderSize set, that
// function gets the VR size instead, while the window keeps its own size; the
// backbuffer is presented scaled to fit the window (letterboxed), and the
// engine's mouse position is mapped between window and render pixels.
// See meta/docs/08-engine-map.md, "Render size".
namespace rm::render_size {

// DllMain: read [VR] RenderSize / RenderScale. `enabled`: VR on, stereo mode.
void Configure(const vrcore::Config& config, bool enabled);
// Game code unpacked, no viewport yet (first Direct3DCreate9): hook, and for
// RenderSize=auto ask the headset for its eye size.
void Install(bool knownBuild);
// The compositor knows the headset's eye size (its session started): use it
// if the startup probe had none, and resize.
void OnEyeSize(uint32_t eyeWidth, uint32_t eyeHeight);
// After CreateDevice/Reset: re-apply the size if the hook came too late for
// the viewport's creation.
void OnDevice(HWND window, uint32_t backbufferWidth, uint32_t backbufferHeight);
// Configured to decouple the render size from the window.
bool Requested();
// The render size is decoupled from the window right now (letterbox the window).
bool Managed();

}  // namespace rm::render_size
