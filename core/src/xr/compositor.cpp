#include "vrcore/compositor.h"

#include "vrcore/log.h"

namespace vrcore {
namespace {

constexpr uint64_t kInitRetryMs = 10000;
constexpr size_t kMaxQueuedFrames = 3;

struct InteropGuard {
  DxvkInterop& interop;
  ~InteropGuard() { ReleaseDxvkInterop(interop); }
};

VkImageMemoryBarrier Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags srcAccess,
                             VkAccessFlags dstAccess) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return b;
}

}  // namespace

bool Compositor::BeginStereoFrame(StereoFrame& out) {
  if (!stereoReady_.load()) return false;
  const int64_t last = lastDisplayTime_.load();
  const int64_t period = displayPeriod_.load();
  if (last == 0) return false;
  // The game thread renders the frame after the one the render thread is
  // presenting, so predict one display period past the last predicted time.
  out.displayTime = last + period;
  XrView views[2];
  if (!xr_.LocateEyes(out.displayTime, views)) return false;
  for (int i = 0; i < 2; ++i) {
    out.pose[i] = views[i].pose;
    out.fov[i] = views[i].fov;
  }
  return true;
}

void Compositor::SubmitStereoFrame(const StereoFrame& frame) {
  std::lock_guard<std::mutex> lock(queueMutex_);
  queue_.push_back(frame);
  while (queue_.size() > kMaxQueuedFrames) queue_.pop_front();
}

bool Compositor::PopStereoFrame(StereoFrame& out) {
  std::lock_guard<std::mutex> lock(queueMutex_);
  if (queue_.empty()) return false;
  out = queue_.front();
  queue_.pop_front();
  return true;
}

