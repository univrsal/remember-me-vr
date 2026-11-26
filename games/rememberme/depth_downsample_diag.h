#pragma once

#include <d3d9.h>

#include "vrcore/config.h"

namespace rm::depth_downsample_diag {

// Diagnostic for the right-eye banding (docs/09, artifact A): logs what UE3's
// DownsampleSceneDepth (0x00A60D30) sees and sends to D3D9 for each view, i.e.
// per eye: the view's render-target rect, projection matrix and the flag that
// picks the scene-colour sampler, then every pixel-shader constant, sampler
// state, texture and viewport the pass sets. Off unless [Debug]
// DepthDownsampleLog > 0; logs that many two-view (stereo) calls, re-armed by Rearm().
void Configure(const vrcore::Config& config);

// Hooks the engine function (known build only) and the device methods. Call
// from Game::OnDeviceCreated.
void Install(IDirect3DDevice9* device);

// Log the next N stereo calls again (e.g. when the side-by-side test is toggled).
void Rearm();

}  // namespace rm::depth_downsample_diag
