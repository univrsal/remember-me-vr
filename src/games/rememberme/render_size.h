#pragma once

#include <windows.h>
#include <d3d9.h>

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

// DllMain: read [VR] RenderSize / RenderScale / ForceWindowed / DisableVSync.
// `enabled`: VR on, stereo mode (render size, window); `vr`: VR on (vsync).
void Configure(const vrcore::Config& config, bool enabled, bool vr);
// Game code unpacked, no viewport yet (first Direct3DCreate9, before the
// engine loads its gfx config): hook, and for RenderSize=auto ask the headset
// for its eye size.
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

// Game window and vsync while VR runs, so the game's own display settings
// (fullscreen and vsync by default) don't get in the way:
// - [VR] ForceWindowed: with RenderSize auto/fixed, the game starts windowed
//   (the render size only applies to a window), in a window at the render
//   aspect that fits the screen; also when a windowed size doesn't fit. Done
//   in memory after the game loads its gfx config; when the game saves that
//   config, it writes the player's own values back, not the mod's.
// - [VR] DisableVSync: present without waiting for the monitor (the headset
//   paces the game), so a 60 Hz monitor doesn't cap the headset at 60 fps.
// Both take effect at the next start.
bool ForceWindowed();
void SetForceWindowed(bool on);
bool DisableVSync();
void SetDisableVSync(bool on);
void Save(const vrcore::Config& config);
// Before CreateDevice/Reset: applies DisableVSync.
void AdjustPresentParameters(D3DPRESENT_PARAMETERS& params);

}  // namespace rm::render_size
