#pragma once

#include "vrcore/config.h"

namespace rm::renderer_hooks {

// Render-thread engine hooks on the scene renderer.
//
// DownsampleSceneDepth (0x00A60D30) runs once per scene render, before the
// translucency, post-process and Sensen passes, with the renderer's views. It
// feeds the per-frame views to sensen_stereo and hosts the per-eye
// diagnostic: with [Debug] DepthDownsampleLog = N > 0 it logs, for the next N
// two-view calls, each view's rect, projection and scene-colour sampler flag,
// then (via device_state tracing) every shader constant, sampler state,
// texture and viewport the pass sets (docs/09). Rearm() logs N more.
void Configure(const vrcore::Config& config);
bool DiagnosticEnabled();

// Known build only; device_state must be installed for the tracing.
void Install();

void Rearm();

// Render thread: this frame's view rects in the scene buffer (X, Y, SizeX,
// SizeY), set at DownsampleSceneDepth when the renderer has two views (a
// stereo frame). False otherwise, and after EndFrame().
struct ViewRect {
  int x, y, w, h;
};
bool StereoViewRects(ViewRect& left, ViewRect& right);
void EndFrame();

}  // namespace rm::renderer_hooks
