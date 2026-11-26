#pragma once

// DXVK D3D9 <-> Vulkan interop interfaces.
//
// Trimmed from DXVK's src/d3d9/d3d9_interfaces.h
// (https://github.com/doitsujin/dxvk), Copyright (c) 2017 Philip Rebohle and
// contributors, zlib license. Only the interop interfaces are included.
// Method order must match DXVK exactly; DXVK only ever appends methods, so
// newer DXVK builds stay compatible with these declarations.

#include <d3d9.h>
#include <vulkan/vulkan.h>

MIDL_INTERFACE("3461a81b-ce41-485b-b6b5-fcf08ba6a6bd")
ID3D9VkInteropInterface : public IUnknown {
  virtual void STDMETHODCALLTYPE GetInstanceHandle(VkInstance* pInstance) = 0;
  virtual void STDMETHODCALLTYPE GetPhysicalDeviceHandle(UINT Adapter, VkPhysicalDevice* pPhysicalDevice) = 0;
};

MIDL_INTERFACE("d56344f5-8d35-46fd-806d-94c351b472c1")
ID3D9VkInteropTexture : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage* pHandle, VkImageLayout* pLayout,
                                                       VkImageCreateInfo* pInfo) = 0;
};

MIDL_INTERFACE("2eaa4b89-0107-4bdb-87f7-0f541c493ce0")
ID3D9VkInteropDevice : public IUnknown {
  virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance* pInstance, VkPhysicalDevice* pPhysDev,
                                                  VkDevice* pDevice) = 0;
  virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue* pQueue, uint32_t* pQueueIndex,
                                                    uint32_t* pQueueFamilyIndex) = 0;
  virtual void STDMETHODCALLTYPE TransitionTextureLayout(ID3D9VkInteropTexture* pTexture,
                                                         const VkImageSubresourceRange* pSubresources,
                                                         VkImageLayout OldLayout, VkImageLayout NewLayout) = 0;
  virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
  virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
  virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
  virtual void STDMETHODCALLTYPE LockDevice() = 0;
  virtual void STDMETHODCALLTYPE UnlockDevice() = 0;
  virtual bool STDMETHODCALLTYPE WaitForResource(IDirect3DResource9* pResource, DWORD MapFlags) = 0;
};

#ifndef _MSC_VER
__CRT_UUID_DECL(ID3D9VkInteropInterface, 0x3461a81b, 0xce41, 0x485b, 0xb6, 0xb5, 0xfc, 0xf0, 0x8b, 0xa6, 0xa6, 0xbd);
__CRT_UUID_DECL(ID3D9VkInteropTexture,   0xd56344f5, 0x8d35, 0x46fd, 0x80, 0x6d, 0x94, 0xc3, 0x51, 0xb4, 0x72, 0xc1);
__CRT_UUID_DECL(ID3D9VkInteropDevice,    0x2eaa4b89, 0x0107, 0x4bdb, 0x87, 0xf7, 0x0f, 0x54, 0x1c, 0x49, 0x3c, 0xe0);
#endif

namespace vrcore {

// Vulkan objects behind a DXVK D3D9 device. Only valid when running on DXVK.
struct DxvkInterop {
  ID3D9VkInteropDevice* device = nullptr;  // owned reference
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice vkDevice = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queueIndex = 0;
  uint32_t queueFamilyIndex = 0;
};

// Queries the interop interface. Returns false (and leaves `out` empty) when
// the device isn't a DXVK device, e.g. native D3D9 on Windows without DXVK.
bool QueryDxvkInterop(IDirect3DDevice9* device, DxvkInterop& out);
void ReleaseDxvkInterop(DxvkInterop& interop);

}  // namespace vrcore
