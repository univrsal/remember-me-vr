// dinput8.dll proxy: forwards every export to the system dinput8.dll.
// The real DLL is loaded lazily on first use, never from DllMain.

#include <windows.h>
#include <unknwn.h>

namespace {

HMODULE g_real = nullptr;

FARPROC Real(const char* name) {
  if (!g_real) {
    wchar_t path[MAX_PATH];
    const UINT len = GetSystemDirectoryW(path, MAX_PATH);  // SysWOW64 for a 32-bit process
    if (len == 0 || len + 13 >= MAX_PATH) return nullptr;
    lstrcatW(path, L"\\dinput8.dll");
    g_real = LoadLibraryW(path);
    if (!g_real) return nullptr;
  }
  return GetProcAddress(g_real, name);
}

template <typename Fn>
Fn RealFn(const char* name) {
  return reinterpret_cast<Fn>(reinterpret_cast<void*>(Real(name)));
}

}  // namespace

extern "C" {

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
  auto fn = RealFn<decltype(&Proxy_DirectInput8Create)>("DirectInput8Create");
  return fn ? fn(inst, version, riid, out, outer) : E_FAIL;
}

HRESULT WINAPI Proxy_DllCanUnloadNow() {
  auto fn = RealFn<decltype(&Proxy_DllCanUnloadNow)>("DllCanUnloadNow");
  return fn ? fn() : S_FALSE;
}

HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
  auto fn = RealFn<decltype(&Proxy_DllGetClassObject)>("DllGetClassObject");
  return fn ? fn(clsid, riid, out) : E_FAIL;
}

HRESULT WINAPI Proxy_DllRegisterServer() {
  auto fn = RealFn<decltype(&Proxy_DllRegisterServer)>("DllRegisterServer");
  return fn ? fn() : E_FAIL;
}

HRESULT WINAPI Proxy_DllUnregisterServer() {
  auto fn = RealFn<decltype(&Proxy_DllUnregisterServer)>("DllUnregisterServer");
  return fn ? fn() : E_FAIL;
}

LPCVOID WINAPI Proxy_GetdfDIJoystick() {
  auto fn = RealFn<decltype(&Proxy_GetdfDIJoystick)>("GetdfDIJoystick");
  return fn ? fn() : nullptr;
}

}  // extern "C"
