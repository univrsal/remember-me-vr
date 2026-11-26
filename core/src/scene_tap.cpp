#include "vrcore/scene_tap.h"

#include "vrcore/hook.h"
#include "vrcore/log.h"

namespace vrcore::scene_tap {
namespace {

// IDirect3DDevice9 vtable slots (d3d9.h declaration order).
constexpr size_t kGetBackBuffer = 18;
constexpr size_t kCreateTexture = 23;
constexpr size_t kSetRenderTarget = 37;
constexpr size_t kClear = 43;
constexpr size_t kSetViewport = 47;
constexpr size_t kGetViewport = 48;
constexpr size_t kSetRenderState = 57;
constexpr size_t kGetRenderState = 58;
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
bool g_uiEnabled = false;

IDirect3DSurface9* g_backbuffer = nullptr;  // identity only, refreshed per frame
D3DSURFACE_DESC g_backbufferDesc{};
bool g_backbufferKnown = false;
bool g_targetIsBackbuffer = false;  // what the game last bound as RT0
bool g_watchNextDraw = false;       // backbuffer just bound as RT0, scene not yet recorded
bool g_recorded = false;            // this frame's scene texture already captured
IDirect3DTexture9* g_scene = nullptr;
IDirect3DBaseTexture9* g_sceneIdentity = nullptr;  // for recognising the copy draws

DrawObserver g_observer = nullptr;
thread_local bool t_inObserver = false;  // no re-entry while an observer adds draws

IDirect3DTexture9* g_ui = nullptr;  // our render target (D3DPOOL_DEFAULT)
IDirect3DSurface9* g_uiSurface = nullptr;
UINT g_uiWidth = 0, g_uiHeight = 0;
bool g_uiRedirected = false;   // RT0 currently swapped to g_uiSurface
bool g_uiDrawnThisFrame = false;

template <typename Fn>
Fn VFn(IDirect3DDevice9* device, size_t slot) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(device))[slot]);
}

void RefreshBackbuffer(IDirect3DDevice9* device) {
  if (g_backbufferKnown) return;
  using GetBackBuffer_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DBACKBUFFER_TYPE,
                                                       IDirect3DSurface9**);
  IDirect3DSurface9* bb = nullptr;
  if (SUCCEEDED(VFn<GetBackBuffer_t>(device, kGetBackBuffer)(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
    g_backbuffer = bb;
    bb->GetDesc(&g_backbufferDesc);
    bb->Release();
    g_backbufferKnown = true;
  }
}

IDirect3DBaseTexture9* TextureAtStage0(IDirect3DDevice9* device) {
  using GetTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9**);
  IDirect3DBaseTexture9* base = nullptr;
  if (FAILED(VFn<GetTexture_t>(device, kGetTexture)(device, 0, &base))) return nullptr;
  return base;  // AddRef'd
}

void RecordScene(IDirect3DDevice9* device) {
  g_watchNextDraw = false;
  g_recorded = true;
  IDirect3DBaseTexture9* base = TextureAtStage0(device);
  if (!base) return;
  if (base->GetType() == D3DRTYPE_TEXTURE) {
    g_scene = static_cast<IDirect3DTexture9*>(base);  // keeps GetTexture's reference
    g_sceneIdentity = base;
  } else {
    base->Release();
  }
}

bool EnsureUiTarget(IDirect3DDevice9* device, UINT width, UINT height) {
  if (g_ui && g_uiWidth == width && g_uiHeight == height) return true;
  detail::ReleaseDeviceResources();
  using CreateTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT,
                                                       D3DPOOL, IDirect3DTexture9**, HANDLE*);
  if (FAILED(VFn<CreateTexture_t>(device, kCreateTexture)(device, width, height, 1, D3DUSAGE_RENDERTARGET,
                                                          D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_ui, nullptr)) ||
      !g_ui || FAILED(g_ui->GetSurfaceLevel(0, &g_uiSurface))) {
    VR_LOG_ERROR("scene tap: creating the %ux%u UI target failed", width, height);
    detail::ReleaseDeviceResources();
    g_uiEnabled = false;
    return false;
  }
  g_uiWidth = width;
  g_uiHeight = height;
  VR_LOG_INFO("scene tap: UI redirected into a %ux%u texture", width, height);
  return true;
}

