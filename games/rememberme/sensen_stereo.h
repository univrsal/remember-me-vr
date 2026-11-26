#pragma once

#include "vrcore/config.h"

namespace rm::sensen_stereo {

// Sensen AR hints in both eyes (docs/09, artifact D).
//
// The game draws the hints once per frame with Scaleform into an offscreen
// layer, in View[0]'s viewport, with a per-draw vertex constant `Transform` =
// (movie-to-world affine) x View[0]'s translated view-projection. A composite
// pass (pixel constant `DNESensenParam1`) then blends tiles of that layer over
// the scene, again only where View[0]'s hints are.
//
// In stereo frames this module draws every hint draw a second time with the
// right view's viewport and Transform x C, where
//   C = inverse(TVP_left) x Translate(origin_left - origin_right) x TVP_right
// (TVP = rotation-only view x projection, from the views' matrices), and adds
// one composite quad covering the right view (the composite discards pixels
// without hint content, so covering the whole view is safe).
void Configure(const vrcore::Config& config);
bool Enabled();

// Registers the draw observer (needs device_state and scene_tap installed).
void Install();

// Render thread, once per scene render (DownsampleSceneDepth): the renderer's
// views. Two views = a stereo frame; anything else turns duplication off.
void OnRendererViews(void* renderer);

// Render thread, at Present.
void EndFrame();

}  // namespace rm::sensen_stereo
