// Minimal OpenXR runtime loader: active-runtime discovery + loader/runtime
// interface negotiation, as specified in the OpenXR loader spec
// ("Loader/Runtime Interface Negotiation"). No API layer support.

#include <cstdlib>
#include <string>

#include <openxr/openxr_loader_negotiation.h>

#include "vrcore/log.h"
#include "vrcore/process.h"
#include "vrcore/xr.h"

namespace vrcore::xr {
namespace {

std::wstring Widen(const std::string& s) {
  if (s.empty()) return {};
  const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring out(len, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
  return out;
}

std::wstring ActiveRuntimeJson() {
  wchar_t env[MAX_PATH];
  const DWORD envLen = GetEnvironmentVariableW(L"XR_RUNTIME_JSON", env, MAX_PATH);
  if (envLen > 0 && envLen < MAX_PATH) return env;

  // A 32-bit process reads the 32-bit registry view (Wow6432Node), like the official loader.
  wchar_t value[MAX_PATH];
  DWORD size = sizeof(value);
  const LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime",
                                      RRF_RT_REG_SZ, nullptr, value, &size);
  return status == ERROR_SUCCESS ? std::wstring(value) : std::wstring();
}

std::string ReadTextFile(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  std::string data;
  char buffer[4096];
  DWORD read = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) data.append(buffer, read);
  CloseHandle(file);
  return data;
}

// Extracts runtime.library_path from the manifest. The manifest format is
// fixed and tiny, so a targeted string scan is enough (handles \\ and \" escapes).
std::string LibraryPath(const std::string& json) {
  size_t key = json.find("\"library_path\"");
  if (key == std::string::npos) return {};
  size_t pos = json.find(':', key);
  pos = (pos == std::string::npos) ? pos : json.find('"', pos);
  if (pos == std::string::npos) return {};
  std::string out;
  for (++pos; pos < json.size() && json[pos] != '"'; ++pos) {
    if (json[pos] == '\\' && pos + 1 < json.size()) ++pos;
    out.push_back(json[pos]);
  }
  return out;
}

}  // namespace

PFN_xrGetInstanceProcAddr LoadRuntime() {
  const std::wstring manifest = ActiveRuntimeJson();
  if (manifest.empty()) {
    VR_LOG_ERROR("xr: no active OpenXR runtime (registry ActiveRuntime / XR_RUNTIME_JSON not set)");
    return nullptr;
  }
  const std::string json = ReadTextFile(manifest);
  std::wstring library = Widen(LibraryPath(json));
  if (library.empty()) {
    VR_LOG_ERROR("xr: could not read library_path from %s", Narrow(manifest).c_str());
    return nullptr;
  }
  // Relative library paths are relative to the manifest's directory.
  const bool absolute = library.size() > 2 && (library[1] == L':' || (library[0] == L'\\' && library[1] == L'\\'));
  if (!absolute) library = manifest.substr(0, manifest.find_last_of(L"\\/") + 1) + library;
  VR_LOG_INFO("xr: runtime manifest %s -> %s", Narrow(manifest).c_str(), Narrow(library).c_str());

  HMODULE runtime = LoadLibraryW(library.c_str());
  if (!runtime) {
    VR_LOG_ERROR("xr: LoadLibrary(%s) failed (error %lu)", Narrow(library).c_str(), GetLastError());
    return nullptr;
  }
  auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderRuntimeInterface>(
      reinterpret_cast<void*>(GetProcAddress(runtime, "xrNegotiateLoaderRuntimeInterface")));
  if (!negotiate) {
    VR_LOG_ERROR("xr: runtime has no xrNegotiateLoaderRuntimeInterface");
    return nullptr;
  }

  XrNegotiateLoaderInfo loaderInfo{};
  loaderInfo.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
  loaderInfo.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
  loaderInfo.structSize = sizeof(loaderInfo);
  loaderInfo.minInterfaceVersion = 1;
  loaderInfo.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
  loaderInfo.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
  loaderInfo.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);

  XrNegotiateRuntimeRequest request{};
  request.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
  request.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
  request.structSize = sizeof(request);

  const XrResult result = negotiate(&loaderInfo, &request);
  if (XR_FAILED(result) || !request.getInstanceProcAddr) {
    VR_LOG_ERROR("xr: runtime negotiation failed: %s", ResultName(result));
    return nullptr;
  }
  VR_LOG_INFO("xr: negotiated runtime interface v%u, API %u.%u.%u", request.runtimeInterfaceVersion,
              XR_VERSION_MAJOR(request.runtimeApiVersion), XR_VERSION_MINOR(request.runtimeApiVersion),
              XR_VERSION_PATCH(request.runtimeApiVersion));
  return request.getInstanceProcAddr;
}

}  // namespace vrcore::xr
