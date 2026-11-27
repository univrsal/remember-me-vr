#include "vrcore/compositor.h"

#include <algorithm>
#include <cmath>

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

bool Compositor::MenuPanel(XrPosef& pose, XrExtent2Df& size) const {
  if (!menuAnchored_) return false;
  pose = menuPose_;
  size = menuSize_;
  return true;
}

void Compositor::OnPresent(IDirect3DDevice9* device, IDirect3DTexture9* sceneTexture, IDirect3DTexture9* uiTexture,
                           IDirect3DTexture9* menuTexture) {
  if (device != device_) {
    if (device_) {
      // The old device (and DXVK's VkDevice) may already be gone: abandon its
      // Vulkan objects instead of destroying them through a dead device.
      VR_LOG_WARN("compositor: D3D9 device changed, restarting VR");
      stereoReady_ = false;
      input_.Reset();
      xr_.Shutdown();
      stereoSwapchain_ = Swapchain{.name = "stereo"};
      screenSwapchain_ = Swapchain{.name = "screen"};
      uiSwapchain_ = Swapchain{.name = "ui"};
      menuSwapchain_ = Swapchain{.name = "menu"};
      menuAnchored_ = false;
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
    DestroySwapchains();
    input_.Reset();
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
  if (recenterRequested_.exchange(false)) recenterPending_ = true;

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
    menuAnchored_ = false;  // the LOCAL space moved: place the menu again
  }
  input_.Sync(xr_, frame.predictedDisplayTime);
  if (!menuTexture) menuAnchored_ = false;

  StereoFrame stereo;
  const bool haveStereo = PopStereoFrame(stereo);

  XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerQuad uiQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerQuad menuQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
  XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  XrCompositionLayerProjectionView eyes[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                              {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
  const XrCompositionLayerBaseHeader* layers[3] = {};
  uint32_t layerCount = 0;

  if (frame.shouldRender) {
    IDirect3DSurface9* backbuffer = nullptr;
    D3DSURFACE_DESC bb{};
    if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) && backbuffer &&
        SUCCEEDED(backbuffer->GetDesc(&bb))) {
      auto isBgra8 = [](D3DFORMAT f) { return f == D3DFMT_X8R8G8B8 || f == D3DFMT_A8R8G8B8; };
      if (haveStereo && settings_.mode == Mode::Stereo) {
        // Prefer the scene image at its internal resolution; its top-left
        // (backbuffer size x supersampling factor) is what was rendered.
        IDirect3DSurface9* source = backbuffer;
        bool sourceBgra8 = isBgra8(bb.Format);
        uint32_t width = bb.Width, height = bb.Height;
        IDirect3DSurface9* sceneSurface = nullptr;
        D3DSURFACE_DESC td{};
        int scale = 0;
        if (sceneTexture && SUCCEEDED(sceneTexture->GetLevelDesc(0, &td)) && isBgra8(td.Format)) {
          scale = static_cast<int>(std::min(td.Width / bb.Width, td.Height / bb.Height));
          if (scale >= 1 && SUCCEEDED(sceneTexture->GetSurfaceLevel(0, &sceneSurface)) && sceneSurface) {
            source = sceneSurface;
            sourceBgra8 = true;
            width = bb.Width * scale;
            height = bb.Height * scale;
          } else {
            scale = 0;
          }
        }
        if (scale != sceneSourceLogged_) {
          if (scale > 0) {
            VR_LOG_INFO("compositor: stereo from the scene image (%ux%u texture, %dx the %ux%u backbuffer): %ux%u",
                        td.Width, td.Height, scale, bb.Width, bb.Height, width, height);
          } else {
            VR_LOG_INFO("compositor: stereo from the backbuffer (%ux%u)%s", bb.Width, bb.Height,
                        sceneTexture ? "; scene image unusable (size/format)" : "");
          }
          sceneSourceLogged_ = scale;
        }
        if (EnsureSwapchain(stereoSwapchain_, width, height) &&
            CopyToSwapchain(dxvk, source, sourceBgra8, stereoSwapchain_, width, height)) {
          const int32_t half = static_cast<int32_t>(width / 2);
          for (int i = 0; i < 2; ++i) {
            eyes[i].pose = stereo.pose[i];
            eyes[i].fov = stereo.fov[i];
            eyes[i].subImage.swapchain = stereoSwapchain_.handle;
            eyes[i].subImage.imageRect = {{i * half, 0}, {half, static_cast<int32_t>(height)}};
          }
          projection.space = xr_.LocalSpace();
          projection.viewCount = 2;
          projection.views = eyes;
          layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
          layerCount = 1;

          // UI over the scene: premultiplied alpha, world-locked like the screen.
          D3DSURFACE_DESC ud{};
          IDirect3DSurface9* uiSurface = nullptr;
          if (uiTexture && SUCCEEDED(uiTexture->GetLevelDesc(0, &ud)) &&
              SUCCEEDED(uiTexture->GetSurfaceLevel(0, &uiSurface)) && uiSurface &&
              EnsureSwapchain(uiSwapchain_, ud.Width, ud.Height) &&
              CopyToSwapchain(dxvk, uiSurface, true, uiSwapchain_, ud.Width, ud.Height)) {
            uiQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            uiQuad.space = xr_.LocalSpace();
            uiQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            uiQuad.subImage.swapchain = uiSwapchain_.handle;
            uiQuad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(ud.Width), static_cast<int32_t>(ud.Height)}};
            uiQuad.pose.orientation.w = 1.0f;
            uiQuad.pose.position = {0.0f, settings_.uiHeight, -settings_.uiDistance};
            uiQuad.size = {settings_.uiWidth,
                           settings_.uiWidth * static_cast<float>(ud.Height) / static_cast<float>(ud.Width)};
            layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&uiQuad);
          }
          if (uiSurface) uiSurface->Release();
        }
        if (sceneSurface) sceneSurface->Release();
      } else if (EnsureSwapchain(screenSwapchain_, bb.Width, bb.Height) &&
                 CopyToSwapchain(dxvk, backbuffer, isBgra8(bb.Format), screenSwapchain_, bb.Width, bb.Height)) {
        quad.space = xr_.LocalSpace();
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = screenSwapchain_.handle;
        quad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(bb.Width), static_cast<int32_t>(bb.Height)}};
        quad.pose.orientation.w = 1.0f;
        quad.pose.position = {0.0f, 0.0f, -settings_.screenDistance};
        quad.size = {settings_.screenWidth,
                     settings_.screenWidth * static_cast<float>(bb.Height) / static_cast<float>(bb.Width)};
        layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
        layerCount = 1;
      }
    }
    if (backbuffer) backbuffer->Release();

    // Overlay menu on top of everything.
    D3DSURFACE_DESC md{};
    IDirect3DSurface9* menuSurface = nullptr;
    if (layerCount && menuTexture && SUCCEEDED(menuTexture->GetLevelDesc(0, &md)) &&
        SUCCEEDED(menuTexture->GetSurfaceLevel(0, &menuSurface)) && menuSurface &&
        EnsureSwapchain(menuSwapchain_, md.Width, md.Height) &&
        CopyToSwapchain(dxvk, menuSurface, true, menuSwapchain_, md.Width, md.Height)) {
      menuSize_ = {settings_.menuWidth, settings_.menuWidth * static_cast<float>(md.Height) / static_cast<float>(md.Width)};
      if (!menuAnchored_) AnchorMenu(frame.predictedDisplayTime);
      menuQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
      menuQuad.space = xr_.LocalSpace();
      menuQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
      menuQuad.subImage.swapchain = menuSwapchain_.handle;
      menuQuad.subImage.imageRect = {{0, 0}, {static_cast<int32_t>(md.Width), static_cast<int32_t>(md.Height)}};
      menuQuad.pose = menuPose_;
      menuQuad.size = menuSize_;
      layers[layerCount++] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&menuQuad);
    }
    if (menuSurface) menuSurface->Release();
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
      VR_LOG_INFO("compositor: first stereo frame submitted (%ux%u, eye FOV L %.1f/%.1f R %.1f/%.1f deg)",
                  stereoSwapchain_.width, stereoSwapchain_.height, stereo.fov[0].angleLeft * 57.29578f, stereo.fov[0].angleRight * 57.29578f,
                  stereo.fov[1].angleLeft * 57.29578f, stereo.fov[1].angleRight * 57.29578f);
    }
  } else if (layerCount && ++screenFramesSubmitted_ == 1) {
    VR_LOG_INFO("compositor: first screen frame submitted (%ux%u, %.1f m wide at %.1f m)", screenSwapchain_.width,
                screenSwapchain_.height,
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
  input_.Init(xr_);  // controller actions; failure only means no controller input
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

bool Compositor::EnsureSwapchain(Swapchain& sc, uint32_t width, uint32_t height) {
  if (sc.handle != XR_NULL_HANDLE && width == sc.width && height == sc.height) return true;
  DestroySwapchain(sc);

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
  const XrResult result = xr_.fn.CreateSwapchain(xr_.Session(), &info, &sc.handle);
  if (XR_FAILED(result)) {
    VR_LOG_ERROR("compositor: xrCreateSwapchain(%ux%u, format %d) failed: %s", width, height,
                 static_cast<int>(chosen->format), xr::ResultName(result));
    sc.handle = XR_NULL_HANDLE;
    return false;
  }
  uint32_t count = 0;
  xr_.fn.EnumerateSwapchainImages(sc.handle, 0, &count, nullptr);
  std::vector<XrSwapchainImageVulkanKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
  xr_.fn.EnumerateSwapchainImages(sc.handle, count, &count,
                                  reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
  sc.images.clear();
  for (const auto& image : images) sc.images.push_back(image.image);
  sc.rawCopy = chosen->rawCopy;
  sc.width = width;
  sc.height = height;
  VR_LOG_INFO("compositor: %s swapchain %ux%u, VkFormat %d, %u images, %s", sc.name, width, height,
              static_cast<int>(chosen->format), count, sc.rawCopy ? "copy" : "blit");
  return true;
}

bool Compositor::CopyToSwapchain(const DxvkInterop& dxvk, IDirect3DSurface9* source, bool sourceIsBgra8,
                                 Swapchain& sc, uint32_t width, uint32_t height) {
  ID3D9VkInteropTexture* texture = nullptr;
  if (FAILED(source->QueryInterface(__uuidof(ID3D9VkInteropTexture), reinterpret_cast<void**>(&texture))) ||
      !texture) {
    VR_LOG_ERROR("compositor: source surface has no ID3D9VkInteropTexture");
    return false;
  }
  VkImage sourceImage = VK_NULL_HANDLE;
  VkImageLayout sourceLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  const HRESULT hr = texture->GetVulkanImageInfo(&sourceImage, &sourceLayout, nullptr);
  texture->Release();
  if (FAILED(hr) || sourceImage == VK_NULL_HANDLE) {
    VR_LOG_ERROR("compositor: GetVulkanImageInfo failed (0x%08lx)", hr);
    return false;
  }
  uint32_t index = 0;
  XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait.timeout = XR_INFINITE_DURATION;
  if (XR_FAILED(xr_.fn.AcquireSwapchainImage(sc.handle, &acquire, &index)) ||
      XR_FAILED(xr_.fn.WaitSwapchainImage(sc.handle, &wait))) {
    return false;
  }
  const VkImage target = sc.images[index];
  const bool rawCopy = sc.rawCopy && sourceIsBgra8;
  const bool ok = [&] {
  // Make sure the game's frame has finished rendering. LockSubmissionQueue
  // alone doesn't order us after DXVK's queued work, so wait on the GPU.
  dxvk.device->FlushRenderingCommands();
  dxvk.device->WaitForResource(source, D3DLOCK_READONLY);

  vk_.ResetCommandBuffer(cmd_, 0);
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vk_.BeginCommandBuffer(cmd_, &begin);

  const VkImageMemoryBarrier before[] = {
      Barrier(sourceImage, sourceLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
              VK_ACCESS_TRANSFER_READ_BIT),
      Barrier(target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
              VK_ACCESS_TRANSFER_WRITE_BIT),
  };
  vk_.CmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 2, before);

  const VkImageSubresourceLayers layers{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  if (rawCopy) {
    VkImageCopy region{};
    region.srcSubresource = layers;
    region.dstSubresource = layers;
    region.extent = {width, height, 1};
    vk_.CmdCopyImage(cmd_, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &region);
  } else {
    VkImageBlit region{};
    region.srcSubresource = layers;
    region.dstSubresource = layers;
    region.srcOffsets[1] = {static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
    region.dstOffsets[1] = region.srcOffsets[1];
    vk_.CmdBlitImage(cmd_, sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     1, &region, VK_FILTER_NEAREST);
  }

  const VkImageMemoryBarrier after[] = {
      Barrier(sourceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sourceLayout, VK_ACCESS_TRANSFER_READ_BIT,
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
  }();
  XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xr_.fn.ReleaseSwapchainImage(sc.handle, &release);
  return ok;
}

void Compositor::OnDeviceChanged() { DestroySwapchains(); }

// In front of the head (yaw only, upright), menuDistance away, menuHeight
// below eye height; facing the viewer.
void Compositor::AnchorMenu(XrTime time) {
  XrView views[2];
  XrVector3f head{0.0f, 0.0f, 0.0f};
  float yaw = 0.0f;
  if (xr_.LocateEyes(time, views)) {
    head = {(views[0].pose.position.x + views[1].pose.position.x) * 0.5f,
            (views[0].pose.position.y + views[1].pose.position.y) * 0.5f,
            (views[0].pose.position.z + views[1].pose.position.z) * 0.5f};
    // Forward (-Z) rotated by the head orientation.
    const XrQuaternionf& q = views[0].pose.orientation;
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    if (fx * fx + fz * fz > 1e-6f) yaw = std::atan2(-fx, -fz);
  }
  menuPose_.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
  menuPose_.position = {head.x - std::sin(yaw) * settings_.menuDistance, head.y + settings_.menuHeight,
                        head.z - std::cos(yaw) * settings_.menuDistance};
  menuAnchored_ = true;
  VR_LOG_INFO("compositor: menu panel placed %.2f m ahead (yaw %.0f deg)", settings_.menuDistance,
              yaw * 57.29578f);
}

void Compositor::DestroySwapchain(Swapchain& sc) {
  if (sc.handle != XR_NULL_HANDLE && xr_.fn.DestroySwapchain) xr_.fn.DestroySwapchain(sc.handle);
  sc.handle = XR_NULL_HANDLE;
  sc.images.clear();
  sc.width = sc.height = 0;
}

void Compositor::DestroySwapchains() {
  DestroySwapchain(stereoSwapchain_);
  DestroySwapchain(screenSwapchain_);
  DestroySwapchain(uiSwapchain_);
  DestroySwapchain(menuSwapchain_);
  menuAnchored_ = false;
  sceneSourceLogged_ = -1;
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
  DestroySwapchains();
  input_.Reset();
  xr_.Shutdown();
  DestroyVulkan();
  device_ = nullptr;
}

}  // namespace vrcore
