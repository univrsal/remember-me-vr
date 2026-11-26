#pragma once

#include <d3d9.h>

namespace rm::device_state {

// D3D9 state hooks shared by the Remember Me fixes and diagnostics (scene_tap
// owns the draw and render-target hooks; these are the others). Installed once.
//
// DNEResolution fix: DONTNOD's material parameter DNEResolution is
// (2/SizeX, -2/SizeY, -1, 1), and shaders turn the pixel position into clip
// space with ndc = (vPos - 0.5) * xy + zw. That ignores the view's offset in
// the render target, so for the right eye (X = SizeX) ndc.x runs 1..3 and the
// scene-depth lookup clamps to the last column: horizontal bands in depth-faded
// effects (docs/09, artifact A). When a constant uploaded to either stage equals
// that value for the current viewport and the viewport doesn't start at 0,0,
// its z/w are rewritten to -1 - X*2/SizeX and 1 + Y*2/SizeY. Mono and left-eye
// draws are unchanged.
struct Options {
  bool fixDneResolution = true;
};

bool Install(IDirect3DDevice9* device, const Options& options);

// Logs every viewport, texture, sampler state, pixel shader and pixel/vertex
// shader constant set on the calling thread while enabled (render thread).
void SetTrace(bool enabled);

}  // namespace rm::device_state
