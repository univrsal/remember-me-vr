#include "vrcore/vk.h"

#include <type_traits>

#include <windows.h>

#include "vrcore/log.h"

namespace vrcore::vk {

bool Load(VkInstance instance, VkDevice device, Functions& out) {
  out = {};
  HMODULE lib = GetModuleHandleW(L"winevulkan.dll");
  const char* libName = "winevulkan.dll";
  if (!lib) {
    lib = LoadLibraryW(L"vulkan-1.dll");
    libName = "vulkan-1.dll";
  }
  if (!lib) {
    VR_LOG_ERROR("vk: neither winevulkan.dll nor vulkan-1.dll could be loaded");
    return false;
  }
  out.GetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      reinterpret_cast<void*>(GetProcAddress(lib, "vkGetInstanceProcAddr")));
  if (!out.GetInstanceProcAddr) {
    VR_LOG_ERROR("vk: %s has no vkGetInstanceProcAddr", libName);
    return false;
  }
  out.GetDeviceProcAddr =
      reinterpret_cast<PFN_vkGetDeviceProcAddr>(out.GetInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
  if (!out.GetDeviceProcAddr) {
    VR_LOG_ERROR("vk: vkGetDeviceProcAddr unavailable");
    return false;
  }

  bool ok = true;
  auto device_fn = [&](auto& slot, const char* name) {
    slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(out.GetDeviceProcAddr(device, name));
    if (!slot) {
      VR_LOG_ERROR("vk: missing %s", name);
      ok = false;
    }
  };
  device_fn(out.CreateCommandPool, "vkCreateCommandPool");
  device_fn(out.DestroyCommandPool, "vkDestroyCommandPool");
  device_fn(out.AllocateCommandBuffers, "vkAllocateCommandBuffers");
  device_fn(out.ResetCommandBuffer, "vkResetCommandBuffer");
  device_fn(out.BeginCommandBuffer, "vkBeginCommandBuffer");
  device_fn(out.EndCommandBuffer, "vkEndCommandBuffer");
  device_fn(out.CmdPipelineBarrier, "vkCmdPipelineBarrier");
  device_fn(out.CmdCopyImage, "vkCmdCopyImage");
  device_fn(out.CmdBlitImage, "vkCmdBlitImage");
  device_fn(out.QueueSubmit, "vkQueueSubmit");
  device_fn(out.CreateFence, "vkCreateFence");
  device_fn(out.DestroyFence, "vkDestroyFence");
  device_fn(out.WaitForFences, "vkWaitForFences");
  device_fn(out.ResetFences, "vkResetFences");
  if (ok) VR_LOG_DEBUG("vk: device functions loaded via %s", libName);
  return ok;
}

}  // namespace vrcore::vk
