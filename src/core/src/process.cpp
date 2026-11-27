#include "vrcore/process.h"

namespace vrcore {
namespace {

ExeInfo g_exe;
std::wstring g_modDir;

std::wstring ModulePath(HMODULE module) {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD len = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (len < path.size()) {
      path.resize(len);
      return path;
    }
    path.resize(path.size() * 2);
  }
}

// OptionalHeader.ImageBase as stored in the PE file at `path`, or `fallback`.
uint32_t FileImageBase(const std::wstring& path, uint32_t fallback) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return fallback;
  uint32_t result = fallback;
  auto readAt = [file](LONG offset, void* out, DWORD size) {
    DWORD read = 0;
    return SetFilePointer(file, offset, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
           ReadFile(file, out, size, &read, nullptr) && read == size;
  };
  IMAGE_DOS_HEADER dos{};
  IMAGE_NT_HEADERS32 nt{};
  if (readAt(0, &dos, sizeof(dos)) && dos.e_magic == IMAGE_DOS_SIGNATURE && readAt(dos.e_lfanew, &nt, sizeof(nt)) &&
      nt.Signature == IMAGE_NT_SIGNATURE && nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
    result = nt.OptionalHeader.ImageBase;
  }
  CloseHandle(file);
  return result;
}

}  // namespace

const ExeInfo& Exe() { return g_exe; }
const std::wstring& ModDirectory() { return g_modDir; }

std::string Narrow(const std::wstring& s) {
  if (s.empty()) return {};
  const int len = WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(len, '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len, nullptr, nullptr);
  return out;
}

namespace detail {

void InitProcessInfo(HMODULE modModule) {
  const std::wstring modPath = ModulePath(modModule);
  g_modDir = modPath.substr(0, modPath.find_last_of(L"\\/") + 1);

  g_exe.base = GetModuleHandleW(nullptr);
  g_exe.path = ModulePath(nullptr);
  g_exe.fileName = g_exe.path.substr(g_exe.path.find_last_of(L"\\/") + 1);

  // PE headers are never encrypted by SteamStub, so they're readable this early.
  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(g_exe.base);
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(reinterpret_cast<const char*>(g_exe.base) + dos->e_lfanew);
  // When Windows relocates the image (ASLR), the loader rewrites ImageBase in the
  // mapped header to the actual base, so take the preferred base from the file.
  g_exe.preferredBase = FileImageBase(g_exe.path, nt->OptionalHeader.ImageBase);
  g_exe.imageSize = nt->OptionalHeader.SizeOfImage;
  g_exe.timestamp = nt->FileHeader.TimeDateStamp;
}

}  // namespace detail
}  // namespace vrcore
