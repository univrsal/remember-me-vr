#include "vrcore/scene_tap.h"

#include "vrcore/hook.h"
#include "vrcore/log.h"

namespace vrcore::scene_tap {
namespace {

// IDirect3DDevice9 vtable slots (d3d9.h declaration order).
constexpr size_t kGetBackBuffer = 18;
constexpr size_t kSetRenderTarget = 37;
constexpr size_t kGetTexture = 64;
constexpr size_t kDrawPrimitive = 81;
constexpr size_t kDrawIndexedPrimitive = 82;
constexpr size_t kDrawPrimitiveUP = 83;
constexpr size_t kDrawIndexedPrimitiveUP = 84;

using SetRenderTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using DrawPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT,
                                                           UINT, UINT);
using DrawPrimitiveUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUP_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT,
                                                             const void*, D3DFORMAT, const void*, UINT);

SetRenderTarget_t g_origSetRenderTarget = nullptr;
DrawPrimitive_t g_origDrawPrimitive = nullptr;
DrawIndexedPrimitive_t g_origDrawIndexedPrimitive = nullptr;
DrawPrimitiveUP_t g_origDrawPrimitiveUP = nullptr;
DrawIndexedPrimitiveUP_t g_origDrawIndexedPrimitiveUP = nullptr;

// All state is touched only by the thread issuing D3D9 calls (UE3: render thread).
bool g_enabled = false;
IDirect3DSurface9* g_backbuffer = nullptr;  // identity only, refreshed per frame
bool g_backbufferKnown = false;
bool g_watchNextDraw = false;  // backbuffer just bound as RT0
bool g_recorded = false;       // this frame's texture already captured
IDirect3DTexture9* g_scene = nullptr;

void Record(IDirect3DDevice9* device) {
  g_watchNextDraw = false;
  g_recorded = true;
  using GetTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9**);
  auto getTexture = reinterpret_cast<GetTexture_t>((*reinterpret_cast<void***>(device))[kGetTexture]);
  IDirect3DBaseTexture9* base = nullptr;
  if (FAILED(getTexture(device, 0, &base)) || !base) return;
  if (base->GetType() == D3DRTYPE_TEXTURE) {
    g_scene = static_cast<IDirect3DTexture9*>(base);  // keeps GetTexture's reference
  } else {
    base->Release();
  }
}

inline void OnDraw(IDirect3DDevice9* device) {
  if (g_watchNextDraw) Record(device);
}

HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* surface) {
  if (g_enabled && index == 0 && !g_recorded) {
    if (!g_backbufferKnown) {
      using GetBackBuffer_t =
          HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9**);
      auto getBackBuffer = reinterpret_cast<GetBackBuffer_t>((*reinterpret_cast<void***>(self))[kGetBackBuffer]);
      IDirect3DSurface9* bb = nullptr;
      if (SUCCEEDED(getBackBuffer(self, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        g_backbuffer = bb;
        bb->Release();
        g_backbufferKnown = true;
      }
    }
    g_watchNextDraw = surface && surface == g_backbuffer;
  }
  return g_origSetRenderTarget(self, index, surface);
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT start, UINT count) {
  OnDraw(self);
  return g_origDrawPrimitive(self, type, start, count);
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, INT base,
                                                   UINT minIndex, UINT numVertices, UINT start, UINT count) {
  OnDraw(self);
  return g_origDrawIndexedPrimitive(self, type, base, minIndex, numVertices, start, count);
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitiveUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT count,
                                              const void* data, UINT stride) {
  OnDraw(self);
  return g_origDrawPrimitiveUP(self, type, count, data, stride);
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitiveUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT minIndex,
                                                     UINT numVertices, UINT count, const void* indices,
                                                     D3DFORMAT indexFormat, const void* vertices, UINT stride) {
  OnDraw(self);
  return g_origDrawIndexedPrimitiveUP(self, type, minIndex, numVertices, count, indices, indexFormat, vertices,
                                      stride);
}

}  // namespace

void Enable(bool enable) { g_enabled = enable; }
bool Enabled() { return g_enabled; }

IDirect3DTexture9* Take() {
  IDirect3DTexture9* texture = g_scene;
  g_scene = nullptr;
  return texture;
}

namespace detail {

void InstallHooks(IDirect3DDevice9* device) {
  if (!g_enabled || g_origSetRenderTarget) return;
  hook::InstallVTable("IDirect3DDevice9::SetRenderTarget", device, kSetRenderTarget, &HookSetRenderTarget,
                      &g_origSetRenderTarget);
  hook::InstallVTable("IDirect3DDevice9::DrawPrimitive", device, kDrawPrimitive, &HookDrawPrimitive,
                      &g_origDrawPrimitive);
  hook::InstallVTable("IDirect3DDevice9::DrawIndexedPrimitive", device, kDrawIndexedPrimitive,
                      &HookDrawIndexedPrimitive, &g_origDrawIndexedPrimitive);
  hook::InstallVTable("IDirect3DDevice9::DrawPrimitiveUP", device, kDrawPrimitiveUP, &HookDrawPrimitiveUP,
                      &g_origDrawPrimitiveUP);
  hook::InstallVTable("IDirect3DDevice9::DrawIndexedPrimitiveUP", device, kDrawIndexedPrimitiveUP,
                      &HookDrawIndexedPrimitiveUP, &g_origDrawIndexedPrimitiveUP);
  VR_LOG_INFO("scene tap enabled");
}

void EndFrame() {
  if (g_scene) {
    g_scene->Release();
    g_scene = nullptr;
  }
  g_recorded = false;
  g_watchNextDraw = false;
  g_backbufferKnown = false;  // the backbuffer surface may change after Reset
}

}  // namespace detail
}  // namespace vrcore::scene_tap