void Compositor::OnPresent(IDirect3DDevice9* device) {
  if (device != device_) {
    if (device_) {
      // The old device (and DXVK's VkDevice) may already be gone: abandon its
      // Vulkan objects instead of destroying them through a dead device.
      VR_LOG_WARN("compositor: D3D9 device changed, restarting VR");
      stereoReady_ = false;
      xr_.Shutdown();
      swapchain_ = XR_NULL_HANDLE;
      swapchainImages_.clear();
      pool_ = VK_NULL_HANDLE;
      cmd_ = VK_NULL_HANDLE;
      fence_ = VK_NULL_HANDLE;
      vk_ = {};
      nextInitAttemptMs_ = 0;
    }
    device_ = device;
  }

  DxvkInterop dxvk;
  if (!QueryDxvkInterop(device, dxvk, /*quiet=*/true)) return;
  InteropGuard guard{dxvk};

  if (!EnsureInit(dxvk)) return;
  if (!xr_.PollEvents()) {
    VR_LOG_WARN("compositor: OpenXR session ended; retrying in %llu s",
                static_cast<unsigned long long>(kInitRetryMs / 1000));
    stereoReady_ = false;
    DestroySwapchain();
    xr_.Shutdown();
    nextInitAttemptMs_ = GetTickCount64() + kInitRetryMs;
    return;
  }
  const bool running = xr_.Running();
  stereoReady_ = running && settings_.mode == Mode::Stereo;
  if (!running) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    queue_.clear();
    return;
  }

  if (settings_.recenterKey) {
    const bool down = (GetAsyncKeyState(settings_.recenterKey) & 0x8000) != 0;
    if (down && !recenterKeyDown_) recenterPending_ = true;
    recenterKeyDown_ = down;
  }

  XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
  XrFrameState frame{XR_TYPE_FRAME_STATE};
  XrResult result = xr_.fn.WaitFrame(xr_.Session(), &waitInfo, &frame);
  if (XR_FAILED(result)) {
    VR_LOG_ERROR("xr: xrWaitFrame failed: %s", xr::ResultName(result));
    return;
  }
  lastDisplayTime_ = frame.predictedDisplayTime;
  displayPeriod_ = frame.predictedDisplayPeriod;
  XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
  result = xr_.fn.BeginFrame(xr_.Session(), &beginInfo);
  if (XR_FAILED(result)) {
    VR_LOG_ERROR("xr: xrBeginFrame failed: %s", xr::ResultName(result));
    return;
  }
  if (recenterPending_) {
    xr_.Recenter(frame.predictedDisplayTime);
    recenterPending_ = false;
  }

  StereoFrame stereo;
  const bool haveStereo = PopStereoFrame(stereo);

  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView eyes[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  const XrCompositionLayerBaseHeader* layers[1] = {};
  uint32_t layerCount = 0;

  if (frame.shouldRender) {
    IDirect3DSurface9* backbuffer = nullptr;
    D3DSURFACE_DESC desc{};
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) && backbuffer &&
        SUCCEEDED(backbuffer->GetDesc(&desc)) && EnsureSwapchain(desc.Width, desc.Height)) {
      rawCopy_ = rawCopy_ && (desc.Format == D3DFMT_X8R8G8B8 || desc.Format == D3DFMT_A8R8G8B8);
      uint32_t index = 0;
      XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
      XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
      wait.timeout = XR_INFINITE_DURATION;
      if (XR_SUCCEEDED(xr_.fn.AcquireSwapchainImage(swapchain_, &acquire, &index)) &&
          XR_SUCCEEDED(xr_.fn.WaitSwapchainImage(swapchain_, &wait))) {
        const bool copied = CopyToSwapchain(dxvk, backbuffer, swapchainImages_[index], desc.Width, desc.Height);
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xr_.fn.ReleaseSwapchainImage(swapchain_, &release);
        if (copied && haveStereo && settings_.mode == Mode::Stereo) {
          const int32_t half = static_cast<int32_t>(desc.Width / 2);
          for (int i = 0; i < 2; ++i) {
            eyes[i].pose = stereo.pose[i];
            eyes[i].fov = stereo.fov[i];
            eyes[i].subImage.swapchain = swapchain_;
            eyes[i].subImage.imageRect = {{i * half, 0}, {half, static_cast<int32_t>(desc.Height)}};
          }
          projection.space = xr_.LocalSpace();
          projection.viewCount = 2;
          projection.views = eyes;
          layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
          layerCount = 1;
        } else if (copied) {
          quad.space = xr_.LocalSpace();
          quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
          quad.subImage.swapchain = swapchain_;
          quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(desc.Width), static_cast<int32_t>(desc.Height)}};
          quad.pose.orientation.w = 1.0f;
          quad.pose.position = {0.0f, 0.0f, -settings_.screenDistance};
          quad.size = {settings_.screenWidth,
                       settings_.screenWidth * static_cast<float>(desc.Height) / static_cast<float>(desc.Width)};
          layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
          layerCount = 1;
        }
      }
    }
    if (backbuffer) backbuffer->Release();
  }

  XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
  endInfo.displayTime = frame.predictedDisplayTime;
  endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  endInfo.layerCount = layerCount;
  endInfo.layers = layerCount ? layers : nullptr;
  result = xr_.fn.EndFrame(xr_.Session(), &endInfo);
  if (XR_FAILED(result)) {
    VR_LOG_ERROR("xr: xrEndFrame failed: %s", xr::ResultName(result));
  } else if (layerCount && layers[0] == reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)) {
    if (++stereoFramesSubmitted_ == 1) {
      VR_LOG_INFO("compositor: first stereo frame submitted (%ux%u backbuffer, eye FOV L %.1f/%.1f R %.1f/%.1f deg)",
                  width_, height_, stereo.fov[0].angleLeft * 57.29578f, stereo.fov[0].angleRight * 57.29578f,
                  stereo.fov[1].angleLeft * 57.29578f, stereo.fov[1].angleRight * 57.29578f);
    }
  } else if (layerCount && ++screenFramesSubmitted_ == 1) {
    VR_LOG_INFO("compositor: first screen frame submitted (%ux%u, %.1f m wide at %.1f m)", width_, height_,
                settings_.screenWidth, settings_.screenDistance);
  }
}

