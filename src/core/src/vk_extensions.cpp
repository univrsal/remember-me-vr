// Vulkan extensions for the OpenXR runtime. OpenXR's Vulkan binding needs the
// runtime's instance and device extensions enabled on DXVK's VkInstance and
// VkDevice. DXVK only asks an OpenXR runtime for them through Wine's
// wineopenxr.dll, so on native Windows it never enables them and
// xrGetVulkanGraphicsDeviceKHR fails. These hooks on vulkan-1.dll's
// vkCreateInstance / vkCreateDevice add the extensions OpenXR runtimes ask for
// (where the driver supports them) before DXVK creates its objects.
//
// Under Wine/Proton DXVK loads winevulkan.dll directly and wineopenxr does
// this already, so the hooks aren't installed there.

#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include <vulkan/vulkan.h>

#include "internal.h"
#include "vrcore/hook.h"
#include "vrcore/log.h"

namespace vrcore::detail {
namespace {

// What SteamVR's OpenXR runtime (and other Windows runtimes) list from
// xrGetVulkanInstanceExtensionsKHR / xrGetVulkanDeviceExtensionsKHR.
constexpr const char* kInstanceExtensions[] = {
    "VK_KHR_external_memory_capabilities",
    "VK_KHR_external_semaphore_capabilities",
    "VK_KHR_external_fence_capabilities",
    "VK_KHR_get_physical_device_properties2",
};
constexpr const char* kDeviceExtensions[] = {
    "VK_KHR_external_memory",       "VK_KHR_external_memory_win32", "VK_KHR_external_semaphore",
    "VK_KHR_external_semaphore_win32", "VK_KHR_external_fence",     "VK_KHR_external_fence_win32",
    "VK_KHR_timeline_semaphore",    "VK_KHR_dedicated_allocation",  "VK_KHR_get_memory_requirements2",
    "VK_KHR_win32_keyed_mutex",
};

PFN_vkCreateInstance g_origCreateInstance = nullptr;
PFN_vkCreateDevice g_origCreateDevice = nullptr;
PFN_vkEnumerateInstanceExtensionProperties g_enumInstanceExts = nullptr;
PFN_vkEnumerateDeviceExtensionProperties g_enumDeviceExts = nullptr;
std::vector<std::string> g_addedInstanceExtensions;

bool Listed(const char* const* names, uint32_t count, const char* name) {
  for (uint32_t i = 0; i < count; ++i) {
    if (names[i] && std::strcmp(names[i], name) == 0) return true;
  }
  return false;
}

bool Supported(const std::vector<VkExtensionProperties>& available, const char* name) {
  for (const auto& ext : available) {
    if (std::strcmp(ext.extensionName, name) == 0) return true;
  }
  return false;
}

// `names` + the wanted extensions that `available` has and `names` lacks.
template <size_t N>
std::vector<const char*> Extend(const char* const* names, uint32_t count,
                                const std::vector<VkExtensionProperties>& available, const char* const (&wanted)[N],
                                std::string& added) {
  std::vector<const char*> out(names, names + count);
  for (const char* name : wanted) {
    if (Listed(names, count, name) || !Supported(available, name)) continue;
    out.push_back(name);
    added += name;
    added += ' ';
  }
  return out;
}

VKAPI_ATTR VkResult VKAPI_CALL HookCreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* alloc,
                                                  VkInstance* instance) {
  if (!info) return g_origCreateInstance(info, alloc, instance);
  uint32_t count = 0;
  std::vector<VkExtensionProperties> available;
  if (g_enumInstanceExts(nullptr, &count, nullptr) == VK_SUCCESS) {
    available.resize(count);
    if (g_enumInstanceExts(nullptr, &count, available.data()) < VK_SUCCESS) available.clear();
    available.resize(count);
  }
  std::string added;
  const auto names = Extend(info->ppEnabledExtensionNames, info->enabledExtensionCount, available,
                            kInstanceExtensions, added);
  VkInstanceCreateInfo patched = *info;
  patched.enabledExtensionCount = static_cast<uint32_t>(names.size());
  patched.ppEnabledExtensionNames = names.data();
  VkResult result = g_origCreateInstance(&patched, alloc, instance);
  if (result != VK_SUCCESS && !added.empty()) {
    VR_LOG_WARN("vkCreateInstance with OpenXR extensions (%s) failed (%d); retrying without", added.c_str(),
                static_cast<int>(result));
    return g_origCreateInstance(info, alloc, instance);
  }
  if (result == VK_SUCCESS && !added.empty()) {
    VR_LOG_INFO("vkCreateInstance: added OpenXR extensions %s", added.c_str());
    for (size_t i = info->enabledExtensionCount; i < names.size(); ++i) g_addedInstanceExtensions.push_back(names[i]);
  }
  return result;
}

VKAPI_ATTR VkResult VKAPI_CALL HookCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* info,
                                                const VkAllocationCallbacks* alloc, VkDevice* device) {
  if (!info) return g_origCreateDevice(physicalDevice, info, alloc, device);
  uint32_t count = 0;
  std::vector<VkExtensionProperties> available;
  if (g_enumDeviceExts(physicalDevice, nullptr, &count, nullptr) == VK_SUCCESS) {
    available.resize(count);
    if (g_enumDeviceExts(physicalDevice, nullptr, &count, available.data()) < VK_SUCCESS) available.clear();
    available.resize(count);
  }
  std::string added;
  const auto names =
      Extend(info->ppEnabledExtensionNames, info->enabledExtensionCount, available, kDeviceExtensions, added);
  VkDeviceCreateInfo patched = *info;
  patched.enabledExtensionCount = static_cast<uint32_t>(names.size());
  patched.ppEnabledExtensionNames = names.data();
  VkResult result = g_origCreateDevice(physicalDevice, &patched, alloc, device);
  if (result != VK_SUCCESS && !added.empty()) {
    VR_LOG_WARN("vkCreateDevice with OpenXR extensions (%s) failed (%d); retrying without", added.c_str(),
                static_cast<int>(result));
    return g_origCreateDevice(physicalDevice, info, alloc, device);
  }
  if (result == VK_SUCCESS && !added.empty()) VR_LOG_INFO("vkCreateDevice: added OpenXR extensions %s", added.c_str());
  return result;
}

}  // namespace

