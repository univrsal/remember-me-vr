#pragma once

#include <d3d9.h>

#include "vrcore/config.h"
#include "vrcore/scene_tap.h"

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
// (TVP = rotation-only view x projection). The game's composite tiles (placed
// where the game camera sees the hints) are replaced by one quad per view (the
// composite discards pixels without hint content).
void Configure(const vrcore::Config& config);
bool Enabled();

// Enables the redraw (needs device_state and scene_tap installed; draws come
// through draw_fixes' observer).
void Install();

// Render thread, before each non-UI draw (from draw_fixes). True if the draw
// was handled (the game's draw is skipped).
bool OnDraw(IDirect3DDevice9* device, const vrcore::scene_tap::DrawCall& call);

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
