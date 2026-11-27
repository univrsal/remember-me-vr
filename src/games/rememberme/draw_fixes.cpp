#include "draw_fixes.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "device_state_hooks.h"
#include "renderer_hooks.h"
#include "sensen_stereo.h"
#include "vrcore/log.h"

namespace rm::draw_fixes {
namespace {

using vrcore::scene_tap::DrawCall;

constexpr size_t kGetRenderTarget = 38;

bool g_fixUvs = true;
bool g_installed = false;
uint64_t g_uvFixes = 0;
std::vector<uint8_t> g_scratch;  // render thread

template <typename Fn>
Fn VFn(IDirect3DDevice9* device, size_t slot) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(device))[slot]);
}

bool RenderTargetSize(IDirect3DDevice9* device, UINT& width, UINT& height) {
  using GetRenderTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9**);
  IDirect3DSurface9* rt = nullptr;
  if (FAILED(VFn<GetRenderTarget_t>(device, kGetRenderTarget)(device, 0, &rt)) || !rt) return false;
  D3DSURFACE_DESC desc{};
  const bool ok = SUCCEEDED(rt->GetDesc(&desc));
  rt->Release();
  width = desc.Width;
  height = desc.Height;
  return ok;
}

UINT VertexCount(const DrawCall& call) {
  if (call.kind == DrawCall::Kind::IndexedPrimitiveUP) return call.minIndex + call.numVertices;
  switch (call.type) {
    case D3DPT_TRIANGLELIST: return call.count * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: return call.count + 2;
    default: return 0;
  }
}

// See the header. Vertices: float4 clip position at 0, float2 UV at 16.
bool FixViewportRelativeUvs(IDirect3DDevice9* device, const DrawCall& call) {
  if (call.kind != DrawCall::Kind::PrimitiveUP && call.kind != DrawCall::Kind::IndexedPrimitiveUP) return false;
  if (!call.vertices || call.stride < 24) return false;
  // Only the right view of a stereo frame: corner viewports of pooled render
  // targets (downsample/bloom/eye-adaptation chains) legitimately use
  // viewport-relative UVs, and the left view starts at x = 0 anyway.
  renderer_hooks::ViewRect left{}, right{};
  if (!renderer_hooks::StereoViewRects(left, right)) return false;
  const D3DVIEWPORT9& vp = device_state::CurrentViewport();
  if (static_cast<int>(vp.X) != right.x || static_cast<int>(vp.Y) != right.y ||
      static_cast<int>(vp.Width) != right.w || static_cast<int>(vp.Height) != right.h || right.x <= 0) {
    return false;
  }
  UINT rtW = 0, rtH = 0, texW = 0, texH = 0;
  if (!RenderTargetSize(device, rtW, rtH)) return false;
  if (!device_state::Stage0TextureSize(texW, texH) || texW != rtW || texH != rtH) return false;
  const UINT n = VertexCount(call);
  if (n < 3 || n > 64) return false;
  const auto* base = static_cast<const uint8_t*>(call.vertices);
  const float tolU = 2.0f / vp.Width, tolV = 2.0f / vp.Height;
  for (UINT i = 0; i < n; ++i) {
    float v[6];
    std::memcpy(v, base + i * call.stride, sizeof(v));
    if (v[3] == 0.0f) return false;
    const float relU = (v[0] / v[3] + 1.0f) * 0.5f, relV = (1.0f - v[1] / v[3]) * 0.5f;
    if (std::fabs(v[4] - relU) > tolU || std::fabs(v[5] - relV) > tolV) return false;
  }
  g_scratch.assign(base, base + n * call.stride);
  for (UINT i = 0; i < n; ++i) {
    float uv[2];
    std::memcpy(uv, g_scratch.data() + i * call.stride + 16, sizeof(uv));
    uv[0] = (static_cast<float>(vp.X) + uv[0] * vp.Width) / texW;
    uv[1] = (static_cast<float>(vp.Y) + uv[1] * vp.Height) / texH;
    std::memcpy(g_scratch.data() + i * call.stride + 16, uv, sizeof(uv));
  }
  call.Reissue(device, g_scratch.data());
  if (g_uvFixes++ == 0) {
    VR_LOG_INFO("viewport-relative UV fix active: %u vertices, viewport %lu,%lu %lux%lu of a %ux%u target", n, vp.X,
                vp.Y, vp.Width, vp.Height, rtW, rtH);
  }
  return true;
}

bool OnDraw(IDirect3DDevice9* device, const DrawCall& call) {
  if (sensen_stereo::OnDraw(device, call)) return true;
  if (g_fixUvs && FixViewportRelativeUvs(device, call)) return true;
  return false;
}

}  // namespace

void Configure(const vrcore::Config& config) { g_fixUvs = config.GetBool("Stereo", "FixQuadTextureSize", true); }

void Install() {
  if (g_installed) return;
  if (!vrcore::scene_tap::Enabled()) {
    VR_LOG_WARN("draw fixes need [VR] StereoSource=scene (the draw hooks); off");
    return;
  }
  vrcore::scene_tap::SetDrawObserver(&OnDraw);
  g_installed = true;
  VR_LOG_INFO("draw fixes: viewport-relative UV fix %s", g_fixUvs ? "on" : "off");
}

}  // namespace rm::draw_fixes
