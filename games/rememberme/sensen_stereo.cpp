#include "sensen_stereo.h"

#include <d3d9.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <utility>

#include "device_state_hooks.h"
#include "engine.h"
#include "vrcore/log.h"
#include "vrcore/memory.h"
#include "vrcore/scene_tap.h"

namespace rm::sensen_stereo {
namespace {

using vrcore::scene_tap::DrawCall;

// IDirect3DDevice9 vtable slots used to change and restore state around the
// added draws (calling through the vtable keeps device_state's tracking right).
constexpr size_t kGetBackBuffer = 18;
constexpr size_t kGetRenderTarget = 38;
constexpr size_t kSetViewport = 47;
constexpr size_t kSetVertexShaderConstantF = 94;
constexpr size_t kGetVertexShaderConstantF = 95;
constexpr size_t kSetStreamSource = 100;
constexpr size_t kGetStreamSource = 101;

using Mat = double[4][4];

struct ViewData {
  int x = 0, y = 0, w = 0, h = 0;
  float view[16]{};  // ViewMatrix (row-major, row vectors)
  float proj[16]{};  // ProjectionMatrix
};

struct GameCamera {
  bool valid = false;
  float key[16]{};  // left-eye ViewMatrix of the frame
  float view[16]{};
  float proj[16]{};
};

bool g_enabled = true;
bool g_installed = false;

// Game thread -> render thread: the last few frames' game cameras.
constexpr int kCameraSlots = 8;
std::mutex g_camerasMutex;
GameCamera g_cameras[kCameraSlots];
int g_nextCamera = 0;

// Frame state, touched only on the render thread.
bool g_stereo = false;  // this frame's hints are redrawn: views and corrections known
int g_viewCount = 0;    // 2 (stereo) or 1 (mono, free camera)
ViewData g_views[2];
float g_correction[2][16]{};  // C_eye, row-major
bool g_compositeDone = false;  // this frame's composite quads drawn
uint64_t g_hintDraws = 0, g_composites = 0, g_frames = 0, g_monoFrames = 0;
bool g_loggedViewportMismatch = false;
bool g_loggedNoCamera = false;

template <typename Fn>
Fn VFn(IDirect3DDevice9* device, size_t slot) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(device))[slot]);
}

void Mul(const Mat a, const Mat b, Mat out) {
  Mat r;
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      double s = 0;
      for (int k = 0; k < 4; ++k) s += a[i][k] * b[k][j];
      r[i][j] = s;
    }
  }
  std::memcpy(out, r, sizeof(Mat));
}

bool Invert(const Mat m, Mat out) {
  double a[4][8];
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      a[i][j] = m[i][j];
      a[i][j + 4] = i == j ? 1.0 : 0.0;
    }
  }
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int r = c + 1; r < 4; ++r) {
      if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
    }
    if (std::fabs(a[pivot][c]) < 1e-12) return false;
    if (pivot != c) {
      for (int j = 0; j < 8; ++j) std::swap(a[c][j], a[pivot][j]);
    }
    const double d = a[c][c];
    for (int j = 0; j < 8; ++j) a[c][j] /= d;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      const double f = a[r][c];
      for (int j = 0; j < 8; ++j) a[r][j] -= f * a[c][j];
    }
  }
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) out[i][j] = a[i][j + 4];
  }
  return true;
}

// Translated view-projection (camera at the origin) and the view origin, from
// a view matrix (Translate(-origin) x R) and a projection.
bool TranslatedViewProjection(const float* viewM, const float* projM, Mat tvp, double origin[3]) {
  Mat view, rot, proj;
  for (int i = 0; i < 16; ++i) {
    view[i / 4][i % 4] = viewM[i];
    proj[i / 4][i % 4] = projM[i];
  }
  std::memcpy(rot, view, sizeof(Mat));
  rot[3][0] = rot[3][1] = rot[3][2] = 0.0;
  rot[3][3] = 1.0;
  Mat inv;
  if (!Invert(rot, inv)) return false;
  for (int j = 0; j < 3; ++j) {
    origin[j] = -(view[3][0] * inv[0][j] + view[3][1] * inv[1][j] + view[3][2] * inv[2][j]);
  }
  Mul(rot, proj, tvp);
  return true;
}

