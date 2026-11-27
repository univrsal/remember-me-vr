#pragma once

#include <cstdint>
#include <string>

#include <windows.h>

namespace vrcore {

// Facts about the host executable, read from its in-memory PE headers.
struct ExeInfo {
  HMODULE base = nullptr;          // actual load address (ASLR applies)
  uint32_t preferredBase = 0;      // OptionalHeader.ImageBase
  uint32_t imageSize = 0;
  uint32_t timestamp = 0;          // FileHeader.TimeDateStamp: identifies the build
  std::wstring path;
  std::wstring fileName;

  // Converts a VA from a disassembler (at the preferred base) to a live pointer.
  template <typename T = void>
  T* FromPreferredVA(uint32_t va) const {
    return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + (va - preferredBase));
  }
  template <typename T = void>
  T* FromRVA(uint32_t rva) const {
    return reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(base) + rva);
  }
};

const ExeInfo& Exe();

// Directory containing the mod DLL, with a trailing backslash.
const std::wstring& ModDirectory();

std::string Narrow(const std::wstring& s);

namespace detail {
void InitProcessInfo(HMODULE modModule);
}

}  // namespace vrcore
