#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include <d3d9.h>

#include "vrcore/vk.h"
#include "vrcore/xr.h"

namespace vrcore {

// Per-eye pose (in the compositor's LOCAL space) and field of view used to
// render one frame. Index 0 = left eye, 1 = right eye.
struct StereoFrame {
  XrTime displayTime = 0;
  XrPosef pose[2]{};
  XrFovf fov[2]{};
};

// Submits the game's D3D9 backbuffer to OpenXR (requires DXVK). Two ways:
//  - Stereo: the game renders the left eye into the left half of the
//    backbuffer and the right eye into the right half, using the poses/FOVs
//    from BeginStereoFrame (game thread). They're submitted as a projection
//    layer with exactly those poses.
//  - Screen: anything else (menus without stereo, loading screens, videos, or
//    Mode=screen) is shown on a flat world-locked panel (quad layer).
//
// Threading: OnPresent/OnDeviceChanged run on the presenting thread (UE3:
// render thread); BeginStereoFrame/SubmitStereoFrame on the thread that sets
// up the views (UE3: game thread).
class Compositor {
 public:
  enum class Mode { Stereo, Screen };
  struct Settings {
    Mode mode = Mode::Stereo;
    float screenDistance = 2.5f;  // metres in front of the recentred head
    float screenWidth = 3.0f;     // metres; height follows the backbuffer aspect
    int recenterKey = 0x77;       // virtual-key code (default F8); 0 = none
    const char* appName = "vrcore";
  };

  Compositor() = default;
  Compositor(const Compositor&) = delete;
  Compositor& operator=(const Compositor&) = delete;

  void Configure(const Settings& settings) { settings_ = settings; }

  // Presenting thread, before the original Present. Blocks in xrWaitFrame,
  // which paces the game to the headset while the session is running.
  void OnPresent(IDirect3DDevice9* device);
  // Device reset/recreation: drops the swapchain (sizes may change).
  void OnDeviceChanged();
  // Tears down OpenXR + Vulkan objects. Only while the D3D9 device is alive;
  // there is intentionally no destructor cleanup (see xr::Context).
  void Shutdown();

  // Game thread: eye poses/FOVs predicted for the frame about to be rendered.
  // False when stereo isn't available (render mono then).
  bool BeginStereoFrame(StereoFrame& out);
  // Game thread: the frame was rendered with `frame`; the next Present submits it.
  void SubmitStereoFrame(const StereoFrame& frame);

  // Recommended per-eye resolution (0 until the session exists).
  uint32_t EyeWidth() const { return eyeWidth_; }
  uint32_t EyeHeight() const { return eyeHeight_; }

 private:
  bool EnsureInit(const DxvkInterop& dxvk);
  bool EnsureSwapchain(uint32_t width, uint32_t height);
  void DestroySwapchain();
  void DestroyVulkan();
  bool CopyToSwapchain(const DxvkInterop& dxvk, IDirect3DSurface9* backbuffer, VkImage target, uint32_t width,
                       uint32_t height);
  bool PopStereoFrame(StereoFrame& out);

  Settings settings_;
  xr::Context xr_;
  vk::Functions vk_{};
  IDirect3DDevice9* device_ = nullptr;  // identity only, not ref-counted
  VkDevice vkDevice_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;

  XrSwapchain swapchain_ = XR_NULL_HANDLE;
  std::vector<VkImage> swapchainImages_;
  int64_t swapchainFormat_ = 0;
  bool rawCopy_ = true;
  uint32_t width_ = 0;
  uint32_t height_ = 0;

  // Game thread -> presenting thread.
  std::atomic<bool> stereoReady_{false};
  std::atomic<int64_t> lastDisplayTime_{0};
  std::atomic<int64_t> displayPeriod_{0};
  std::atomic<uint32_t> eyeWidth_{0};
  std::atomic<uint32_t> eyeHeight_{0};
  std::mutex queueMutex_;
  std::deque<StereoFrame> queue_;

  uint64_t nextInitAttemptMs_ = 0;
  uint64_t stereoFramesSubmitted_ = 0;
  uint64_t screenFramesSubmitted_ = 0;
  bool recenterKeyDown_ = false;
  bool recenterPending_ = true;
};

}  // namespace vrcore