// C = inverse(TVP_from) x Translate(origin_from - origin_to) x TVP_to.
bool Correction(const float* fromView, const float* fromProj, const float* toView, const float* toProj, float* out) {
  Mat tvpFrom, tvpTo, invFrom, shift, c;
  double oFrom[3], oTo[3];
  if (!TranslatedViewProjection(fromView, fromProj, tvpFrom, oFrom) ||
      !TranslatedViewProjection(toView, toProj, tvpTo, oTo) || !Invert(tvpFrom, invFrom)) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) shift[i][j] = i == j ? 1.0 : 0.0;
  }
  for (int j = 0; j < 3; ++j) shift[3][j] = oFrom[j] - oTo[j];
  Mul(invFrom, shift, c);
  Mul(c, tvpTo, c);
  for (int i = 0; i < 16; ++i) out[i] = static_cast<float>(c[i / 4][i % 4]);
  return true;
}

bool SameRect(const D3DVIEWPORT9& vp, const ViewData& v) {
  return static_cast<int>(vp.X) == v.x && static_cast<int>(vp.Y) == v.y && static_cast<int>(vp.Width) == v.w &&
         static_cast<int>(vp.Height) == v.h;
}

bool TargetIsBackbuffer(IDirect3DDevice9* device) {
  using GetRenderTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9**);
  using GetBackBuffer_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DBACKBUFFER_TYPE,
                                                       IDirect3DSurface9**);
  IDirect3DSurface9 *rt = nullptr, *bb = nullptr;
  VFn<GetRenderTarget_t>(device, kGetRenderTarget)(device, 0, &rt);
  VFn<GetBackBuffer_t>(device, kGetBackBuffer)(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
  const bool same = rt && rt == bb;
  if (rt) rt->Release();
  if (bb) bb->Release();
  return same;
}

D3DVIEWPORT9 RectViewport(const ViewData& v, const D3DVIEWPORT9& like) {
  D3DVIEWPORT9 vp = like;
  vp.X = static_cast<DWORD>(v.x);
  vp.Y = static_cast<DWORD>(v.y);
  vp.Width = static_cast<DWORD>(v.w);
  vp.Height = static_cast<DWORD>(v.h);
  return vp;
}

void Multiply(const float* t, const float* c, float* out) {
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) {
      float s = 0;
      for (int k = 0; k < 4; ++k) s += t[i * 4 + k] * c[k * 4 + j];
      out[i * 4 + j] = s;
    }
  }
}

// A Scaleform hint draw in View[0]'s viewport: draw it once per eye.
bool DrawHintPerEye(IDirect3DDevice9* device, const DrawCall& call, int reg) {
  using SetViewport_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
  using ConstF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
  using GetConstF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, float*, UINT);
  float t[16];
  if (FAILED(VFn<GetConstF_t>(device, kGetVertexShaderConstantF)(device, reg, t, 4))) return false;
  // Perspective hints get each eye's projection; flat (screen-space)
  // elements keep their transform and are only repeated in the right view.
  const bool perspective = std::fabs(t[3]) + std::fabs(t[7]) + std::fabs(t[11]) > 1e-9f;
  const D3DVIEWPORT9 saved = device_state::CurrentViewport();
  auto set = VFn<ConstF_t>(device, kSetVertexShaderConstantF);
  auto viewport = VFn<SetViewport_t>(device, kSetViewport);
  float eyeT[16];
  for (int eye = 0; eye < g_viewCount; ++eye) {
    if (perspective) {
      Multiply(t, g_correction[eye], eyeT);
      set(device, reg, eyeT, 4);
    }
    const D3DVIEWPORT9 vp = RectViewport(g_views[eye], saved);
    viewport(device, &vp);
    call.Reissue(device);
  }
  viewport(device, &saved);
  set(device, reg, t, 4);
  if (g_hintDraws++ == 0) {
    VR_LOG_INFO("sensen stereo: first hint draw redrawn per view (%s transform; %d view(s), last %d,%d %dx%d)",
                perspective ? "perspective" : "flat", g_viewCount, g_views[g_viewCount - 1].x,
                g_views[g_viewCount - 1].y, g_views[g_viewCount - 1].w, g_views[g_viewCount - 1].h);
  }
  return true;
}

