#include "vrcore/memory.h"

#include <windows.h>

#include "vrcore/process.h"

namespace vrcore::mem {
namespace {

constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                            PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;

// Last readable region seen by this thread; most probes hit the same region.
thread_local uintptr_t t_regionStart = 0;
thread_local uintptr_t t_regionEnd = 0;

bool RegionReadable(uintptr_t address, uintptr_t& regionEnd) {
  MEMORY_BASIC_INFORMATION info{};
  if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) != sizeof(info)) return false;
  regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
  return info.State == MEM_COMMIT && (info.Protect & kReadable) && !(info.Protect & (PAGE_GUARD | PAGE_NOACCESS));
}

}  // namespace

bool IsReadable(const void* p, size_t size) {
  uintptr_t address = reinterpret_cast<uintptr_t>(p);
  if (address < 0x10000 || size == 0) return address >= 0x10000;
  const uintptr_t end = address + size;
  if (end < address) return false;
  while (address < end) {
    if (address >= t_regionStart && address < t_regionEnd) {
      address = t_regionEnd;
      continue;
    }
    uintptr_t regionEnd = 0;
    if (!RegionReadable(address, regionEnd)) return false;
    t_regionStart = address;
    t_regionEnd = regionEnd;
    address = regionEnd;
  }
  return true;
}

bool ReadCString(uintptr_t address, std::string& out, size_t max) {
  out.clear();
  for (size_t i = 0; i < max; ++i) {
    char c;
    if (!Read(address + i, c)) return false;
    if (c == '\0') return true;
    if (c < 0x20 || c > 0x7e) return false;
    out.push_back(c);
  }
  return false;
}

bool Matches(uintptr_t address, const uint8_t* expected, size_t size) {
  if (!IsReadable(reinterpret_cast<const void*>(address), size)) return false;
  return std::memcmp(reinterpret_cast<const void*>(address), expected, size) == 0;
}

std::vector<Section> ExeSections() {
  std::vector<Section> out;
  auto* base = reinterpret_cast<const char*>(Exe().base);
  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
  for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
    Section s{};
    std::memcpy(s.name, section->Name, 8);
    s.start = reinterpret_cast<uintptr_t>(base) + section->VirtualAddress;
    s.size = section->Misc.VirtualSize;
    s.writable = (section->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
    s.executable = (section->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    out.push_back(s);
  }
  return out;
}

}  // namespace vrcore::mem
