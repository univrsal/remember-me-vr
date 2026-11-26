#pragma once

#include <atomic>

#include "vrcore/config.h"

namespace rm {

// Hooks ULocalPlayer::CalcSceneView and the GetPlayerViewPoint event thunk.
//  - Logs CalcSceneView calls (arguments, view count) to confirm the signature.
//  - Side-by-side test: when enabled, each CalcSceneView becomes two calls, one
//    per half of the player's viewport, with the camera shifted left/right.
//    Both views land in the same FSceneViewFamily (strategy S2, docs/08).
// All hooked code runs on UE3's game thread.
class SceneViewHooks {
 public:
  void Configure(const vrcore::Config& config);

  // Installs the hooks after verifying the target prologues. Call once the
  // exe is unpacked (Game::OnDeviceCreated).
  bool Install();

  // Thread-safe toggle (called from the render-thread key poll).
  void ToggleSideBySide();
  int ToggleKey() const { return toggleKey_; }

 private:
  int toggleKey_ = 0x79;
};

}  // namespace rm
