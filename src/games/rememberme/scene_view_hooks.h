#pragma once

#include "vrcore/compositor.h"
#include <cstdint>
#include <string>

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

  // A player-controller bool (word offset and bit, from reflection) during
  // which frames render mono, so VR shows the flat panel: loading screens
  // (bOnLoadingScreen: the tips screen as on a monitor instead of the
  // half-streamed level) and menus (bMenuPause: the pause/Sensen menus' 3D
  // backdrop is made for the game's FOV). `name` is for the log.
  // `menu`: governed by FlatMenus, else by FlatLoadingScreens.
  void AddFlatFlag(const char* name, int32_t offset, uint32_t mask, bool menu);
  bool FlatLoadingScreens() const;
  bool FlatMenus() const;

  // Live settings for the config menu (any thread).
  void SetFlatLoadingScreens(bool on);
  void SetFlatMenus(bool on);
  float WorldScale() const;  // Unreal units per metre
  void SetWorldScale(float unitsPerMeter);
  bool FullOrientation() const;  // [VR] CameraOrientation=full
  void SetFullOrientation(bool full);
  bool SymmetricProjection() const;
  void SetSymmetricProjection(bool on);
  bool SideBySide() const;
  // Writes the settings above to the INI.
  void Save(const vrcore::Config& config) const;
  int ToggleKey() const { return toggleKey_; }

 private:
  int toggleKey_ = 0x79;
};

}  // namespace rm
