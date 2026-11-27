// Hang watchdog: once the game has presented, a background thread checks that
// Presents keep coming. After `[Debug] HangDumpSeconds` without one it logs
// every thread's registers and likely call chain, once per hang.
//
// The call chain comes from a raw stack scan: each stack dword that points
// into a module's code just after a CALL instruction is logged as
// module+offset. That needs no symbols or unwind data (the game has neither),
// at the price of some stale entries. Resolve the mod's own offsets with
// addr2line on the matching build (address = its ImageBase + offset), the
// game's as VA = 0x00400000 + offset.
//
// Threads are suspended only while their context and stack are copied into
// static buffers; nothing allocates or logs meanwhile, because a suspended
// thread may hold the heap or log lock.

#include <atomic>
#include <cstdint>
#include <cstring>

#include <windows.h>

#include <tlhelp32.h>

#include "internal.h"
#include "vrcore/log.h"

namespace vrcore::detail {
namespace {

std::atomic<uint64_t> g_lastPresentMs{0};
std::atomic<const char*> g_note{nullptr};
std::atomic<DWORD> g_noteThread{0};
DWORD g_thresholdMs = 0;
DWORD g_watchdogThread = 0;

struct Module {
  uintptr_t start, end;
  wchar_t name[64];
};
Module g_modules[512];
int g_moduleCount = 0;

constexpr size_t kStackBytes = 64 * 1024;
uint8_t g_stack[kStackBytes];

void CollectModules() {
  g_moduleCount = 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
  if (snap == INVALID_HANDLE_VALUE) return;
  MODULEENTRY32W entry{sizeof(entry)};
  for (BOOL ok = Module32FirstW(snap, &entry); ok && g_moduleCount < 512; ok = Module32NextW(snap, &entry)) {
    Module& m = g_modules[g_moduleCount++];
    m.start = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
    m.end = m.start + entry.modBaseSize;
    lstrcpynW(m.name, entry.szModule, 64);
  }
  CloseHandle(snap);
}

const Module* ModuleAt(uintptr_t address) {
  for (int i = 0; i < g_moduleCount; ++i) {
    if (address >= g_modules[i].start && address < g_modules[i].end) return &g_modules[i];
  }
  return nullptr;
}

bool Executable(uintptr_t address, size_t before) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<const void*>(address - before), &mbi, sizeof(mbi)) ||
      mbi.State != MEM_COMMIT) {
    return false;
  }
  const DWORD exec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
  return (mbi.Protect & exec) != 0 && (mbi.Protect & PAGE_GUARD) == 0 && address <= regionEnd;
}

// Whether the instruction ending at `ret` is a CALL (E8 rel32, or FF /2 in any form).
bool AfterCall(uintptr_t ret) {
  if (!Executable(ret, 7)) return false;
  const auto* p = reinterpret_cast<const uint8_t*>(ret);
  if (p[-5] == 0xE8) return true;
  for (int len = 2; len <= 7; ++len) {
    if (p[-len] != 0xFF) continue;
    const uint8_t modrm = p[-len + 1];
    if (((modrm >> 3) & 7) != 2) continue;
    const int mod = modrm >> 6, rm = modrm & 7;
    int expected = 2;
    if (mod == 0) expected = rm == 4 ? ((p[-len + 2] & 7) == 5 ? 7 : 3) : rm == 5 ? 6 : 2;
    if (mod == 1) expected = rm == 4 ? 4 : 3;
    if (mod == 2) expected = rm == 4 ? 7 : 6;
    if (expected == len) return true;
  }
  return false;
}

void Describe(uintptr_t address, char* out, size_t size) {
  if (const Module* m = ModuleAt(address)) {
    char name[64];
    WideCharToMultiByte(CP_UTF8, 0, m->name, -1, name, sizeof(name), nullptr, nullptr);
    wsprintfA(out, "%s+0x%x", name, static_cast<unsigned>(address - m->start));
  } else {
    wsprintfA(out, "%08x", static_cast<unsigned>(address));
  }
  (void)size;
}

