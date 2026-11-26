#include "depth_downsample_diag.h"

#include <atomic>
#include <cstdint>

#include "engine.h"
#include "vrcore/hook.h"
#include "vrcore/log.h"
#include "vrcore/memory.h"
#include "vrcore/process.h"

namespace rm::depth_downsample_diag {
namespace {

// IDirect3DDevice9 vtable slots (d3d9.h declaration order). scene_tap owns the
// draw and SetRenderTarget hooks; these are the ones nobody else hooks.
constexpr size_t kSetViewport = 47;
constexpr size_t kSetTexture = 65;
constexpr size_t kSetSamplerState = 69;
constexpr size_t kSetPixelShader = 107;
constexpr size_t kSetPixelShaderConstantF = 109;

using SetViewport_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using SetTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using SetSamplerState_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, D3DSAMPLERSTATETYPE, DWORD);
using SetPixelShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetPixelShaderConstantF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);

SetViewport_t g_origSetViewport = nullptr;
SetTexture_t g_origSetTexture = nullptr;
SetSamplerState_t g_origSetSamplerState = nullptr;
SetPixelShader_t g_origSetPixelShader = nullptr;
SetPixelShaderConstantF_t g_origSetPixelShaderConstantF = nullptr;
DownsampleSceneDepthFn g_origDownsample = nullptr;

int g_logCalls = 0;                // from the config; 0 = off
std::atomic<int> g_remaining{0};   // stereo calls still to log
uint64_t g_logged = 0;             // calls logged so far (numbering in the log)
thread_local bool t_inPass = false;  // inside a logged DownsampleSceneDepth (render thread)

const char* SamplerStateName(D3DSAMPLERSTATETYPE t) {
  switch (t) {
    case D3DSAMP_ADDRESSU: return "ADDRESSU";
    case D3DSAMP_ADDRESSV: return "ADDRESSV";
    case D3DSAMP_ADDRESSW: return "ADDRESSW";
    case D3DSAMP_BORDERCOLOR: return "BORDERCOLOR";
    case D3DSAMP_MAGFILTER: return "MAGFILTER";
    case D3DSAMP_MINFILTER: return "MINFILTER";
    case D3DSAMP_MIPFILTER: return "MIPFILTER";
    case D3DSAMP_MIPMAPLODBIAS: return "MIPMAPLODBIAS";
    case D3DSAMP_MAXMIPLEVEL: return "MAXMIPLEVEL";
    case D3DSAMP_MAXANISOTROPY: return "MAXANISOTROPY";
    case D3DSAMP_SRGBTEXTURE: return "SRGBTEXTURE";
    case D3DSAMP_ELEMENTINDEX: return "ELEMENTINDEX";
    case D3DSAMP_DMAPOFFSET: return "DMAPOFFSET";
    default: return "?";
  }
}

HRESULT STDMETHODCALLTYPE HookSetViewport(IDirect3DDevice9* self, const D3DVIEWPORT9* vp) {
  if (t_inPass && vp) {
    VR_LOG_INFO("  SetViewport x=%lu y=%lu w=%lu h=%lu z=%.3f..%.3f", vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ,
                vp->MaxZ);
  }
  return g_origSetViewport(self, vp);
}

HRESULT STDMETHODCALLTYPE HookSetTexture(IDirect3DDevice9* self, DWORD stage, IDirect3DBaseTexture9* texture) {
  if (t_inPass) {
    D3DSURFACE_DESC desc{};
    bool have = false;
    if (texture && texture->GetType() == D3DRTYPE_TEXTURE) {
      have = SUCCEEDED(static_cast<IDirect3DTexture9*>(texture)->GetLevelDesc(0, &desc));
    }
    if (have) {
      VR_LOG_INFO("  SetTexture s%lu = %p (%ux%u fmt %d usage 0x%lx)", stage, static_cast<void*>(texture), desc.Width,
                  desc.Height, static_cast<int>(desc.Format), desc.Usage);
    } else {
      VR_LOG_INFO("  SetTexture s%lu = %p", stage, static_cast<void*>(texture));
    }
  }
  return g_origSetTexture(self, stage, texture);
}

HRESULT STDMETHODCALLTYPE HookSetSamplerState(IDirect3DDevice9* self, DWORD sampler, D3DSAMPLERSTATETYPE type,
                                              DWORD value) {
  if (t_inPass) VR_LOG_INFO("  SetSamplerState s%lu %s = %lu (0x%08lx)", sampler, SamplerStateName(type), value, value);
  return g_origSetSamplerState(self, sampler, type, value);
}

HRESULT STDMETHODCALLTYPE HookSetPixelShader(IDirect3DDevice9* self, IDirect3DPixelShader9* shader) {
  if (t_inPass) VR_LOG_INFO("  SetPixelShader %p", static_cast<void*>(shader));
  return g_origSetPixelShader(self, shader);
}

