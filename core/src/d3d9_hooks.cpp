// D3D9 entry points: Direct3DCreate9 -> IDirect3D9::CreateDevice -> device
// Present/Reset and swapchain Present. Hooks are vtable-slot inline hooks, so
// they apply to every object of the implementing class (DXVK or native).

#include <d3d9.h>

#include "internal.h"
#include "vrcore/dxvk_interop.h"
#include "vrcore/game.h"
#include "vrcore/hook.h"
#include "vrcore/log.h"

namespace vrcore::detail {
namespace {

// vtable slots (d3d9.h declaration order)
constexpr size_t kD3D9_CreateDevice = 16;
constexpr size_t kDevice_GetSwapChain = 14;
constexpr size_t kDevice_Reset = 16;
constexpr size_t kDevice_Present = 17;
constexpr size_t kSwapChain_Present = 3;

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using Direct3DCreate9Ex_t = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);
using CreateDevice_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                                   D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Reset_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using SwapChainPresent_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND,
                                                       const RGNDATA*, DWORD);

Direct3DCreate9_t g_origCreate9 = nullptr;
Direct3DCreate9Ex_t g_origCreate9Ex = nullptr;
CreateDevice_t g_origCreateDevice = nullptr;
Reset_t g_origReset = nullptr;
Present_t g_origPresent = nullptr;
SwapChainPresent_t g_origSwapChainPresent = nullptr;

// DXVK's device Present may call into the swapchain's Present; only report the outermost one.
thread_local bool t_inPresent = false;
uint64_t g_presentCount = 0;

void NotifyPresent(IDirect3DDevice9* device, const char* via) {
  if (++g_presentCount == 1) VR_LOG_INFO("first Present (via %s)", via);
  GetGame().OnPresent(device);
}

HRESULT STDMETHODCALLTYPE HookPresent(IDirect3DDevice9* self, const RECT* src, const RECT* dst, HWND wnd,
                                      const RGNDATA* dirty) {
  if (t_inPresent) return g_origPresent(self, src, dst, wnd, dirty);
  t_inPresent = true;
  NotifyPresent(self, "IDirect3DDevice9::Present");
  const HRESULT hr = g_origPresent(self, src, dst, wnd, dirty);
  t_inPresent = false;
  return hr;
}

HRESULT STDMETHODCALLTYPE HookSwapChainPresent(IDirect3DSwapChain9* self, const RECT* src, const RECT* dst,
                                               HWND wnd, const RGNDATA* dirty, DWORD flags) {
  if (t_inPresent) return g_origSwapChainPresent(self, src, dst, wnd, dirty, flags);
  t_inPresent = true;
  IDirect3DDevice9* device = nullptr;
  if (SUCCEEDED(self->GetDevice(&device)) && device) {
    NotifyPresent(device, "IDirect3DSwapChain9::Present");
    device->Release();
  }
  const HRESULT hr = g_origSwapChainPresent(self, src, dst, wnd, dirty, flags);
  t_inPresent = false;
  return hr;
}

HRESULT STDMETHODCALLTYPE HookReset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* params) {
  VR_LOG_INFO("Reset: %ux%u windowed=%d", params ? params->BackBufferWidth : 0,
              params ? params->BackBufferHeight : 0, params ? params->Windowed : -1);
  GetGame().OnBeforeReset(self);
  const HRESULT hr = g_origReset(self, params);
  if (FAILED(hr)) VR_LOG_WARN("Reset failed: 0x%08lx", hr);
  if (params) GetGame().OnAfterReset(self, *params, hr);
  return hr;
}

