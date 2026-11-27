#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include <d3d9.h>

#include "vrcore/vk.h"
#include "vrcore/xr.h"
#include "vrcore/xr_input.h"

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
    float uiDistance = 1.6f;      // UI panel in stereo frames: metres in front of the recentred head
    float uiWidth = 1.3f;         // metres; height follows the UI texture aspect
    float uiHeight = 0.0f;        // vertical offset in metres (negative = lower)
    float menuDistance = 1.2f;    // overlay menu: metres in front of the head when it opens
    float menuWidth = 1.0f;       // metres; height follows the menu texture aspect
    float menuHeight = -0.1f;     // vertical offset from eye height in metres
    int recenterKey = 0x77;       // virtual-key code (default F8); 0 = none
    const char* appName = "vrcore";
  };

  Compositor() = default;
  Compositor(const Compositor&) = delete;
  Compositor& operator=(const Compositor&) = delete;

  void Configure(const Settings& settings) { settings_ = settings; }
  // Presenting thread (e.g. an in-game menu drawn at Present): live changes.
  Settings& MutableSettings() { return settings_; }

  // Presenting thread, before the original Present. Blocks in xrWaitFrame,
  // which paces the game to the headset while the session is running.
  // `sceneTexture` (optional, see scene_tap.h) is the game's final scene image
  // before UI, at its internal resolution: stereo frames are taken from it
  // (resolution independent of the window) when its size/format fit,
  // otherwise from the backbuffer.
  // `uiTexture` (optional, premultiplied alpha) is shown on a floating panel
  // over the stereo scene. `menuTexture` (optional, premultiplied alpha) is an
  // overlay menu on its own panel, placed in front of the head when it first
  // appears and kept there while it's passed every frame (stereo and screen).
  void OnPresent(IDirect3DDevice9* device, IDirect3DTexture9* sceneTexture = nullptr,
                 IDirect3DTexture9* uiTexture = nullptr, IDirect3DTexture9* menuTexture = nullptr);
  // Recenters on the next frame (any thread).
  void RequestRecenter() { recenterRequested_ = true; }
  // Presenting thread: the menu panel's pose (LOCAL space, +Z toward the
  // viewer) and size in metres, while it's shown.
  bool MenuPanel(XrPosef& pose, XrExtent2Df& size) const;
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

  // Controller input (any thread): latest synced state, and rumble requests.
  xr::ControllerState Controllers() const { return input_.State(); }
  void Vibrate(float left, float right) { input_.Vibrate(left, right); }

  // True while the OpenXR session runs in stereo mode (frames can be stereo).
  bool StereoActive() const { return stereoReady_.load(); }

  // Recommended per-eye resolution (0 until the session exists).
  uint32_t EyeWidth() const { return eyeWidth_; }
  uint32_t EyeHeight() const { return eyeHeight_; }

 private:
  struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<VkImage> images;
    uint32_t width = 0;
    uint32_t height = 0;
    bool rawCopy = true;  // vkCmdCopyImage (bit-exact) vs vkCmdBlitImage
    const char* name = "";
  };

  bool EnsureInit(const DxvkInterop& dxvk);
  bool EnsureSwapchain(Swapchain& swapchain, uint32_t width, uint32_t height);
  void DestroySwapchain(Swapchain& swapchain);
  void DestroySwapchains();
  void DestroyVulkan();
  // Copies (0,0)-(width,height) of `source` into the swapchain's next image.
  bool CopyToSwapchain(const DxvkInterop& dxvk, IDirect3DSurface9* source, bool sourceIsBgra8, Swapchain& swapchain,
                       uint32_t width, uint32_t height);
  bool PopStereoFrame(StereoFrame& out);
  void AnchorMenu(XrTime time);

  Settings settings_;
  xr::Context xr_;
  xr::Input input_;
  vk::Functions vk_{};
  IDirect3DDevice9* device_ = nullptr;  // identity only, not ref-counted
  VkDevice vkDevice_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  VkFence fence_ = VK_NULL_HANDLE;

  Swapchain stereoSwapchain_{.name = "stereo"};
  Swapchain screenSwapchain_{.name = "screen"};
  Swapchain uiSwapchain_{.name = "ui"};
  Swapchain menuSwapchain_{.name = "menu"};
  bool menuAnchored_ = false;  // menu panel pose chosen (cleared when the menu closes or on recenter)
  XrPosef menuPose_{};
  XrExtent2Df menuSize_{};
  std::atomic<bool> recenterRequested_{false};
  int sceneSourceLogged_ = -1;  // last logged stereo source (0 backbuffer, n = scene at n x)

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