// The game composites tiles where *it* thinks the hints are (game camera), so
// with the hints moved per eye its tiles can miss them. In stereo frames the
// game's tile draws are replaced by one quad per view, drawn at the first tile
// (later tiles are skipped). The composite discards pixels without hint
// content, so covering a whole view is safe. The quads map the layer 1:1 onto
// the viewport (UV = pixel / viewport size) with D3D9's half-pixel offset;
// vertices are float4 position + float2 UV.
bool CompositeBothViews(IDirect3DDevice9* device, const DrawCall& call) {
  using GetStreamSource_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9**, UINT*,
                                                         UINT*);
  using SetStreamSource_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, IDirect3DVertexBuffer9*, UINT, UINT);
  const D3DVIEWPORT9& vp = device_state::CurrentViewport();
  if (vp.Width == 0 || vp.Height == 0) return false;
  const float w = static_cast<float>(vp.Width), h = static_cast<float>(vp.Height);
  struct Vertex {
    float x, y, z, w, u, v;
  };
  const auto x = [&](float px) { return (px - 0.5f) / w * 2.0f - 1.0f; };
  const auto y = [&](float py) { return 1.0f - (py - 0.5f) / h * 2.0f; };
  // DrawPrimitiveUP unbinds stream 0; UE3 caches it, so put it back.
  IDirect3DVertexBuffer9* stream = nullptr;
  UINT offset = 0, stride = 0;
  VFn<GetStreamSource_t>(device, kGetStreamSource)(device, 0, &stream, &offset, &stride);
  for (int i = 0; i < g_viewCount; ++i) {
    const ViewData& r = g_views[i];
    const float px0 = static_cast<float>(r.x) - vp.X, px1 = px0 + r.w;
    const float py0 = static_cast<float>(r.y) - vp.Y, py1 = py0 + r.h;
    const Vertex quad[4] = {
        {x(px0), y(py0), 0, 1, px0 / w, py0 / h},
        {x(px1), y(py0), 0, 1, px1 / w, py0 / h},
        {x(px0), y(py1), 0, 1, px0 / w, py1 / h},
        {x(px1), y(py1), 0, 1, px1 / w, py1 / h},
    };
    vrcore::scene_tap::DrawPrimitiveUPDirect(device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
  }
  VFn<SetStreamSource_t>(device, kSetStreamSource)(device, 0, stream, offset, stride);
  if (stream) stream->Release();
  if (g_composites++ == 0) {
    VR_LOG_INFO("sensen stereo: composite replaced by one quad per view (game's draw kind %d, viewport %lux%lu)",
                static_cast<int>(call.kind), vp.Width, vp.Height);
  }
  return true;
}

bool HandleDraw(IDirect3DDevice9* device, const DrawCall& call) {
  if (!g_stereo) return false;
  const int transform = device_state::VertexShaderConstant("Transform");
  if (transform >= 0) {
    const D3DVIEWPORT9& vp = device_state::CurrentViewport();
    if (!SameRect(vp, g_views[0])) {
      if (!g_loggedViewportMismatch) {
        g_loggedViewportMismatch = true;
        VR_LOG_INFO("sensen stereo: Scaleform draw with viewport %lu,%lu %lux%lu (left view %d,%d %dx%d) left alone",
                    vp.X, vp.Y, vp.Width, vp.Height, g_views[0].x, g_views[0].y, g_views[0].w, g_views[0].h);
      }
      return false;
    }
    if (TargetIsBackbuffer(device)) return false;  // 2D UI, handled by the UI panel
    return DrawHintPerEye(device, call, transform);
  }
  if (device_state::PixelShaderConstant("DNESensenParam1") >= 0) {
    if (g_compositeDone) return true;  // the game's other tiles: already covered
    g_compositeDone = true;
    return CompositeBothViews(device, call);
  }
  return false;
}

