// Minimal stand-in for a game executable, for testing a built proxy DLL under
// Wine without launching the game. Copy it next to the mod DLL, renamed to the
// game's exe name (see tools/smoke-test.sh), so the mod activates fully.
//
// Exercises: proxy export forwarding (DirectInput8Create), Direct3DCreate9 ->
// CreateDevice -> Present hooks, and device Reset.

#include <cstdio>

#include <windows.h>
#include <unknwn.h>
#include <d3d9.h>

namespace {

using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);

// IID_IDirectInput8W {BF798031-483A-4DA2-AA99-5D64ED369700}
constexpr GUID kIID_IDirectInput8W = {0xbf798031, 0x483a, 0x4da2, {0xaa, 0x99, 0x5d, 0x64, 0xed, 0x36, 0x97, 0x00}};

int Fail(const char* what, long code = 0) {
  std::printf("FAIL: %s (0x%08lx)\n", what, code);
  return 1;
}

}  // namespace

int main() {
  // Same order as the game: the proxy is a load-time import there; here we load it first.
  HMODULE dinput = LoadLibraryW(L"dinput8.dll");
  if (!dinput) return Fail("LoadLibrary(dinput8.dll)", GetLastError());
  auto create8 = reinterpret_cast<DirectInput8Create_t>(
      reinterpret_cast<void*>(GetProcAddress(dinput, "DirectInput8Create")));
  if (!create8) return Fail("GetProcAddress(DirectInput8Create)");

  IUnknown* di = nullptr;
  HRESULT hr = create8(GetModuleHandleW(nullptr), 0x0800, kIID_IDirectInput8W, reinterpret_cast<void**>(&di), nullptr);
  if (FAILED(hr) || !di) return Fail("DirectInput8Create", hr);
  di->Release();
  std::printf("ok: DirectInput8Create forwarded\n");

  HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
  auto create9 = reinterpret_cast<Direct3DCreate9_t>(
      reinterpret_cast<void*>(GetProcAddress(d3d9, "Direct3DCreate9")));
  if (!create9) return Fail("Direct3DCreate9 missing");
  IDirect3D9* d3d = create9(D3D_SDK_VERSION);
  if (!d3d) return Fail("Direct3DCreate9");

  HWND wnd = CreateWindowExW(0, L"STATIC", L"vrmod smoke test", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr,
                             nullptr, nullptr, nullptr);
  D3DPRESENT_PARAMETERS pp{};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_UNKNOWN;
  pp.hDeviceWindow = wnd;
  IDirect3DDevice9* device = nullptr;
  hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
  if (FAILED(hr) || !device) return Fail("CreateDevice", hr);
  std::printf("ok: device created\n");

  for (int i = 0; i < 3; ++i) {
    device->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 64, 128), 1.0f, 0);
    hr = device->Present(nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr)) return Fail("Present", hr);
  }
  hr = device->Reset(&pp);
  if (FAILED(hr)) return Fail("Reset", hr);
  hr = device->Present(nullptr, nullptr, nullptr, nullptr);
  if (FAILED(hr)) return Fail("Present after Reset", hr);
  std::printf("ok: 4 presents, 1 reset\n");

  device->Release();
  d3d->Release();
  DestroyWindow(wnd);
  std::printf("PASS\n");
  return 0;
}
