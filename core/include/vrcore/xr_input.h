#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "vrcore/xr.h"

namespace vrcore::xr {

// Controller state in gamepad-like terms (one hand per index: 0 left, 1 right).
struct ControllerState {
  bool active = false;  // session focused and at least one controller reporting
  float stick[2][2]{};  // thumbstick x, y (-1..1, y up)
  float trigger[2]{};   // 0..1
  float grip[2]{};      // 0..1
  bool primary[2]{};    // left: X (Index A), right: A
  bool secondary[2]{};  // left: Y (Index B), right: B
  bool stickClick[2]{};
  bool padPress[2]{};   // Index: trackpad pressed hard
  bool menu[2]{};       // Touch / simple controller: menu button (left)
  float pad[2][2]{};    // trackpad position (Index), for D-pad directions
  bool aimValid[2]{};   // aim pose tracked this sync
  XrPosef aim[2]{};     // aim pose (pointing ray: -Z) in the LOCAL space, at the frame's display time
  uint32_t sequence = 0;  // incremented on every sync
};

// OpenXR actions for the game's input: one action set with thumbsticks,
// triggers, grips, two face buttons, stick clicks and trackpads (Valve Index
// bindings, Oculus Touch and KHR simple as fallbacks), plus haptics.
// Init/Sync/Reset run on the frame-loop thread (UE3: the render thread);
// State/Vibrate are thread-safe.
class Input {
 public:
  // After the session exists (actions must be attached before syncing).
  bool Init(Context& xr);
  // Drops the handles (they die with the instance/session).
  void Reset();
  // After xrBeginFrame while the session runs: syncs actions, updates the
  // state (aim poses located at `displayTime`) and applies pending haptics.
  void Sync(Context& xr, XrTime displayTime);

  ControllerState State() const;
  // Rumble request, e.g. from XInputSetState (0..1 per hand); applied on Sync.
  void Vibrate(float left, float right);

 private:
  bool initialized_ = false;
  XrActionSet set_ = XR_NULL_HANDLE;
  XrPath hands_[2] = {XR_NULL_PATH, XR_NULL_PATH};
  XrAction stick_ = XR_NULL_HANDLE, trigger_ = XR_NULL_HANDLE, grip_ = XR_NULL_HANDLE;
  XrAction primary_ = XR_NULL_HANDLE, secondary_ = XR_NULL_HANDLE, stickClick_ = XR_NULL_HANDLE;
  XrAction pad_ = XR_NULL_HANDLE, padForce_ = XR_NULL_HANDLE, menu_ = XR_NULL_HANDLE, haptic_ = XR_NULL_HANDLE;
  XrAction aim_ = XR_NULL_HANDLE;
  XrSpace aimSpace_[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

  PFN_xrSyncActions syncActions_ = nullptr;
  PFN_xrGetActionStateBoolean getBool_ = nullptr;
  PFN_xrGetActionStateFloat getFloat_ = nullptr;
  PFN_xrGetActionStateVector2f getVec2_ = nullptr;
  PFN_xrApplyHapticFeedback applyHaptic_ = nullptr;
  PFN_xrStopHapticFeedback stopHaptic_ = nullptr;

  mutable std::mutex mutex_;
  ControllerState state_;
  std::atomic<float> vibrate_[2]{{0.0f}, {0.0f}};
  float vibrating_[2] = {0.0f, 0.0f};
  uint64_t vibrateRenewMs_[2] = {0, 0};
  bool loggedActive_ = false;
};

}  // namespace vrcore::xr