void ReadView(uintptr_t view, ViewData& out) {
  vrcore::mem::Read(view + kViewRenderTargetX, out.x);
  vrcore::mem::Read(view + kViewRenderTargetX + 4, out.y);
  vrcore::mem::Read(view + kViewRenderTargetX + 8, out.w);
  vrcore::mem::Read(view + kViewRenderTargetX + 12, out.h);
  for (int i = 0; i < 16; ++i) {
    vrcore::mem::Read(view + kViewViewMatrix + i * 4, out.view[i]);
    vrcore::mem::Read(view + kViewProjectionMatrix + i * 4, out.proj[i]);
  }
}

bool FindGameCamera(const float* leftView, GameCamera& out) {
  std::lock_guard<std::mutex> lock(g_camerasMutex);
  for (const GameCamera& c : g_cameras) {
    if (c.valid && std::memcmp(c.key, leftView, sizeof(c.key)) == 0) {
      out = c;
      return true;
    }
  }
  return false;
}

}  // namespace

void Configure(const vrcore::Config& config) { g_enabled = config.GetBool("Stereo", "SensenStereo", true); }
bool Enabled() { return g_enabled; }

void Install() {
  if (!g_enabled || g_installed) return;
  if (!vrcore::scene_tap::Enabled()) {
    VR_LOG_WARN("sensen stereo needs [VR] StereoSource=scene (the draw hooks); off");
    return;
  }
  g_installed = true;
  VR_LOG_INFO("sensen stereo: on");
}

bool OnDraw(IDirect3DDevice9* device, const DrawCall& call) { return g_installed && HandleDraw(device, call); }

void PushGameCamera(const float leftEyeView[16], const float gameView[16], const float gameProj[16]) {
  if (!g_installed) return;
  std::lock_guard<std::mutex> lock(g_camerasMutex);
  GameCamera& c = g_cameras[g_nextCamera];
  g_nextCamera = (g_nextCamera + 1) % kCameraSlots;
  std::memcpy(c.key, leftEyeView, sizeof(c.key));
  std::memcpy(c.view, gameView, sizeof(c.view));
  std::memcpy(c.proj, gameProj, sizeof(c.proj));
  c.valid = true;
}

void OnRendererViews(void* renderer) {
  if (!g_installed) return;
  const auto base = reinterpret_cast<uintptr_t>(renderer);
  int32_t count = 0;
  uintptr_t data = 0;
  vrcore::mem::Read(base + kRendererViewsNum, count);
  vrcore::mem::Read(base + kRendererViewsData, data);
  // Two views: stereo. One view: a mono frame, redrawn only if a game camera
  // was queued for it (free camera: the hints are built for the game camera,
  // not the view).
  if ((count != 1 && count != 2) || !data) {
    g_stereo = false;
    return;
  }
  g_viewCount = count;
  ReadView(data, g_views[0]);
  if (count == 2) ReadView(data + kViewInfoStride, g_views[1]);
  GameCamera camera;
  if (!FindGameCamera(g_views[0].view, camera)) {
    if (count == 2 && !g_loggedNoCamera) {
      g_loggedNoCamera = true;
      VR_LOG_WARN("sensen stereo: no game camera recorded for this frame's views; hints not redrawn");
    }
    g_stereo = false;
    return;
  }
  g_stereo = true;
  for (int i = 0; i < count; ++i) {
    g_stereo = g_stereo && Correction(camera.view, camera.proj, g_views[i].view, g_views[i].proj, g_correction[i]);
  }
  if (g_stereo && count == 2 && g_frames++ == 0) {
    VR_LOG_INFO("sensen stereo: first stereo frame matched to its game camera");
  }
  if (g_stereo && count == 1 && g_monoFrames++ == 0) {
    VR_LOG_INFO("sensen stereo: first mono frame re-projected from the game camera (free camera)");
  }
}

void EndFrame() {
  g_stereo = false;
  g_compositeDone = false;
}

}  // namespace rm::sensen_stereo
