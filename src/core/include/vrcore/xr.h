#pragma once

// OpenXR with the Vulkan graphics binding. Include through this header so the
// platform headers come first (openxr_platform.h needs windows.h/unknwn.h and
// vulkan.h; XR_USE_* and XR_NO_PROTOTYPES are set by the openxr_headers target).

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>
#include <unknwn.h>
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "vrcore/dxvk_interop.h"

namespace vrcore::xr {

const char* ResultName(XrResult result);

// Finds the active OpenXR runtime (XR_RUNTIME_JSON env var, else the registry
// key HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime in this process's registry
// view), loads it, and negotiates the loader<->runtime interface. Returns the
// runtime's xrGetInstanceProcAddr, or nullptr (logged). No API layers.
PFN_xrGetInstanceProcAddr LoadRuntime();

// Owns an XrInstance + XrSession bound to DXVK's Vulkan device, a LOCAL
// reference space, and session-state handling. All calls must come from one
// thread (UE3: the render thread, inside the D3D9 Present hook).
class Context {
 public:
  Context() = default;
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  // Deliberately no cleanup: at process exit (static destruction) the D3D9 /
  // DXVK device is usually gone already, and destroying the session then makes
  // the runtime submit to a dead VkQueue and crash. Call Shutdown() explicitly
  // while the device is known to be alive.
  ~Context() = default;

  // Creates instance + session. False on failure (logged); safe to retry later.
  bool Init(const char* appName, const DxvkInterop& dxvk);
  void Shutdown();
  bool Initialized() const { return session_ != XR_NULL_HANDLE; }

  // Drains the event queue and drives xrBeginSession/xrEndSession.
  // Returns false if the session was lost or the runtime is exiting; the caller
  // should Shutdown() and retry Init later.
  bool PollEvents();
  bool Running() const { return running_; }

  XrInstance Instance() const { return instance_; }
  XrSession Session() const { return session_; }
  XrSpace LocalSpace() const { return localSpace_; }
  const std::vector<int64_t>& SwapchainFormats() const { return formats_; }

  // Recenters the LOCAL space on the head pose at `time` (yaw + position only).
  void Recenter(XrTime time);

  // Per-eye poses (in LocalSpace) and FOVs for the primary stereo view
  // configuration at `time`. Thread-safe against Recenter/Shutdown, so it may be
  // called from another thread than the frame loop (e.g. the game thread).
  bool LocateEyes(XrTime time, XrView out[2]);

  // Recommended per-eye image size reported by the runtime.
  uint32_t RecommendedEyeWidth() const { return eyeWidth_; }
  uint32_t RecommendedEyeHeight() const { return eyeHeight_; }

  template <typename Fn>
  Fn Proc(const char* name) const {
    PFN_xrVoidFunction fn = nullptr;
    if (getProcAddr_) getProcAddr_(instance_, name, &fn);
    return reinterpret_cast<Fn>(fn);
  }

  // Core functions used by vrcore (loaded once the instance exists).
  struct Fns {
    PFN_xrDestroyInstance DestroyInstance;
    PFN_xrPollEvent PollEvent;
    PFN_xrBeginSession BeginSession;
    PFN_xrEndSession EndSession;
    PFN_xrDestroySession DestroySession;
    PFN_xrCreateReferenceSpace CreateReferenceSpace;
    PFN_xrDestroySpace DestroySpace;
    PFN_xrLocateSpace LocateSpace;
    PFN_xrWaitFrame WaitFrame;
    PFN_xrBeginFrame BeginFrame;
    PFN_xrEndFrame EndFrame;
    PFN_xrCreateSwapchain CreateSwapchain;
    PFN_xrDestroySwapchain DestroySwapchain;
    PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages;
    PFN_xrAcquireSwapchainImage AcquireSwapchainImage;
    PFN_xrWaitSwapchainImage WaitSwapchainImage;
    PFN_xrReleaseSwapchainImage ReleaseSwapchainImage;
    PFN_xrEnumerateSwapchainFormats EnumerateSwapchainFormats;
    PFN_xrLocateViews LocateViews;
    PFN_xrEnumerateViewConfigurationViews EnumerateViewConfigurationViews;
  } fn{};

 private:
  bool CreateInstance(const char* appName);
  bool CreateSession(const DxvkInterop& dxvk);
  bool CreateLocalSpace(const XrPosef& pose);

  PFN_xrGetInstanceProcAddr getProcAddr_ = nullptr;
  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId system_ = XR_NULL_SYSTEM_ID;
  XrSession session_ = XR_NULL_HANDLE;
  XrSpace localSpace_ = XR_NULL_HANDLE;
  XrSpace viewSpace_ = XR_NULL_HANDLE;
  XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
  std::atomic<bool> running_{false};
  std::vector<int64_t> formats_;
  uint32_t eyeWidth_ = 0;
  uint32_t eyeHeight_ = 0;
  // Guards session/space lifetime between the frame-loop thread and LocateEyes callers.
  mutable std::mutex mutex_;
};

}  // namespace vrcore::xr
