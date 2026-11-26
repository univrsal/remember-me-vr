#include "sensen_stereo.h"

#include <d3d9.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "device_state_hooks.h"
#include "engine.h"
#include "vrcore/log.h"
#include "vrcore/memory.h"
#include "vrcore/scene_tap.h"

namespace rm::sensen_stereo {
namespace {

// IDirect3DDevice9 vtable slots used to change and restore state around the
// added draws (calling through the vtable keeps device_state's tracking right).
constexpr size_t kSetViewport = 47;
constexpr size_t kGetRenderTarget = 38;
constexpr size_t kGetBackBuffer = 18;
constexpr size_t kSetVertexShaderConstantF = 94;
constexpr size_t kGetVertexShaderConstantF = 95;

using Mat = double[4][4];

struct ViewData {
  int x = 0, y = 0, w = 0, h = 0;
  float view[16]{};  // ViewMatrix (row-major, row vectors)
  float proj[16]{};  // ProjectionMatrix
};

bool g_enabled = true;
bool g_installed = false;

// Frame state, touched only on the render thread.
bool t_stereo = false;   // this frame has two views
ViewData t_views[2];
float t_correction[16]{};  // C, row-major
bool t_compositeDone = false;
uint64_t g_hintDraws = 0, g_composites = 0;
bool g_loggedMismatch = false;

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

// Translated view-projection (camera at the origin) and the view origin.
bool TranslatedViewProjection(const ViewData& v, Mat tvp, double origin[3]) {
  Mat view, rot, proj;
  for (int i = 0; i < 16; ++i) {
    view[i / 4][i % 4] = v.view[i];
    proj[i / 4][i % 4] = v.proj[i];
  }
  // view = Translate(-origin) x R: rows 0..2 hold R, row 3 = -origin x R.
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

bool ComputeCorrection() {
  Mat tvpL, tvpR, invL, shift, c;
  double oL[3], oR[3];
  if (!TranslatedViewProjection(t_views[0], tvpL, oL) || !TranslatedViewProjection(t_views[1], tvpR, oR) ||
      !Invert(tvpL, invL)) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    for (int j = 0; j < 4; ++j) shift[i][j] = i == j ? 1.0 : 0.0;
  }
  for (int j = 0; j < 3; ++j) shift[3][j] = oL[j] - oR[j];
  Mul(invL, shift, c);
  Mul(c, tvpR, c);
  for (int i = 0; i < 16; ++i) t_correction[i] = static_cast<float>(c[i / 4][i % 4]);
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

// A Scaleform hint draw in View[0]'s viewport: draw it again for View[1].
void DuplicateHint(IDirect3DDevice9* device, const vrcore::scene_tap::DrawCall& call, int reg) {
  using SetViewport_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
  using ConstF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);
  using GetConstF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, float*, UINT);
  float t[16];
  if (FAILED(VFn<GetConstF_t>(device, kGetVertexShaderConstantF)(device, reg, t, 4))) return;
  // Perspective hints get the right eye's projection; flat (screen-space)
  // elements keep their transform and only move to the right view.
  float tr[16];
  const bool perspective = std::fabs(t[3]) + std::fabs(t[7]) + std::fabs(t[11]) > 1e-9f;
  if (perspective) {
    for (int i = 0; i < 4; ++i) {
      for (int j = 0; j < 4; ++j) {
        float s = 0;
        for (int k = 0; k < 4; ++k) s += t[i * 4 + k] * t_correction[k * 4 + j];
        tr[i * 4 + j] = s;
      }
    }
  } else {
    std::memcpy(tr, t, sizeof(tr));
  }
  const D3DVIEWPORT9 saved = device_state::CurrentViewport();
  const D3DVIEWPORT9 right = RectViewport(t_views[1], saved);
  VFn<ConstF_t>(device, kSetVertexShaderConstantF)(device, reg, tr, 4);
  VFn<SetViewport_t>(device, kSetViewport)(device, &right);
  call.Reissue(device);
  VFn<SetViewport_t>(device, kSetViewport)(device, &saved);
  VFn<ConstF_t>(device, kSetVertexShaderConstantF)(device, reg, t, 4);
  if (g_hintDraws++ == 0) {
    VR_LOG_INFO("sensen stereo: first hint draw duplicated for the right view (%s transform, viewport %d,%d %dx%d)",
                perspective ? "perspective" : "flat", t_views[1].x, t_views[1].y, t_views[1].w, t_views[1].h);
  }
}

// A composite tile (client-memory quad: float4 clip position, float2 UV):
// once per frame, also composite the whole right view, with UVs extended
// from the tile's position-to-UV mapping.
void AddRightComposite(IDirect3DDevice9* device, const vrcore::scene_tap::DrawCall& call) {
  using Vertex = float[6];
  if (call.stride != sizeof(Vertex) || !call.vertices) {
    if (!g_loggedMismatch) {
      g_loggedMismatch = true;
      VR_LOG_INFO("sensen stereo: composite draw with vertex stride %u (expected %u); right view not composited",
                  call.stride, static_cast<unsigned>(sizeof(Vertex)));
    }
    return;
  }
  const auto* v = static_cast<const float*>(call.vertices);
  const UINT n = call.kind == vrcore::scene_tap::DrawCall::Kind::IndexedPrimitiveUP ? call.numVertices + call.minIndex
                                                                                     : call.count + 2;
  // Find two vertices that differ in x and two that differ in y.
  int ix = -1, iy = -1;
  for (UINT i = 1; i < n && (ix < 0 || iy < 0); ++i) {
    const float* a = v;
    const float* b = v + i * 6;
    if (ix < 0 && std::fabs(b[0] / b[3] - a[0] / a[3]) > 1e-6f) ix = static_cast<int>(i);
    if (iy < 0 && std::fabs(b[1] / b[3] - a[1] / a[3]) > 1e-6f) iy = static_cast<int>(i);
  }
  if (ix < 0 || iy < 0) return;
  const float* a = v;
  const float* bx = v + ix * 6;
  const float* by = v + iy * 6;
  const float su = (bx[4] - a[4]) / (bx[0] / bx[3] - a[0] / a[3]);
  const float sv = (by[5] - a[5]) / (by[1] / by[3] - a[1] / a[3]);
  const auto u = [&](float x) { return a[4] + (x - a[0] / a[3]) * su; };
  const auto vv = [&](float y) { return a[5] + (y - a[1] / a[3]) * sv; };
  // Right view rect in the composite viewport's clip space.
  const D3DVIEWPORT9& vp = device_state::CurrentViewport();
  if (vp.Width == 0 || vp.Height == 0) return;
  const ViewData& r = t_views[1];
  const float x0 = (static_cast<float>(r.x) - vp.X) / vp.Width * 2.0f - 1.0f;
  const float x1 = (static_cast<float>(r.x + r.w) - vp.X) / vp.Width * 2.0f - 1.0f;
  const float y0 = 1.0f - (static_cast<float>(r.y) - vp.Y) / vp.Height * 2.0f;
  const float y1 = 1.0f - (static_cast<float>(r.y + r.h) - vp.Y) / vp.Height * 2.0f;
  const float z = a[2] / a[3];
  Vertex quad[4] = {
      {x0, y0, z, 1, u(x0), vv(y0)},
      {x1, y0, z, 1, u(x1), vv(y0)},
      {x0, y1, z, 1, u(x0), vv(y1)},
      {x1, y1, z, 1, u(x1), vv(y1)},
  };
  vrcore::scene_tap::DrawPrimitiveUPDirect(device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
  if (g_composites++ == 0) {
    VR_LOG_INFO("sensen stereo: right-view composite quad x %.3f..%.3f y %.3f..%.3f, u %.4f..%.4f v %.4f..%.4f", x0,
                x1, y0, y1, u(x0), u(x1), vv(y0), vv(y1));
  }
}

void OnDraw(IDirect3DDevice9* device, const vrcore::scene_tap::DrawCall& call) {
  if (!t_stereo) return;
  const D3DVIEWPORT9& vp = device_state::CurrentViewport();
  const int transform = device_state::VertexShaderConstant("Transform");
  if (transform >= 0) {
    if (!SameRect(vp, t_views[0])) {
      if (!g_loggedMismatch) {
        g_loggedMismatch = true;
        VR_LOG_INFO("sensen stereo: Scaleform draw with viewport %lu,%lu %lux%lu (left view %d,%d %dx%d); not "
                    "duplicated",
                    vp.X, vp.Y, vp.Width, vp.Height, t_views[0].x, t_views[0].y, t_views[0].w, t_views[0].h);
      }
      return;
    }
    if (TargetIsBackbuffer(device)) return;  // 2D UI, handled by the UI panel
    DuplicateHint(device, call, transform);
    return;
  }
  if (!t_compositeDone && device_state::PixelShaderConstant("DNESensenParam1") >= 0 &&
      (call.kind == vrcore::scene_tap::DrawCall::Kind::PrimitiveUP ||
       call.kind == vrcore::scene_tap::DrawCall::Kind::IndexedPrimitiveUP)) {
    t_compositeDone = true;
    AddRightComposite(device, call);
  }
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

}  // namespace

void Configure(const vrcore::Config& config) { g_enabled = config.GetBool("Stereo", "SensenStereo", true); }
bool Enabled() { return g_enabled; }

void Install() {
  if (!g_enabled || g_installed) return;
  if (!vrcore::scene_tap::Enabled()) {
    VR_LOG_WARN("sensen stereo needs [VR] StereoSource=scene (the draw hooks); off");
    return;
  }
  vrcore::scene_tap::SetDrawObserver(&OnDraw);
  g_installed = true;
  VR_LOG_INFO("sensen stereo: on");
}

void OnRendererViews(void* renderer) {
  if (!g_installed) return;
  const auto base = reinterpret_cast<uintptr_t>(renderer);
  int32_t count = 0;
  uintptr_t data = 0;
  vrcore::mem::Read(base + kRendererViewsNum, count);
  vrcore::mem::Read(base + kRendererViewsData, data);
  if (count != 2 || !data) {
    t_stereo = false;
    return;
  }
  ReadView(data, t_views[0]);
  ReadView(data + kViewInfoStride, t_views[1]);
  t_stereo = ComputeCorrection();
}

void EndFrame() {
  t_stereo = false;
  t_compositeDone = false;
}

}  // namespace rm::sensen_stereo