// Called before a draw that targets the backbuffer and isn't the scene copy.
// Returns true if the draw now goes to the UI target.
bool RedirectUi(IDirect3DDevice9* device) {
  if (g_uiRedirected) return true;
  using GetViewport_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DVIEWPORT9*);
  using SetViewport_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
  using Clear_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
  D3DVIEWPORT9 vp{};
  if (FAILED(VFn<GetViewport_t>(device, kGetViewport)(device, &vp))) return false;
  // Only stereo frames: UI laid out in the left view's rect (left half).
  const UINT half = g_backbufferDesc.Width / 2;
  if (vp.X != 0 || vp.Y != 0 || vp.Width == 0 || vp.Width > half + 1 || vp.Height > g_backbufferDesc.Height) {
    return false;
  }
  if (!EnsureUiTarget(device, vp.Width, g_backbufferDesc.Height)) return false;
  g_origSetRenderTarget(device, 0, g_uiSurface);  // resets the viewport to the full UI target
  VFn<SetViewport_t>(device, kSetViewport)(device, &vp);
  if (!g_uiDrawnThisFrame) {
    VFn<Clear_t>(device, kClear)(device, 0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
    g_uiDrawnThisFrame = true;
  }
  g_uiRedirected = true;
  return true;
}

// Per draw: record the scene copy, or redirect UI with premultiplied-alpha
// blending (restored afterwards so the game's state is untouched).
struct DrawScope {
  IDirect3DDevice9* device;
  bool blendPatched = false;
  bool ui = false;  // this draw went to the UI target
  DWORD separate = 0, srcAlpha = 0, dstAlpha = 0;