bool Compositor::EnsureInit(const DxvkInterop& dxvk) {
  if (xr_.Initialized()) return true;
  const uint64_t now = GetTickCount64();
  if (now < nextInitAttemptMs_) return false;
  nextInitAttemptMs_ = now + kInitRetryMs;

  if (!vk_.QueueSubmit) {
    if (!vk::Load(dxvk.instance, dxvk.vkDevice, vk_)) return false;
    vkDevice_ = dxvk.vkDevice;
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = dxvk.queueFamilyIndex;
    VkCommandBufferAllocateInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdInfo.commandBufferCount = 1;
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vk_.CreateCommandPool(vkDevice_, &poolInfo, nullptr, &pool_) != VK_SUCCESS ||
        (cmdInfo.commandPool = pool_, vk_.AllocateCommandBuffers(vkDevice_, &cmdInfo, &cmd_) != VK_SUCCESS) ||
        vk_.CreateFence(vkDevice_, &fenceInfo, nullptr, &fence_) != VK_SUCCESS) {
      VR_LOG_ERROR("compositor: creating Vulkan command objects failed");
      DestroyVulkan();
      return false;
    }
  }

  if (!xr_.Init(settings_.appName, dxvk)) {
    VR_LOG_WARN("compositor: OpenXR not available; retrying in %llu s",
                static_cast<unsigned long long>(kInitRetryMs / 1000));
    return false;
  }
  eyeWidth_ = xr_.RecommendedEyeWidth();
  eyeHeight_ = xr_.RecommendedEyeHeight();
  if (settings_.mode == Mode::Stereo) {
    VR_LOG_INFO("compositor: stereo mode; for 1:1 pixels set the game window to %ux%u (two eyes side by side)",
                2 * eyeWidth_, eyeHeight_.load());
  }
  recenterPending_ = true;
  stereoFramesSubmitted_ = screenFramesSubmitted_ = 0;
  return true;
}

