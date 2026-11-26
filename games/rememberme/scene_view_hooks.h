#pragma once

#include "vrcore/compositor.h"
#include <cstdint>

#include "vrcore/config.h"

namespace rm {

// Engine hooks that turn UE3's single player view into a stereo pair:
//  - ULocalPlayer::CalcSceneView is called once per eye, with the eye's half of
//    the viewport, its own FSceneViewState, its camera pose and its projection.
//    Both views land in the same FSceneViewFamily (strategy S2, docs/08).
//  - eventGetPlayerViewPoint supplies the per-eye camera: the game's camera
//    (yaw only by default) combined with the tracked eye pose from OpenXR.
//  - FPerspectiveMatrix is replaced by the eye's asymmetric OpenXR frustum.
// Without a running OpenXR session the game renders normally (mono). F10 still
// toggles the flat-screen side-by-side test from M2.
// All hooked code runs on UE3's game thread.
class SceneViewHooks {
 public:
  void Configure(const vrcore::Config& config, vrcore::Compositor* compositor);

  // Installs the hooks after verifying the target prologues. Call once the
  // exe is unpacked (Game::OnDeviceCreated).
  bool Install();

  // Thread-safe toggle (called from the render-thread key poll).
  void ToggleSideBySide();

  // Where the player controller's "on loading screen" bool lives
  // (ExamplePlayerController.bOnLoadingScreen, from reflection). While it's
  // set, frames render mono, so VR shows the flat panel (the tips screen as on
  // a monitor) instead of the half-streamed level around the player.
  void SetLoadingScreenFlag(int32_t offset, uint32_t mask);
  int ToggleKey() const { return toggleKey_; }

 private:
  int toggleKey_ = 0x79;
};

}  // namespace rm
