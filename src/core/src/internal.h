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

// watchdog.cpp: logs all thread stacks when Presents stop for `seconds` (0 = off).
void StartHangWatchdog(unsigned seconds);
void NoteHangWatchdogPresent();
// What the render thread is doing (a string literal, or null); shown in hang dumps.
void SetHangNote(const char* note);

}  // namespace vrcore::detail
