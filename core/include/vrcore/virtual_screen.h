#pragma once

#include <cstdint>
#include <vector>

#include <d3d9.h>

#include "vrcore/vk.h"
#include "vrcore/xr.h"

namespace vrcore {

// Shows the game's D3D9 backbuffer on a flat panel in VR (an OpenXR quad
// layer). Game-independent; used as the first VR milestone and later as the
// fallback for menus/FMVs. Requires DXVK.
//
// Threading: OnPresent must be called from the thread that presents (UE3:
// render thread). OpenXR state is created lazily on that thread.
class VirtualScreen {
 public:
  struct Settings {
    float distance = 2.5f;     // metres in front of the recentred head
    float width = 3.0f;        // metres; height follows the backbuffer aspect
    int recenterKey = 0x77;    // virtual-key code (default F8); 0 = none
    const char* appName = "vrcore";
  };

  VirtualScreen() = default;
  VirtualScreen(const VirtualScreen&) = delete;
  VirtualScreen& operator=(const VirtualScreen&) = delete;

  void Configure(const Settings& settings) { settings_ = settings; }

  // Call before the original Present. Blocks in xrWaitFrame, which paces the
  // game to the headset's refresh rate while the session is running.
  void OnPresent(IDirect3DDevice9* device);

  // Call when the device is reset or recreated: drops the swapchain (sizes may change).
  void OnDeviceChanged();

  // Tears down OpenXR + Vulkan objects. Only call while the D3D9 device is
  // alive; there is intentionally no destructor cleanup (see xr::Context).
  void Shutdown();

 private:
  bool EnsureInit(IDirect3DDevice9* device, const DxvkInterop& dxvk);
  bool EnsureSwapchain(uint32_t width, uint32_t height);
  void DestroySwapchain();
  void DestroyVulkan();
  bool CopyToSwapchain(const DxvkInterop& dxvk, IDirect3DSurface9* backbuffer, VkImage target,
                       uint32_t width, uint32_t height);

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
  bool rawCopy_ = true;  // vkCmdCopyImage (bit-exact) vs vkCmdBlitImage
  uint32_t width_ = 0;
  uint32_t height_ = 0;

  uint64_t nextInitAttemptMs_ = 0;
  uint64_t framesSubmitted_ = 0;
  bool recenterKeyDown_ = false;
  bool recenterPending_ = true;  // recentre once on the first frame
};

}  // namespace vrcore