const std::vector<std::string>& AddedVulkanInstanceExtensions() { return g_addedInstanceExtensions; }

void InstallVulkanExtensionHooks() {
  // Wine/Proton: DXVK uses winevulkan.dll and gets the extensions via wineopenxr.
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll && GetProcAddress(ntdll, "wine_get_version")) return;
  HMODULE lib = LoadLibraryW(L"vulkan-1.dll");
  if (!lib) {
    VR_LOG_WARN("vulkan-1.dll not found; OpenXR's Vulkan extensions can't be added");
    return;
  }
  auto proc = [lib](const char* name) { return reinterpret_cast<void*>(GetProcAddress(lib, name)); };
  void* createInstance = proc("vkCreateInstance");
  void* createDevice = proc("vkCreateDevice");
  g_enumInstanceExts = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
      proc("vkEnumerateInstanceExtensionProperties"));
  g_enumDeviceExts =
      reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(proc("vkEnumerateDeviceExtensionProperties"));
  if (!createInstance || !createDevice || !g_enumInstanceExts || !g_enumDeviceExts) {
    VR_LOG_WARN("vulkan-1.dll lacks the expected exports; OpenXR's Vulkan extensions can't be added");
    return;
  }
  hook::Install("vkCreateInstance", createInstance, reinterpret_cast<void*>(&HookCreateInstance),
                reinterpret_cast<void**>(&g_origCreateInstance));
  hook::Install("vkCreateDevice", createDevice, reinterpret_cast<void*>(&HookCreateDevice),
                reinterpret_cast<void**>(&g_origCreateDevice));
}

}  // namespace vrcore::detail
