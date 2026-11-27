#include "startup.h"

#include <windows.h>

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

}  // namespace

void Configure(const vrcore::Config& config) {
  if (!config.GetBool("Game", "SkipIntroMovies", true)) return;
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

}  // namespace rm::startup