void HookDevice(IDirect3DDevice9* device) {
  if (!g_origPresent) {
    hook::InstallVTable("IDirect3DDevice9::Present", device, kDevice_Present, &HookPresent, &g_origPresent);
    hook::InstallVTable("IDirect3DDevice9::Reset", device, kDevice_Reset, &HookReset, &g_origReset);
  }
  if (!g_origSwapChainPresent) {
    IDirect3DSwapChain9* swapchain = nullptr;
    using GetSwapChain_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, IDirect3DSwapChain9**);
    auto getSwapChain = reinterpret_cast<GetSwapChain_t>((*reinterpret_cast<void***>(device))[kDevice_GetSwapChain]);
    if (SUCCEEDED(getSwapChain(device, 0, &swapchain)) && swapchain) {
      hook::InstallVTable("IDirect3DSwapChain9::Present", swapchain, kSwapChain_Present, &HookSwapChainPresent,
                          &g_origSwapChainPresent);
      swapchain->Release();
    }
  }
}

HRESULT STDMETHODCALLTYPE HookCreateDevice(IDirect3D9* self, UINT adapter, D3DDEVTYPE type, HWND focus,
                                           DWORD flags, D3DPRESENT_PARAMETERS* params, IDirect3DDevice9** out) {
  VR_LOG_INFO("CreateDevice: adapter=%u type=%d flags=0x%08lx %ux%u fmt=%d windowed=%d", adapter, type, flags,
              params ? params->BackBufferWidth : 0, params ? params->BackBufferHeight : 0,
              params ? static_cast<int>(params->BackBufferFormat) : 0, params ? params->Windowed : -1);
  const HRESULT hr = g_origCreateDevice(self, adapter, type, focus, flags, params, out);
  if (FAILED(hr) || !out || !*out) {
    VR_LOG_ERROR("CreateDevice failed: 0x%08lx", hr);
    return hr;
  }

  IDirect3DDevice9* device = *out;
  HookDevice(device);
  CaptureDxvkInstanceExtensions(self);

  DxvkInterop interop;
  const bool onDxvk = QueryDxvkInterop(device, interop);
  // The interop interface holds a device reference; release it after the
  // callback so we never keep the game's device alive. Game modules that need
  // it longer must AddRef interop.device themselves.
  GetGame().OnDeviceCreated(device, *params, onDxvk ? &interop : nullptr);
  ReleaseDxvkInterop(interop);
  return hr;
}

void HookDirect3D(IDirect3D9* d3d) {
  if (d3d && !g_origCreateDevice) {
    hook::InstallVTable("IDirect3D9::CreateDevice", d3d, kD3D9_CreateDevice, &HookCreateDevice,
                        &g_origCreateDevice);
  }
}

IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdkVersion) {
  IDirect3D9* d3d = g_origCreate9(sdkVersion);
  VR_LOG_INFO("Direct3DCreate9(%u) -> %p", sdkVersion, static_cast<void*>(d3d));
  HookDirect3D(d3d);
  return d3d;
}

HRESULT WINAPI HookDirect3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out) {
  const HRESULT hr = g_origCreate9Ex(sdkVersion, out);
  VR_LOG_INFO("Direct3DCreate9Ex(%u) -> 0x%08lx (CreateDeviceEx is not hooked)", sdkVersion, hr);
  if (SUCCEEDED(hr) && out) HookDirect3D(*out);
  return hr;
}

}  // namespace

bool InstallD3D9Hooks() {
  // d3d9.dll is a static import of the game, so it's usually loaded already.
  // Loading it by name keeps the normal search order: a DXVK d3d9.dll next to
  // the exe on Windows, Proton's builtin DXVK on Linux.
  HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
  if (!d3d9) d3d9 = LoadLibraryW(L"d3d9.dll");
  if (!d3d9) {
    VR_LOG_ERROR("could not load d3d9.dll (error %lu)", GetLastError());
    return false;
  }

  auto create9 = reinterpret_cast<Direct3DCreate9_t>(reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9")));
  auto create9Ex = reinterpret_cast<Direct3DCreate9Ex_t>(reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9Ex")));
  bool ok = create9 && hook::Install("Direct3DCreate9", create9, &HookDirect3DCreate9, &g_origCreate9);
  if (create9Ex) hook::Install("Direct3DCreate9Ex", create9Ex, &HookDirect3DCreate9Ex, &g_origCreate9Ex);
  return ok;
}

}  // namespace vrcore::detail
