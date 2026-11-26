#include "renderer_hooks.h"

#include <atomic>
#include <cstdint>

#include "device_state_hooks.h"
#include "engine.h"
#include "sensen_stereo.h"
#include "vrcore/hook.h"
#include "vrcore/log.h"
#include "vrcore/memory.h"
#include "vrcore/process.h"

namespace rm::renderer_hooks {
namespace {

DownsampleSceneDepthFn g_origDownsample = nullptr;
DrawDenormalizedQuadFn g_origDrawDenormalizedQuad = nullptr;
bool g_fixQuadTextureSize = true;
uint64_t g_quadFixes = 0;

int g_logCalls = 0;               // from the config; 0 = off
std::atomic<int> g_remaining{0};  // stereo calls still to log
uint64_t g_logged = 0;            // calls logged so far (numbering in the log)

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
  sensen_stereo::OnRendererViews(renderer);
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
  device_state::SetTrace(true);
  g_origDownsample(renderer);
  device_state::SetTrace(false);
  VR_LOG_INFO("DownsampleSceneDepth #%llu end", static_cast<unsigned long long>(g_logged));
}

// Some quads (e.g. the per-view scene-colour copy before translucency, which
// soft particles and fog read depth from) pass view-relative U/V and the
// viewport's size as the texture size, although the texture is the whole
// scene buffer. In mono the view covers the buffer, so it works; for a stereo
// view it samples the wrong part of the buffer (stretched from x = 0). If the
// texture size given equals the viewport size, the viewport doesn't start at
// 0,0 horizontally or is narrower than the bound texture (a stereo view):
// add the viewport offset to U/V and use the texture's real size.
void HookDrawDenormalizedQuad(float x, float y, float sizeX, float sizeY, float u, float v, float sizeU, float sizeV,
                              uint32_t targetSizeX, uint32_t targetSizeY, uint32_t textureSizeX,
                              uint32_t textureSizeY, float z) {
  if (g_fixQuadTextureSize) {
    const D3DVIEWPORT9& vp = device_state::CurrentViewport();
    UINT texW = 0, texH = 0;
    if (textureSizeX == vp.Width && textureSizeY == vp.Height && targetSizeX == vp.Width &&
        targetSizeY == vp.Height && device_state::Stage0TextureSize(texW, texH) &&
        (vp.X > 0 || vp.Width < texW) && vp.X + vp.Width <= texW && vp.Y + vp.Height <= texH) {
      if (g_quadFixes++ == 0) {
        VR_LOG_INFO("quad texture-size fix active: U/V %+.0f,%+.0f, texture %ux%u -> %ux%u (viewport %lu,%lu %lux%lu, "
                    "caller %p)",
                    static_cast<float>(vp.X), static_cast<float>(vp.Y), textureSizeX, textureSizeY, texW, texH, vp.X,
                    vp.Y, vp.Width, vp.Height, __builtin_return_address(0));
      }
      u += static_cast<float>(vp.X);
      v += static_cast<float>(vp.Y);
      textureSizeX = texW;
      textureSizeY = texH;
    }
  }
  g_origDrawDenormalizedQuad(x, y, sizeX, sizeY, u, v, sizeU, sizeV, targetSizeX, targetSizeY, textureSizeX,
                             textureSizeY, z);
}

}  // namespace

void Configure(const vrcore::Config& config) {
  g_fixQuadTextureSize = config.GetBool("Stereo", "FixQuadTextureSize", true);
  g_logCalls = config.GetInt("Debug", "DepthDownsampleLog", 0);
  g_remaining = g_logCalls;
}

bool DiagnosticEnabled() { return g_logCalls > 0; }

void Install() {
  if (g_fixQuadTextureSize && !g_origDrawDenormalizedQuad) {
    auto* quad = vrcore::Exe().FromPreferredVA<uint8_t>(kDrawDenormalizedQuad);
    if (vrcore::mem::Matches(reinterpret_cast<uintptr_t>(quad), kDrawDenormalizedQuadPrologueA,
                             sizeof(kDrawDenormalizedQuadPrologueA)) &&
        vrcore::mem::Matches(reinterpret_cast<uintptr_t>(quad) + kDrawDenormalizedQuadPrologueBOffset,
                             kDrawDenormalizedQuadPrologueB, sizeof(kDrawDenormalizedQuadPrologueB))) {
      const bool ok = vrcore::hook::Install("DrawDenormalizedQuad", reinterpret_cast<void*>(quad),
                                            reinterpret_cast<void*>(&HookDrawDenormalizedQuad),
                                            reinterpret_cast<void**>(&g_origDrawDenormalizedQuad));
      VR_LOG_INFO("quad texture-size fix %s", ok ? "installed" : "FAILED");
    } else {
      VR_LOG_ERROR("renderer hooks: DrawDenormalizedQuad prologue doesn't match this build");
    }
  }
  if (g_origDownsample || (g_logCalls <= 0 && !sensen_stereo::Enabled())) return;
  auto* fn = vrcore::Exe().FromPreferredVA<uint8_t>(kDownsampleSceneDepth);
  if (!vrcore::mem::Matches(reinterpret_cast<uintptr_t>(fn), kDownsampleSceneDepthPrologueA,
                            sizeof(kDownsampleSceneDepthPrologueA)) ||
      !vrcore::mem::Matches(reinterpret_cast<uintptr_t>(fn) + kDownsampleSceneDepthPrologueBOffset,
                            kDownsampleSceneDepthPrologueB, sizeof(kDownsampleSceneDepthPrologueB))) {
    VR_LOG_ERROR("renderer hooks: DownsampleSceneDepth prologue doesn't match this build");
    return;
  }
  const bool ok = vrcore::hook::Install("DownsampleSceneDepth", reinterpret_cast<void*>(fn),
                                        reinterpret_cast<void*>(&HookDownsample),
                                        reinterpret_cast<void**>(&g_origDownsample));
  VR_LOG_INFO("renderer hooks %s (depth downsample diagnostic: %d stereo call(s), re-armed by the side-by-side "
              "toggle)",
              ok ? "installed" : "FAILED", g_logCalls);
}

void Rearm() { g_remaining = g_logCalls; }

}  // namespace rm::renderer_hooks
