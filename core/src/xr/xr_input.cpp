#include "vrcore/xr_input.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "vrcore/log.h"

namespace vrcore::xr {
namespace {

XrPath ToPath(Context& xr, const char* text) {
  static PFN_xrStringToPath stringToPath = nullptr;
  if (!stringToPath) stringToPath = xr.Proc<PFN_xrStringToPath>("xrStringToPath");
  XrPath path = XR_NULL_PATH;
  if (stringToPath) stringToPath(xr.Instance(), text, &path);
  return path;
}

}  // namespace

bool Input::Init(Context& xr) {
  if (initialized_) return true;
  auto createActionSet = xr.Proc<PFN_xrCreateActionSet>("xrCreateActionSet");
  auto createAction = xr.Proc<PFN_xrCreateAction>("xrCreateAction");
  auto suggest = xr.Proc<PFN_xrSuggestInteractionProfileBindings>("xrSuggestInteractionProfileBindings");
  auto attach = xr.Proc<PFN_xrAttachSessionActionSets>("xrAttachSessionActionSets");
  syncActions_ = xr.Proc<PFN_xrSyncActions>("xrSyncActions");
  getBool_ = xr.Proc<PFN_xrGetActionStateBoolean>("xrGetActionStateBoolean");
  getFloat_ = xr.Proc<PFN_xrGetActionStateFloat>("xrGetActionStateFloat");
  getVec2_ = xr.Proc<PFN_xrGetActionStateVector2f>("xrGetActionStateVector2f");
  applyHaptic_ = xr.Proc<PFN_xrApplyHapticFeedback>("xrApplyHapticFeedback");
  stopHaptic_ = xr.Proc<PFN_xrStopHapticFeedback>("xrStopHapticFeedback");
  if (!createActionSet || !createAction || !suggest || !attach || !syncActions_ || !getBool_ || !getFloat_ ||
      !getVec2_) {
    VR_LOG_ERROR("xr input: action functions missing");
    return false;
  }

  XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strcpy(setInfo.actionSetName, "gameplay");
  std::strcpy(setInfo.localizedActionSetName, "Gameplay");
  XrResult result = createActionSet(xr.Instance(), &setInfo, &set_);
  if (XR_FAILED(result)) {
    VR_LOG_ERROR("xr input: xrCreateActionSet failed: %s", ResultName(result));
    return false;
  }
  hands_[0] = ToPath(xr, "/user/hand/left");
  hands_[1] = ToPath(xr, "/user/hand/right");

  auto make = [&](const char* name, const char* label, XrActionType type, XrAction& out) {
    XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
    std::strcpy(info.actionName, name);
    std::strcpy(info.localizedActionName, label);
    info.actionType = type;
    info.countSubactionPaths = 2;
    info.subactionPaths = hands_;
    const XrResult r = createAction(set_, &info, &out);
    if (XR_FAILED(r)) VR_LOG_ERROR("xr input: xrCreateAction(%s) failed: %s", name, ResultName(r));
    return XR_SUCCEEDED(r);
  };
  bool ok = make("stick", "Stick", XR_ACTION_TYPE_VECTOR2F_INPUT, stick_) &&
            make("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, trigger_) &&
            make("grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, grip_) &&
            make("primary", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT, primary_) &&
            make("secondary", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT, secondary_) &&
            make("stick_click", "Stick click", XR_ACTION_TYPE_BOOLEAN_INPUT, stickClick_) &&
            make("pad", "Trackpad", XR_ACTION_TYPE_VECTOR2F_INPUT, pad_) &&
            make("pad_force", "Trackpad press", XR_ACTION_TYPE_FLOAT_INPUT, padForce_) &&
            make("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, menu_) &&
            make("aim", "Aim", XR_ACTION_TYPE_POSE_INPUT, aim_) &&
            make("haptic", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT, haptic_);
  if (!ok) return false;

  struct Binding {
    XrAction action;
    const char* path;
  };
  auto suggestProfile = [&](const char* profile, const std::vector<Binding>& bindings) {
    std::vector<XrActionSuggestedBinding> list;
    for (const auto& b : bindings) list.push_back({b.action, ToPath(xr, b.path)});
    XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    info.interactionProfile = ToPath(xr, profile);
    info.countSuggestedBindings = static_cast<uint32_t>(list.size());
    info.suggestedBindings = list.data();
    const XrResult r = suggest(xr.Instance(), &info);
    if (XR_FAILED(r)) VR_LOG_WARN("xr input: bindings for %s rejected: %s", profile, ResultName(r));
  };
  suggestProfile("/interaction_profiles/valve/index_controller",
                 {{stick_, "/user/hand/left/input/thumbstick"},
                  {stick_, "/user/hand/right/input/thumbstick"},
                  {trigger_, "/user/hand/left/input/trigger/value"},
                  {trigger_, "/user/hand/right/input/trigger/value"},
                  // squeeze/value is finger curl (~1 just holding the
                  // controller, i.e. LB/RB held all the time); force is
                  // the actual squeeze.
                  {grip_, "/user/hand/left/input/squeeze/force"},
                  {grip_, "/user/hand/right/input/squeeze/force"},
                  {primary_, "/user/hand/left/input/a/click"},
                  {primary_, "/user/hand/right/input/a/click"},
                  {secondary_, "/user/hand/left/input/b/click"},
                  {secondary_, "/user/hand/right/input/b/click"},
                  {stickClick_, "/user/hand/left/input/thumbstick/click"},
                  {stickClick_, "/user/hand/right/input/thumbstick/click"},
                  {pad_, "/user/hand/left/input/trackpad"},
                  {pad_, "/user/hand/right/input/trackpad"},
                  {padForce_, "/user/hand/left/input/trackpad/force"},
                  {padForce_, "/user/hand/right/input/trackpad/force"},
                  {aim_, "/user/hand/left/input/aim/pose"},
                  {aim_, "/user/hand/right/input/aim/pose"},
                  {haptic_, "/user/hand/left/output/haptic"},
                  {haptic_, "/user/hand/right/output/haptic"}});
  suggestProfile("/interaction_profiles/oculus/touch_controller",
                 {{stick_, "/user/hand/left/input/thumbstick"},
                  {stick_, "/user/hand/right/input/thumbstick"},
                  {trigger_, "/user/hand/left/input/trigger/value"},
                  {trigger_, "/user/hand/right/input/trigger/value"},
                  {grip_, "/user/hand/left/input/squeeze/value"},
                  {grip_, "/user/hand/right/input/squeeze/value"},
                  {primary_, "/user/hand/left/input/x/click"},
                  {primary_, "/user/hand/right/input/a/click"},
                  {secondary_, "/user/hand/left/input/y/click"},
                  {secondary_, "/user/hand/right/input/b/click"},
                  {stickClick_, "/user/hand/left/input/thumbstick/click"},
                  {stickClick_, "/user/hand/right/input/thumbstick/click"},
                  {menu_, "/user/hand/left/input/menu/click"},
                  {aim_, "/user/hand/left/input/aim/pose"},
                  {aim_, "/user/hand/right/input/aim/pose"},
                  {haptic_, "/user/hand/left/output/haptic"},
                  {haptic_, "/user/hand/right/output/haptic"}});
  suggestProfile("/interaction_profiles/khr/simple_controller",
                 {{primary_, "/user/hand/right/input/select/click"},
                  {menu_, "/user/hand/left/input/menu/click"},
                  {aim_, "/user/hand/left/input/aim/pose"},
                  {aim_, "/user/hand/right/input/aim/pose"},
                  {haptic_, "/user/hand/left/output/haptic"},
                  {haptic_, "/user/hand/right/output/haptic"}});

  XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  attachInfo.countActionSets = 1;
  attachInfo.actionSets = &set_;
  result = attach(xr.Session(), &attachInfo);
  if (XR_FAILED(result)) {
    VR_LOG_ERROR("xr input: xrAttachSessionActionSets failed: %s", ResultName(result));
    return false;
  }
  // Aim poses (laser pointer): one action space per hand.
  auto createActionSpace = xr.Proc<PFN_xrCreateActionSpace>("xrCreateActionSpace");
  for (int h = 0; h < 2 && createActionSpace; ++h) {
    XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.action = aim_;
    spaceInfo.subactionPath = hands_[h];
    spaceInfo.poseInActionSpace.orientation.w = 1.0f;
    const XrResult r = createActionSpace(xr.Session(), &spaceInfo, &aimSpace_[h]);
    if (XR_FAILED(r)) VR_LOG_WARN("xr input: aim space for hand %d failed: %s", h, ResultName(r));
  }
  initialized_ = true;
  VR_LOG_INFO("xr input: actions attached (Index, Touch, simple controller bindings)");
  return true;
}

void Input::Reset() {
  initialized_ = false;
  set_ = XR_NULL_HANDLE;
  stick_ = trigger_ = grip_ = primary_ = secondary_ = stickClick_ = pad_ = padForce_ = menu_ = haptic_ = aim_ =
      XR_NULL_HANDLE;
  aimSpace_[0] = aimSpace_[1] = XR_NULL_HANDLE;  // die with the session
  vibrating_[0] = vibrating_[1] = 0.0f;
  std::lock_guard<std::mutex> lock(mutex_);
  state_ = ControllerState{};
}

void Input::Sync(Context& xr, XrTime displayTime) {
  if (!initialized_) return;
  XrActiveActionSet active{set_, XR_NULL_PATH};
  XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
  syncInfo.countActiveActionSets = 1;
  syncInfo.activeActionSets = &active;
  const XrResult result = syncActions_(xr.Session(), &syncInfo);
  ControllerState s;
  if (result == XR_SUCCESS) {  // XR_SESSION_NOT_FOCUSED etc.: no input
    bool any = false;
    for (int h = 0; h < 2; ++h) {
      XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
      get.subactionPath = hands_[h];
      XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
      XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT};
      XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN};
      get.action = stick_;
      if (XR_SUCCEEDED(getVec2_(xr.Session(), &get, &v)) && v.isActive) {
        s.stick[h][0] = v.currentState.x;
        s.stick[h][1] = v.currentState.y;
        any = true;
      }
      get.action = trigger_;
      if (XR_SUCCEEDED(getFloat_(xr.Session(), &get, &f)) && f.isActive) {
        s.trigger[h] = f.currentState;
        any = true;
      }
      get.action = grip_;
      if (XR_SUCCEEDED(getFloat_(xr.Session(), &get, &f)) && f.isActive) s.grip[h] = f.currentState;
      get.action = primary_;
      if (XR_SUCCEEDED(getBool_(xr.Session(), &get, &b)) && b.isActive) s.primary[h] = b.currentState;
      get.action = secondary_;
      if (XR_SUCCEEDED(getBool_(xr.Session(), &get, &b)) && b.isActive) s.secondary[h] = b.currentState;
      get.action = stickClick_;
      if (XR_SUCCEEDED(getBool_(xr.Session(), &get, &b)) && b.isActive) s.stickClick[h] = b.currentState;
      get.action = pad_;
      if (XR_SUCCEEDED(getVec2_(xr.Session(), &get, &v)) && v.isActive) {
        s.pad[h][0] = v.currentState.x;
        s.pad[h][1] = v.currentState.y;
      }
      get.action = padForce_;
      if (XR_SUCCEEDED(getFloat_(xr.Session(), &get, &f)) && f.isActive) s.padPress[h] = f.currentState > 0.5f;
      get.action = menu_;
      if (XR_SUCCEEDED(getBool_(xr.Session(), &get, &b)) && b.isActive) s.menu[h] = b.currentState;
      if (aimSpace_[h] != XR_NULL_HANDLE && xr.fn.LocateSpace) {
        XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
        constexpr XrSpaceLocationFlags kTracked =
            XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_SUCCEEDED(xr.fn.LocateSpace(aimSpace_[h], xr.LocalSpace(), displayTime, &location)) &&
            (location.locationFlags & kTracked) == kTracked) {
          s.aimValid[h] = true;
          s.aim[h] = location.pose;
        }
      }
    }
    s.active = any;
    if (any != loggedActive_) {
      loggedActive_ = any;
      VR_LOG_INFO("xr input: controllers %s", any ? "active" : "inactive");
    }

    // Haptics: (re)start or stop when the requested amplitude changes.
    if (applyHaptic_ && stopHaptic_) {
      for (int h = 0; h < 2; ++h) {
        const float want = vibrate_[h].load(std::memory_order_relaxed);
        const uint64_t now = GetTickCount64();
        const bool changed = std::fabs(want - vibrating_[h]) >= 0.02f;
        const bool renew = want > 0.0f && now >= vibrateRenewMs_[h];  // pulses last 0.5 s
        if (!changed && !renew) continue;
        vibrating_[h] = want;
        vibrateRenewMs_[h] = now + 400;
        XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = haptic_;
        info.subactionPath = hands_[h];
        if (want <= 0.0f) {
          stopHaptic_(xr.Session(), &info);
        } else {
          XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
          vibration.amplitude = want;
          vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
          vibration.duration = 500000000;  // 0.5 s; renewed while the game keeps rumbling
          applyHaptic_(xr.Session(), &info, reinterpret_cast<XrHapticBaseHeader*>(&vibration));
        }
      }
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  s.sequence = state_.sequence + 1;
  state_ = s;
}

ControllerState Input::State() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

void Input::Vibrate(float left, float right) {
  vibrate_[0].store(left, std::memory_order_relaxed);
  vibrate_[1].store(right, std::memory_order_relaxed);
}

}  // namespace vrcore::xr
