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

int g_logCalls = 0;               // from the config; 0 = off
std::atomic<int> g_remaining{0};  // stereo calls still to log
uint64_t g_logged = 0;
bool g_stereoFrame = false;  // render thread
ViewRect g_rects[2]{};            // calls logged so far (numbering in the log)

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
  g_stereoFrame = views == 2;
  if (g_stereoFrame) {
    const uintptr_t first = At<uintptr_t>(base, kRendererViewsData);
    for (int i = 0; i < 2; ++i) {
      const uintptr_t view = first + static_cast<uintptr_t>(i) * kViewInfoStride;
      g_rects[i] = {At<int32_t>(view, kViewRenderTargetX), At<int32_t>(view, kViewRenderTargetX + 4),
                    At<int32_t>(view, kViewRenderTargetX + 8), At<int32_t>(view, kViewRenderTargetX + 12)};
    }
  }
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

}  // namespace

void Configure(const vrcore::Config& config) {
  g_logCalls = config.GetInt("Debug", "DepthDownsampleLog", 0);
  g_remaining = g_logCalls;
}

bool DiagnosticEnabled() { return g_logCalls > 0; }

void Install() {
  if (g_origDownsample) return;
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

bool StereoViewRects(ViewRect& left, ViewRect& right) {
  if (!g_stereoFrame) return false;
  left = g_rects[0];
  right = g_rects[1];
  return true;
}

void EndFrame() { g_stereoFrame = false; }

}  // namespace rm::renderer_hooks
