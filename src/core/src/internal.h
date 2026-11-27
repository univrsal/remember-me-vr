#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace vrcore::detail {

void Attach(HMODULE self);
void Detach();

bool InstallD3D9Hooks();

// vk_extensions.cpp: adds OpenXR's Vulkan extensions to DXVK's instance and
// device on native Windows (DXVK only does it under Wine).
void InstallVulkanExtensionHooks();
const std::vector<std::string>& AddedVulkanInstanceExtensions();

}  // namespace vrcore::detail