void DumpThread(DWORD tid) {
  HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
  if (!thread) return;
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  size_t copied = 0;
  bool haveContext = false;
  if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
    if (GetThreadContext(thread, &ctx)) {
      haveContext = true;
      // Copy from ESP up to the end of its committed region (the stack top), bounded.
      MEMORY_BASIC_INFORMATION mbi{};
      if (VirtualQuery(reinterpret_cast<const void*>(ctx.Esp), &mbi, sizeof(mbi)) && mbi.State == MEM_COMMIT) {
        const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        copied = end > ctx.Esp ? end - ctx.Esp : 0;
        if (copied > kStackBytes) copied = kStackBytes;
        std::memcpy(g_stack, reinterpret_cast<const void*>(ctx.Esp), copied);
      }
    }
    ResumeThread(thread);
  }
  CloseHandle(thread);
  if (!haveContext) return;

  char eip[96];
  Describe(ctx.Eip, eip, sizeof(eip));
  VR_LOG_WARN("hang: thread %5lu eip %s esp %08lx ebp %08lx eax %08lx ecx %08lx edx %08lx", tid, eip, ctx.Esp,
              ctx.Ebp, ctx.Eax, ctx.Ecx, ctx.Edx);
  char line[1024];
  size_t used = 0;
  int frames = 0;
  line[0] = '\0';
  for (size_t off = 0; off + 4 <= copied && frames < 40; off += 4) {
    uint32_t value;
    std::memcpy(&value, g_stack + off, 4);
    if (!ModuleAt(value) || !AfterCall(value)) continue;
    char entry[96];
    Describe(value, entry, sizeof(entry));
    const size_t len = std::strlen(entry);
    if (used + len + 2 >= sizeof(line)) break;
    if (used) line[used++] = ' ';
    std::memcpy(line + used, entry, len + 1);
    used += len;
    ++frames;
  }
  if (used) VR_LOG_WARN("hang: thread %5lu calls: %s", tid, line);
}

void DumpAllThreads() {
  CollectModules();
  VR_LOG_WARN("hang: no Present for %lu ms; thread stacks follow (most recent call first)", g_thresholdMs);
  if (const char* note = g_note.load()) {
    VR_LOG_WARN("hang: thread %lu is in %s", g_noteThread.load(), note);
  } else {
    VR_LOG_WARN("hang: no OpenXR call in progress");
  }
  for (int i = 0; i < g_moduleCount; ++i) {
    char name[64];
    WideCharToMultiByte(CP_UTF8, 0, g_modules[i].name, -1, name, sizeof(name), nullptr, nullptr);
    VR_LOG_DEBUG("hang: module %08x-%08x %s", static_cast<unsigned>(g_modules[i].start),
                 static_cast<unsigned>(g_modules[i].end), name);
  }
  DWORD tids[256];
  int count = 0;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap != INVALID_HANDLE_VALUE) {
    THREADENTRY32 entry{sizeof(entry)};
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &entry); ok && count < 256; ok = Thread32Next(snap, &entry)) {
      if (entry.th32OwnerProcessID == pid && entry.th32ThreadID != g_watchdogThread) tids[count++] = entry.th32ThreadID;
    }
    CloseHandle(snap);
  }
  for (int i = 0; i < count; ++i) DumpThread(tids[i]);
  VR_LOG_WARN("hang: end of dump");
}

DWORD WINAPI WatchdogMain(void*) {
  bool dumped = false;
  for (;;) {
    Sleep(500);
    const uint64_t last = g_lastPresentMs.load();
    if (last == 0) continue;
    const uint64_t idle = GetTickCount64() - last;
    if (idle < g_thresholdMs) {
      if (dumped) VR_LOG_INFO("hang: Presents resumed");
      dumped = false;
      continue;
    }
    if (!dumped) {
      dumped = true;
      DumpAllThreads();
    }
  }
}

}  // namespace

void StartHangWatchdog(unsigned seconds) {
  if (seconds == 0 || g_thresholdMs) return;
  g_thresholdMs = seconds * 1000;
  HANDLE thread = CreateThread(nullptr, 0, &WatchdogMain, nullptr, 0, &g_watchdogThread);
  if (thread) CloseHandle(thread);
}

void NoteHangWatchdogPresent() { g_lastPresentMs.store(GetTickCount64()); }

void SetHangNote(const char* note) {
  g_noteThread.store(GetCurrentThreadId());
  g_note.store(note);
}

}  // namespace vrcore::detail
