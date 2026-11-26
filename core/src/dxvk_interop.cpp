#include "vrcore/dxvk_interop.h"

#include "vrcore/log.h"

namespace vrcore {

bool QueryDxvkInterop(IDirect3DDevice9* device, DxvkInterop& out) {
  out = {};
  ID3D9VkInteropDevice* interop = nullptr;
  const HRESULT hr = device->QueryInterface(__uuidof(ID3D9VkInteropDevice), reinterpret_cast<void**>(&interop));
  if (FAILED(hr) || !interop) {
    VR_LOG_WARN("device has no ID3D9VkInteropDevice (hr=0x%08lx): not running on DXVK?", hr);
    return false;
  }
  out.device = interop;
  interop->GetVulkanHandles(&out.instance, &out.physicalDevice, &out.vkDevice);
  interop->GetSubmissionQueue(&out.queue, &out.queueIndex, &out.queueFamilyIndex);
  VR_LOG_INFO("DXVK interop: VkInstance=%p VkPhysicalDevice=%p VkDevice=%p VkQueue=%p (family %u, index %u)",
              static_cast<void*>(out.instance), static_cast<void*>(out.physicalDevice),
              static_cast<void*>(out.vkDevice), static_cast<void*>(out.queue), out.queueFamilyIndex,
              out.queueIndex);
  return true;
}

void ReleaseDxvkInterop(DxvkInterop& interop) {
  if (interop.device) interop.device->Release();
  interop = {};
}

}  // namespace vrcore
