#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace vrcore::mem {

// True if [p, p+size) is committed, readable memory. Uses VirtualQuery (with a
// small per-thread cache), so it's safe for probing unknown pointers.
bool IsReadable(const void* p, size_t size);

template <typename T>
bool Read(uintptr_t address, T& out) {
  if (!IsReadable(reinterpret_cast<const void*>(address), sizeof(T))) return false;
  std::memcpy(&out, reinterpret_cast<const void*>(address), sizeof(T));
  return true;
}

// Reads a NUL-terminated printable ASCII string of at most `max` chars.
bool ReadCString(uintptr_t address, std::string& out, size_t max = 256);

struct Section {
  char name[9];
  uintptr_t start;  // live address
  size_t size;      // virtual size (includes zero-filled .bss)
  bool writable;
  bool executable;
};

// Sections of the host executable as loaded in memory.
std::vector<Section> ExeSections();

}  // namespace vrcore::mem
