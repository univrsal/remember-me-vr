#pragma once

#include <d3d9.h>

namespace vrcore::scene_tap {

// Splits a frame's final backbuffer work into "scene" and "UI", for games that
// copy their finished scene image into the backbuffer and then draw the UI on
// top (UE3).
//
// Scene: records the texture sampled (stage 0) by the first draw into the
// backbuffer in each frame: the final scene image at the game's internal
// (e.g. supersampled) resolution, before any UI.
//
// UI (optional): in frames where UI draws use a viewport covering only the left
// half of the backbuffer (stereo frames: the UI is laid out in the left view's
// rect), every later draw into the backbuffer that doesn't sample the scene
// texture is redirected into a separate transparent texture of that size, with
// premultiplied-alpha blending. Frames with a full-width UI viewport (mono:
// loading screens, menus without a 3D view) are left alone.
//
// Must be enabled before the D3D9 device is created (installs draw hooks).
void Enable(bool enable);
void EnableUiRedirect(bool enable);
bool Enabled();

// Presenting thread, before the original Present: textures recorded this
// frame, AddRef'd (caller releases), or nullptr.
IDirect3DTexture9* Take();    // scene image
IDirect3DTexture9* TakeUi();  // redirected UI (only if any UI was drawn this frame)

namespace detail {
void InstallHooks(IDirect3DDevice9* device);
void BeforePresent(IDirect3DDevice9* device);  // undo a pending UI redirect
void EndFrame();                               // after Present: drop untaken textures, re-arm
void ReleaseDeviceResources();                 // before Reset
}  // namespace detail

}  // namespace vrcore::scene_tap