bool Compositor::EnsureSwapchain(uint32_t width, uint32_t height) {
  if (swapchain_ != XR_NULL_HANDLE && width == width_ && height == height_) return true;
  DestroySwapchain();

  // Game output is already gamma-encoded. An sRGB swapchain filled with a raw
  // byte copy displays it correctly; anything else is a fallback.
  struct Candidate {
    VkFormat format;
    bool rawCopy;
    const char* note;
  };
  const Candidate candidates[] = {
      {VK_FORMAT_B8G8R8A8_SRGB, true, nullptr},
      {VK_FORMAT_B8G8R8A8_UNORM, true, "non-sRGB swapchain: image will look too dark"},
      {VK_FORMAT_R8G8B8A8_SRGB, false, "RGBA swapchain needs a blit: image will look too dark"},
      {VK_FORMAT_R8G8B8A8_UNORM, false, "RGBA non-sRGB swapchain: image will look too dark"},
  };
  const Candidate* chosen = nullptr;
  for (const auto& c : candidates) {
    for (int64_t f : xr_.SwapchainFormats()) {
      if (f == c.format) {
        chosen = &c;
        break;
      }
    }
    if (chosen) break;
  }
  if (!chosen) {
    VR_LOG_ERROR("compositor: runtime offers no 8-bit BGRA/RGBA swapchain format");
    return false;
  }
  if (chosen->note) VR_LOG_WARN("compositor: %s", chosen->note);

  XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  info.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  info.format = chosen->format;
  info.sampleCount = 1;
  info.width = width;
  info.height = height;
  info.faceCount = 1;
  info.arraySize = 1;
  info.mipCount = 1;
  const XrResult result = xr_.fn.CreateSwapchain(xr_.Session(), &info, &swapchain_);
  if (XR_FAILED(result)) {
    VR_LOG_ERROR("compositor: xrCreateSwapchain(%ux%u, format %d) failed: %s", width, height,
                 static_cast<int>(chosen->format), xr::ResultName(result));
    swapchain_ = XR_NULL_HANDLE;
    return false;
  }
  uint32_t count = 0;
  xr_.fn.EnumerateSwapchainImages(swapchain_, 0, &count, nullptr);
  std::vector<XrSwapchainImageVulkanKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  xr_.fn.EnumerateSwapchainImages(swapchain_, count, &count,
                                  reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
  swapchainImages_.clear();
  for (const auto& image : images) swapchainImages_.push_back(image.image);

  swapchainFormat_ = chosen->format;
  rawCopy_ = chosen->rawCopy;
  width_ = width;
  height_ = height;
  VR_LOG_INFO("compositor: swapchain %ux%u, VkFormat %d, %u images, %s", width, height,
              static_cast<int>(swapchainFormat_), count, rawCopy_ ? "copy" : "blit");
  return true;
}

bool Compositor::CopyToSwapchain(const DxvkInterop& dxvk, IDirect3DSurface9* backbuffer, VkImage target,
                                 uint32_t width, uint32_t height) {
  ID3D9VkInteropTexture* texture = nullptr;
  if (FAILED(backbuffer->QueryInterface(__uuidof(ID3D9VkInteropTexture), reinterpret_cast<void**>(&texture))) ||
      !texture) {
    VR_LOG_ERROR("compositor: backbuffer has no ID3D9VkInteropTexture");
    return false;
  }
  VkImage source = VK_NULL_HANDLE;
  VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  const HRESULT hr = texture->GetVulkanImageInfo(&source, &sourceLayout, nullptr);
  texture->Release();
  if (FAILED(hr) || source == VK_NULL_HANDLE) {
    VR_LOG_ERROR("compositor: GetVulkanImageInfo failed (0x%08lx)", hr);
    return false;
  }

  // Make sure the game's frame has finished rendering. LockSubmissionQueue
  // alone doesn't order us after DXVK's queued work, so wait on the GPU.
  dxvk.device->FlushRenderingCommands();
  dxvk.device->WaitForResource(backbuffer, D3DLOCK_READONLY);

  vk_.ResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vk_.BeginCommandBuffer(cmd_, &begin);

  const VkImageMemoryBarrier before[] = {
      Barrier(source, sourceLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
              VK_ACCESS_TRANSFER_READ_BIT),
      Barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
              VK_ACCESS_TRANSFER_WRITE_BIT),
  };
  vk_.CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, before);

  const VkImageSubresourceLayers layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  if (rawCopy_) {
    VkImageCopy region{};
    region.srcSubresource = layers;
    region.dstSubresource = layers;
    region.extent = {width, height, 1};
    vk_.CmdCopyImage(cmd_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &region);
  } else {
    VkImageBlit region{};
    region.srcSubresource = layers;
    region.dstSubresource = layers;
    region.srcOffsets[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
    region.dstOffsets[1] = region.srcOffsets[1];
    vk_.CmdBlitImage(cmd_, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &region, VK_FILTER_NEAREST);
  }

  const VkImageMemoryBarrier after[] = {
      Barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sourceLayout, VK_ACCESS_TRANSFER_READ_BIT,
              VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT),
      Barrier(target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT),
  };
  vk_.CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, after);
  vk_.EndCommandBuffer(cmd_);

  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd_;
  dxvk.device->LockSubmissionQueue();
  const VkResult vr = vk_.QueueSubmit(dxvk.queue, 1, &submit, fence_);
  dxvk.device->ReleaseSubmissionQueue();
  if (vr != VK_SUCCESS) {
    VR_LOG_ERROR("compositor: vkQueueSubmit failed (%d)", static_cast<int>(vr));
    return false;
  }
  vk_.WaitForFences(vkDevice_, 1, &fence_, VK_TRUE, UINT64_MAX);
  vk_.ResetFences(vkDevice_, 1, &fence_);
  return true;
}

void Compositor::OnDeviceChanged() { DestroySwapchain(); }

void Compositor::DestroySwapchain() {
  if (swapchain_ != XR_NULL_HANDLE && xr_.fn.DestroySwapchain) xr_.fn.DestroySwapchain(swapchain_);
  swapchain_ = XR_NULL_HANDLE;
  swapchainImages_.clear();
  width_ = height_ = 0;
}

void Compositor::DestroyVulkan() {
  if (vkDevice_ == VK_NULL_HANDLE) return;
  if (fence_) vk_.DestroyFence(vkDevice_, fence_, nullptr);
  if (pool_) vk_.DestroyCommandPool(vkDevice_, pool_, nullptr);  // frees cmd_
  fence_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  cmd_ = VK_NULL_HANDLE;
  vk_ = {};
  vkDevice_ = VK_NULL_HANDLE;
}

void Compositor::Shutdown() {
  stereoReady_ = false;
  DestroySwapchain();
  xr_.Shutdown();
  DestroyVulkan();
  device_ = nullptr;
}

}  // namespace vrcore