  explicit DrawScope(IDirect3DDevice9* d) : device(d) {
    if (!g_enabled) return;
    if (g_watchNextDraw) {
      RecordScene(device);
      return;
    }
    if (!g_uiEnabled || !(g_targetIsBackbuffer || g_uiRedirected) || !g_sceneIdentity) return;
    IDirect3DBaseTexture9* stage0 = TextureAtStage0(device);
    const bool sceneCopy = stage0 && stage0 == g_sceneIdentity;
    if (stage0) stage0->Release();
    if (sceneCopy) {
      if (g_uiRedirected) {  // unexpected order; put the backbuffer back
        g_origSetRenderTarget(device, 0, g_backbuffer);
        g_uiRedirected = false;
      }
      return;
    }
    if (!RedirectUi(device)) return;
    ui = true;
    using GetRenderState_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD*);
    using SetRenderState_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
    auto get = VFn<GetRenderState_t>(device, kGetRenderState);
    auto set = VFn<SetRenderState_t>(device, kSetRenderState);
    DWORD blend = FALSE;
    get(device, D3DRS_ALPHABLENDENABLE, &blend);
    if (!blend) return;
    // Destination alpha accumulates coverage: A = As + Ad * (1 - As).
    get(device, D3DRS_SEPARATEALPHABLENDENABLE, &separate);
    get(device, D3DRS_SRCBLENDALPHA, &srcAlpha);
    get(device, D3DRS_DESTBLENDALPHA, &dstAlpha);
    set(device, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    set(device, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    set(device, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    blendPatched = true;
  }

  ~DrawScope() {
    if (!blendPatched) return;
    using SetRenderState_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
    auto set = VFn<SetRenderState_t>(device, kSetRenderState);
    set(device, D3DRS_SEPARATEALPHABLENDENABLE, separate);
    set(device, D3DRS_SRCBLENDALPHA, srcAlpha);
    set(device, D3DRS_DESTBLENDALPHA, dstAlpha);
  }
};

HRESULT STDMETHODCALLTYPE HookSetRenderTarget(IDirect3DDevice9* self, DWORD index, IDirect3DSurface9* surface) {
  if (g_enabled && index == 0) {
    RefreshBackbuffer(self);
    g_targetIsBackbuffer = surface && surface == g_backbuffer;
    g_uiRedirected = false;  // the game binds its own target now
    g_watchNextDraw = g_targetIsBackbuffer && !g_recorded;
  }
  return g_origSetRenderTarget(self, index, surface);
}

void Notify(IDirect3DDevice9* device, const DrawScope& scope, const DrawCall& call) {
  if (!g_observer || scope.ui || t_inObserver) return;
  t_inObserver = true;
  g_observer(device, call);
  t_inObserver = false;
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT start, UINT count) {
  DrawScope scope(self);
  const HRESULT hr = g_origDrawPrimitive(self, type, start, count);
  if (g_observer) {
    DrawCall call{DrawCall::Kind::Primitive, type};
    call.start = start;
    call.count = count;
    Notify(self, scope, call);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitive(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, INT base,
                                                   UINT minIndex, UINT numVertices, UINT start, UINT count) {
  DrawScope scope(self);
  const HRESULT hr = g_origDrawIndexedPrimitive(self, type, base, minIndex, numVertices, start, count);
  if (g_observer) {
    DrawCall call{DrawCall::Kind::IndexedPrimitive, type};
    call.baseVertex = base;
    call.minIndex = minIndex;
    call.numVertices = numVertices;
    call.start = start;
    call.count = count;
    Notify(self, scope, call);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE HookDrawPrimitiveUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT count,
                                              const void* data, UINT stride) {
  DrawScope scope(self);
  const HRESULT hr = g_origDrawPrimitiveUP(self, type, count, data, stride);
  if (g_observer) {
    DrawCall call{DrawCall::Kind::PrimitiveUP, type};
    call.count = count;
    call.vertices = data;
    call.stride = stride;
    Notify(self, scope, call);
  }
  return hr;
}

HRESULT STDMETHODCALLTYPE HookDrawIndexedPrimitiveUP(IDirect3DDevice9* self, D3DPRIMITIVETYPE type, UINT minIndex,
                                                     UINT numVertices, UINT count, const void* indices,
                                                     D3DFORMAT indexFormat, const void* vertices, UINT stride) {
  DrawScope scope(self);
  const HRESULT hr = g_origDrawIndexedPrimitiveUP(self, type, minIndex, numVertices, count, indices, indexFormat,
                                                  vertices, stride);
  if (g_observer) {
    DrawCall call{DrawCall::Kind::IndexedPrimitiveUP, type};
    call.minIndex = minIndex;
    call.numVertices = numVertices;
    call.count = count;
    call.indices = indices;
    call.indexFormat = indexFormat;
    call.vertices = vertices;
    call.stride = stride;
    Notify(self, scope, call);
  }
  return hr;
}

}  // namespace

void Enable(bool enable) { g_enabled = enable; }
void SetDrawObserver(DrawObserver observer) { g_observer = observer; }

HRESULT DrawCall::Reissue(IDirect3DDevice9* device, const void* otherVertices) const {
  const void* v = otherVertices ? otherVertices : vertices;
  switch (kind) {
    case Kind::Primitive:
      return g_origDrawPrimitive(device, type, start, count);
    case Kind::IndexedPrimitive:
      return g_origDrawIndexedPrimitive(device, type, baseVertex, minIndex, numVertices, start, count);
    case Kind::PrimitiveUP:
      return g_origDrawPrimitiveUP(device, type, count, v, stride);
    case Kind::IndexedPrimitiveUP:
      return g_origDrawIndexedPrimitiveUP(device, type, minIndex, numVertices, count, indices, indexFormat, v,
                                          stride);
  }
  return E_INVALIDARG;
}

HRESULT DrawPrimitiveUPDirect(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT count, const void* vertices,
                              UINT stride) {
  return g_origDrawPrimitiveUP(device, type, count, vertices, stride);
}
void EnableUiRedirect(bool enable) { g_uiEnabled = enable; }
bool Enabled() { return g_enabled; }

IDirect3DTexture9* Take() {
  IDirect3DTexture9* texture = g_scene;
  g_scene = nullptr;
  return texture;
}

IDirect3DTexture9* TakeUi() {
  if (!g_uiDrawnThisFrame || !g_ui) return nullptr;
  g_ui->AddRef();
  return g_ui;
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
  VR_LOG_INFO("scene tap enabled%s", g_uiEnabled ? " (with UI redirect)" : "");
}

void BeforePresent(IDirect3DDevice9* device) {
  // Leave the device as the game expects it: RT0 = backbuffer.
  if (g_uiRedirected && g_backbuffer) {
    g_origSetRenderTarget(device, 0, g_backbuffer);
    g_uiRedirected = false;
  }
}

void EndFrame() {
  if (g_scene) {
    g_scene->Release();
    g_scene = nullptr;
  }
  g_sceneIdentity = nullptr;
  g_recorded = false;
  g_watchNextDraw = false;
  g_uiRedirected = false;
  g_uiDrawnThisFrame = false;
  g_backbufferKnown = false;  // the backbuffer surface may change after Reset
}

void ReleaseDeviceResources() {
  if (g_uiSurface) g_uiSurface->Release();
  if (g_ui) g_ui->Release();
  g_uiSurface = nullptr;
  g_ui = nullptr;
  g_uiWidth = g_uiHeight = 0;
}

}  // namespace detail
}  // namespace vrcore::scene_tap
