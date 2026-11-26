#pragma once

#include <d3d9.h>

namespace vrcore::scene_tap {

// Records the texture sampled (stage 0) by the first draw into the backbuffer
// in each frame. In UE3 that's the copy of the final scene image (at the
// internal, e.g. supersampled, resolution) into the backbuffer, before any UI
// is drawn on top. The compositor validates size/format before using it.
// Must be enabled before the D3D9 device is created (installs draw hooks).
void Enable(bool enable);
bool Enabled();

// Presenting thread: the texture recorded this frame, AddRef'd (caller
// releases), or nullptr. Recording restarts after each Present.
IDirect3DTexture9* Take();

namespace detail {
void InstallHooks(IDirect3DDevice9* device);
void EndFrame();  // after Present: drop an untaken texture, re-arm
}

}  // namespace vrcore::scene_tap
