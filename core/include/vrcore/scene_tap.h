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
// With the UI redirect: also draw each redirected UI draw into the backbuffer
// (where the game put it), so the window still shows the UI.
void EnableUiMirror(bool enable);
bool Enabled();
bool UiMirrorEnabled();

// Presenting thread, before the original Present: textures recorded this
// frame, AddRef'd (caller releases), or nullptr.
IDirect3DTexture9* Take();    // scene image
IDirect3DTexture9* TakeUi();  // redirected UI (only if any UI was drawn this frame)

// A draw call as the game issued it.
struct DrawCall {
  enum class Kind { Primitive, IndexedPrimitive, PrimitiveUP, IndexedPrimitiveUP };
  Kind kind;
  D3DPRIMITIVETYPE type;
  INT baseVertex = 0;          // IndexedPrimitive
  UINT minIndex = 0;           // indexed kinds
  UINT numVertices = 0;        // indexed kinds
  UINT start = 0;              // Primitive: start vertex; IndexedPrimitive: start index
  UINT count = 0;              // primitive count
  const void* indices = nullptr;   // IndexedPrimitiveUP
  D3DFORMAT indexFormat = D3DFMT_UNKNOWN;
  const void* vertices = nullptr;  // UP kinds
  UINT stride = 0;                 // UP kinds

  // Issues the same draw again with the device's current state, bypassing
  // the hooks. For UP kinds, `otherVertices` (same layout and count) replaces
  // the game's vertex data.
  HRESULT Reissue(IDirect3DDevice9* device, const void* otherVertices = nullptr) const;
};

// Called on the drawing thread before every draw that isn't redirected into
// the UI texture (i.e. scene and offscreen work), with the draw's state bound.
// The observer may change state, add draws and Reissue() the draw (e.g. once
// per view), but must restore the state it changed. Returning true means the
// observer issued everything itself and the game's draw is skipped. One observer.
using DrawObserver = bool (*)(IDirect3DDevice9* device, const DrawCall& call);
void SetDrawObserver(DrawObserver observer);

// While alive, the calling thread's render-target changes and draws pass the
// hooks untouched (no scene recording, UI redirect or observer): for the mod's
// own drawing, e.g. an overlay rendered at Present.
class Bypass {
 public:
  Bypass();
  ~Bypass();
  Bypass(const Bypass&) = delete;
  Bypass& operator=(const Bypass&) = delete;
};

// Draws client-memory vertices bypassing the hooks (for observers adding draws).
HRESULT DrawPrimitiveUPDirect(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT count, const void* vertices,
                              UINT stride);

namespace detail {
void InstallHooks(IDirect3DDevice9* device);
void BeforePresent(IDirect3DDevice9* device);  // undo a pending UI redirect
void EndFrame();                               // after Present: drop untaken textures, re-arm
void ReleaseDeviceResources();                 // before Reset
}  // namespace detail

}  // namespace vrcore::scene_tap
