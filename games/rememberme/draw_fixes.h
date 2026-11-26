#pragma once

#include <d3d9.h>

#include "vrcore/config.h"
#include "vrcore/scene_tap.h"

namespace rm::draw_fixes {

// The game module's draw observer (vrcore::scene_tap allows one): dispatches
// to sensen_stereo and the fixes below.
//
// Viewport-relative UV fix ([Stereo] FixQuadTextureSize, docs/09 artifact G):
// some screen-space copies (e.g. the per-view scene-colour copy before
// translucency, whose alpha soft particles and fog read as depth; drawn as a
// batched-element tile) use UVs relative to the view (u = pixel / view width)
// while sampling the whole scene buffer. In mono the view is the buffer; for a
// stereo view they read the buffer from x = 0, stretched. A client-memory quad
// drawn with exactly the right view's viewport in a stereo frame, sampling a
// texture of the render target's size, whose UVs equal its position within the
// viewport, is redrawn with UVs mapped into the buffer. (Not the left view:
// it starts at x = 0, and corner viewports of pooled targets, e.g. downsample
// chains, legitimately use viewport-relative UVs.)
void Configure(const vrcore::Config& config);

// Registers the observer (needs device_state and scene_tap installed).
void Install();

}  // namespace rm::draw_fixes
