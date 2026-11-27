#include "startup.h"

#include <windows.h>

#include <cctype>
#include <cstring>
#include <string>

#include "vrcore/hook.h"
#include "vrcore/log.h"

namespace rm::startup {
namespace {

constexpr wchar_t kSwitch[] = L" -nostartupmovies";

using GetCommandLineW_t = LPWSTR(WINAPI*)();
using GetCommandLineA_t = LPSTR(WINAPI*)();
GetCommandLineW_t g_origW = nullptr;
GetCommandLineA_t g_origA = nullptr;
std::wstring g_lineW;
std::string g_lineA;

// Built in DllMain (loader lock: no other threads run the exe's code yet).
void Build() {
  g_lineW = g_origW();
  g_lineA = g_origA();
  if (g_lineW.find(L"nostartupmovies") == std::wstring::npos) g_lineW += kSwitch;
  if (g_lineA.find("nostartupmovies") == std::string::npos) g_lineA += " -nostartupmovies";
}

LPWSTR WINAPI HookW() { return g_lineW.data(); }
LPSTR WINAPI HookA() { return g_lineA.data(); }

// --- Boot logos ------------------------------------------------------------
// DONTNOD's movie player (ExampleGame.DNEMoviePlayer) plays the Capcom /
// Unreal / DONTNOD / NVIDIA logos from script, not as engine startup movies.
// They're refused at BinkOpen (binkw32.dll, `_BinkOpen@8`: name or, with
// BINKFILEHANDLE, a file handle; flags), so the player moves on.
using BinkOpen_t = void*(WINAPI*)(const char*, unsigned);
BinkOpen_t g_origBinkOpen = nullptr;
constexpr unsigned kBinkFileHandle = 0x00800000;
int g_binkLogs = 0;

std::string MovieName(const char* name, unsigned flags) {
  if (!name) return {};
  if (flags & kBinkFileHandle) {
    wchar_t path[MAX_PATH] = L"";
    const DWORD n = GetFinalPathNameByHandleW(reinterpret_cast<HANDLE>(const_cast<char*>(name)), path, MAX_PATH, 0);
    std::string out;
    for (DWORD i = 0; i < n && i < MAX_PATH; ++i) out.push_back(static_cast<char>(path[i]));
    return out;
  }
  return name;
}

bool IsBootLogo(const std::string& path) {
  const size_t slash = path.find_last_of("\\/");
  std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
  for (char& c : file) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return file.rfind("bik_boot_", 0) == 0 || file.rfind("startupmovie", 0) == 0;
}

void* WINAPI HookBinkOpen(const char* name, unsigned flags) {
  const std::string movie = MovieName(name, flags);
  const bool skip = IsBootLogo(movie);
  if (g_binkLogs < 40) {
    ++g_binkLogs;
    VR_LOG_INFO("movie: %s%s", movie.empty() ? "(unknown)" : movie.c_str(), skip ? " -> skipped (boot logo)" : "");
  }
  if (skip) return nullptr;  // as if the file couldn't be opened
  return g_origBinkOpen(name, flags);
}

bool HookBink() {
  HMODULE bink = GetModuleHandleW(L"binkw32.dll");
  void* open = bink ? reinterpret_cast<void*>(GetProcAddress(bink, "_BinkOpen@8")) : nullptr;
  if (!open) return false;
  const bool ok = vrcore::hook::Install("BinkOpen", open, reinterpret_cast<void*>(&HookBinkOpen),
                                        reinterpret_cast<void**>(&g_origBinkOpen));
  VR_LOG_INFO("intro logos: %s", ok ? "skipped (BinkOpen refuses BIK_Boot_* / StartUpMovie)" : "hook FAILED");
  return true;
}

bool g_skip = false;
bool g_binkTried = false;

}  // namespace

void Configure(const vrcore::Config& config) {
  if (!config.GetBool("Game", "SkipIntroMovies", true)) return;
  g_skip = true;
  // binkw32 is one of the exe's imports, normally mapped by now; else later (InstallLate).
  g_binkTried = HookBink();
  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  auto w = kernel32 ? reinterpret_cast<void*>(GetProcAddress(kernel32, "GetCommandLineW")) : nullptr;
  auto a = kernel32 ? reinterpret_cast<void*>(GetProcAddress(kernel32, "GetCommandLineA")) : nullptr;
  if (!w || !a) return;
  g_origW = reinterpret_cast<GetCommandLineW_t>(w);
  g_origA = reinterpret_cast<GetCommandLineA_t>(a);
  Build();
  const bool ok = vrcore::hook::Install("GetCommandLineW", w, reinterpret_cast<void*>(&HookW),
                                        reinterpret_cast<void**>(&g_origW)) &&
                  vrcore::hook::Install("GetCommandLineA", a, reinterpret_cast<void*>(&HookA),
                                        reinterpret_cast<void**>(&g_origA));
  VR_LOG_INFO("intro movies: %s", ok ? "skipped (-nostartupmovies)" : "hook FAILED, playing");
}

void InstallLate() {
  if (!g_skip || g_binkTried) return;
  g_binkTried = true;
  if (!HookBink()) VR_LOG_WARN("intro logos: binkw32.dll not loaded; logos play");
}

}  // namespace rm::startup
