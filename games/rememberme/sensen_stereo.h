#pragma once

#include "vrcore/config.h"

namespace rm::sensen_stereo {

// Sensen AR hints in both eyes (docs/09, artifact D).
//
// The game draws the hints once per frame with Scaleform into an offscreen
// layer, in View[0]'s viewport, with a per-draw vertex constant `Transform` =
// (movie-to-world affine) x the *game camera's* translated view-projection
// (mono: game FOV, full-viewport aspect), not the eye's. A composite pass
// (pixel constant `DNESensenParam1`) then blends tiles of the layer over the
// scene where the hints are.
//
// In stereo frames every hint draw is drawn once per eye with that eye's
// viewport and Transform x C_eye, where
//   C_eye = inverse(TVP_game) x Translate(origin_game - origin_eye) x TVP_eye
// (TVP = rotation-only view x projection), and one composite quad is added
// over the right view (the composite discards pixels without hint content).
void Configure(const vrcore::Config& config);
bool Enabled();

// Registers the draw observer (needs device_state and scene_tap installed).
void Install();

// Game thread, per stereo frame: the game camera's view and projection
// (CalcSceneView in matrices-only mode), keyed by that frame's left-eye
// ViewMatrix so the render thread can match it to the frame it renders.
void PushGameCamera(const float leftEyeView[16], const float gameView[16], const float gameProj[16]);

// Render thread, once per scene render (DownsampleSceneDepth): the renderer's
// views. Two views = a stereo frame; anything else turns the redraw off.
void OnRendererViews(void* renderer);

// Render thread, at Present.
void EndFrame();

}  // namespace rm::sensen_stereo
