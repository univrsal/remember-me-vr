#pragma once

#include <vulkan/vulkan.h>

namespace vrcore::vk {

// The Vulkan entry points vrcore uses, resolved against the VkInstance/VkDevice
// DXVK created. Loaded from winevulkan.dll when present (what DXVK uses under
// Wine/Proton), otherwise vulkan-1.dll.
struct Functions {
  PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;

  PFN_vkCreateCommandPool CreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
  PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
  PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyImage CmdCopyImage = nullptr;
  PFN_vkCmdBlitImage CmdBlitImage = nullptr;
  PFN_vkQueueSubmit QueueSubmit = nullptr;
  PFN_vkCreateFence CreateFence = nullptr;
  PFN_vkDestroyFence DestroyFence = nullptr;
  PFN_vkWaitForFences WaitForFences = nullptr;
  PFN_vkResetFences ResetFences = nullptr;
};

// Returns false (and logs) if any function is missing.
bool Load(VkInstance instance, VkDevice device, Functions& out);

}  // namespace vrcore::vk