HRESULT STDMETHODCALLTYPE HookSetPixelShaderConstantF(IDirect3DDevice9* self, UINT reg, const float* data,
                                                      UINT count) {
  if (t_inPass && data) {
    for (UINT i = 0; i < count; ++i) {
      const float* v = data + i * 4;
      VR_LOG_INFO("  SetPixelShaderConstantF c%u = (%.9g, %.9g, %.9g, %.9g)", reg + i, v[0], v[1], v[2], v[3]);
    }
  }
  return g_origSetPixelShaderConstantF(self, reg, data, count);
}

template <typename T>
T At(uintptr_t base, uint32_t offset) {
  T value{};
  vrcore::mem::Read(base + offset, value);
  return value;
}

void LogView(int index, uintptr_t view) {
  VR_LOG_INFO("  view %d @ %p: rect x=%d y=%d w=%d h=%d, +0x%x (scene-colour sampler switch) = 0x%08x", index,
              reinterpret_cast<void*>(view), At<int32_t>(view, kViewRenderTargetX),
              At<int32_t>(view, kViewRenderTargetX + 4), At<int32_t>(view, kViewRenderTargetX + 8),
              At<int32_t>(view, kViewRenderTargetX + 12), kViewSceneColorSamplerSwitch,
              At<uint32_t>(view, kViewSceneColorSamplerSwitch));
  for (int row = 0; row < 4; ++row) {
    const uint32_t o = kViewProjectionMatrix + row * 16;
    VR_LOG_INFO("    proj row %d: %.9g %.9g %.9g %.9g", row, At<float>(view, o), At<float>(view, o + 4),
                At<float>(view, o + 8), At<float>(view, o + 12));
  }
}

void __fastcall HookDownsample(void* renderer, void* /*edx*/) {
  const auto base = reinterpret_cast<uintptr_t>(renderer);
  const int32_t views = At<int32_t>(base, kRendererViewsNum);
  const bool log = views >= 2 && g_remaining.load(std::memory_order_relaxed) > 0;
  if (!log) {
    g_origDownsample(renderer);
    return;
  }
  g_remaining.fetch_sub(1, std::memory_order_relaxed);
  const uintptr_t data = At<uintptr_t>(base, kRendererViewsData);
  VR_LOG_INFO("DownsampleSceneDepth #%llu: renderer %p, %d views", static_cast<unsigned long long>(++g_logged),
              renderer, views);
  for (int i = 0; i < views && i < 4; ++i) LogView(i, data + static_cast<uintptr_t>(i) * kViewInfoStride);
  t_inPass = true;
  g_origDownsample(renderer);
  t_inPass = false;
  VR_LOG_INFO("DownsampleSceneDepth #%llu end", static_cast<unsigned long long>(g_logged));
}

}  // namespace

void Configure(const vrcore::Config& config) {
  g_logCalls = config.GetInt("Debug", "DepthDownsampleLog", 0);
  g_remaining = g_logCalls;
}

void Install(IDirect3DDevice9* device) {
  if (g_logCalls <= 0 || g_origDownsample) return;
  auto* fn = vrcore::Exe().FromPreferredVA<uint8_t>(kDownsampleSceneDepth);
  if (!vrcore::mem::Matches(reinterpret_cast<uintptr_t>(fn), kDownsampleSceneDepthPrologueA,
                            sizeof(kDownsampleSceneDepthPrologueA)) ||
      !vrcore::mem::Matches(reinterpret_cast<uintptr_t>(fn) + kDownsampleSceneDepthPrologueBOffset,
                            kDownsampleSceneDepthPrologueB, sizeof(kDownsampleSceneDepthPrologueB))) {
    VR_LOG_ERROR("depth downsample diagnostic: DownsampleSceneDepth prologue doesn't match this build");
    return;
  }
  bool ok = vrcore::hook::Install("DownsampleSceneDepth", reinterpret_cast<void*>(fn),
                                  reinterpret_cast<void*>(&HookDownsample), reinterpret_cast<void**>(&g_origDownsample));
  ok = ok && vrcore::hook::InstallVTable("IDirect3DDevice9::SetViewport", device, kSetViewport, &HookSetViewport,
                                         &g_origSetViewport);
  ok = ok && vrcore::hook::InstallVTable("IDirect3DDevice9::SetTexture", device, kSetTexture, &HookSetTexture,
                                         &g_origSetTexture);
  ok = ok && vrcore::hook::InstallVTable("IDirect3DDevice9::SetSamplerState", device, kSetSamplerState,
                                         &HookSetSamplerState, &g_origSetSamplerState);
  ok = ok && vrcore::hook::InstallVTable("IDirect3DDevice9::SetPixelShader", device, kSetPixelShader,
                                         &HookSetPixelShader, &g_origSetPixelShader);
  ok = ok && vrcore::hook::InstallVTable("IDirect3DDevice9::SetPixelShaderConstantF", device,
                                         kSetPixelShaderConstantF, &HookSetPixelShaderConstantF,
                                         &g_origSetPixelShaderConstantF);
  VR_LOG_INFO("depth downsample diagnostic %s: logging %d stereo call(s), re-armed by the side-by-side toggle",
              ok ? "installed" : "FAILED", g_logCalls);
}

void Rearm() { g_remaining = g_logCalls; }

}  // namespace rm::depth_downsample_diag
