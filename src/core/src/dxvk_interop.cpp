#include "vrcore/dxvk_interop.h"

#include "vrcore/log.h"

namespace vrcore {
namespace {
std::vector<std::string> g_instanceExtensions;
}  // namespace


bool QueryDxvkInterop(IDirect3DDevice9* device, DxvkInterop& out, bool quiet) {
  out = {};
  ID3D9VkInteropDevice* interop = nullptr;
  const HRESULT hr = device->QueryInterface(__uuidof(ID3D9VkInteropDevice), reinterpret_cast<void**>(&interop));
  if (FAILED(hr) || !interop) {
    if (!quiet) VR_LOG_WARN("device has no ID3D9VkInteropDevice (hr=0x%08lx): not running on DXVK?", hr);
    return false;
  }
  out.device = interop;
  interop->GetVulkanHandles(&out.instance, &out.physicalDevice, &out.vkDevice);
  interop->GetSubmissionQueue(&out.queue, &out.queueIndex, &out.queueFamilyIndex);
  if (!quiet) VR_LOG_INFO("DXVK interop: VkInstance=%p VkPhysicalDevice=%p VkDevice=%p VkQueue=%p (family %u, index %u)",
              static_cast<void*>(out.instance), static_cast<void*>(out.physicalDevice),
              static_cast<void*>(out.vkDevice), static_cast<void*>(out.queue), out.queueFamilyIndex,
              out.queueIndex);
  return true;
}

void ReleaseDxvkInterop(DxvkInterop& interop) {
  if (interop.device) interop.device->Release();
  interop = {};
}

const std::vector<std::string>& DxvkInstanceExtensions() { return g_instanceExtensions; }

namespace detail {

void CaptureDxvkInstanceExtensions(IDirect3D9* d3d) {
  ID3D9VkInteropInterface1* interop = nullptr;
  if (FAILED(d3d->QueryInterface(__uuidof(ID3D9VkInteropInterface1), reinterpret_cast<void**>(&interop))) || !interop) {
    return;
  }
  UINT count = 0;
  interop->GetInstanceExtensions(&count, nullptr);
  std::vector<const char*> names(count);
  if (count > 0 && SUCCEEDED(interop->GetInstanceExtensions(&count, names.data()))) {
    g_instanceExtensions.assign(names.begin(), names.begin() + count);
    std::string list;
    for (const auto& name : g_instanceExtensions) list += name + " ";
    VR_LOG_DEBUG("DXVK instance extensions: %s", list.c_str());
  }
  interop->Release();
}

}  // namespace detail
}  // namespace vrcore
