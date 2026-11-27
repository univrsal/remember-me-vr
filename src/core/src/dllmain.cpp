#include <windows.h>

#include "internal.h"
#include "vrcore/config.h"
#include "vrcore/game.h"
#include "vrcore/hook.h"
#include "vrcore/log.h"
#include "vrcore/process.h"

namespace vrcore::detail {
namespace {

bool g_active = false;

}  // namespace

// Runs under the loader lock: no thread waits, no COM, nothing that could need
// another DLL's DllMain. Hooks are installed here; everything that touches
// game code waits for Game::OnDeviceCreated (SteamStub has decrypted .text by then).
void Attach(HMODULE self) {
  InitProcessInfo(self);
  Game& game = GetGame();
  const GameInfo info = game.Info();

  const std::wstring base = ModDirectory() + info.modName;
  const Config config(base + L".ini");
  if (!config.GetBool("Core", "Enabled", true)) return;  // pure proxy, no log, no hooks

  log::Open((base + L".log").c_str());
  log::SetLevel(log::ParseLevel(config.GetString("Log", "Level", "info").c_str(), log::Level::Info));

  const ExeInfo& exe = Exe();
  VR_LOG_INFO("%s VR mod, built " __DATE__ " " __TIME__, info.name);
  VR_LOG_INFO("config: %s (%s)", Narrow(config.Path()).c_str(), config.Exists() ? "found" : "missing, using defaults");
  VR_LOG_INFO("host: %s base=%p preferred=0x%08x size=0x%08x timestamp=0x%08x", Narrow(exe.path).c_str(),
              static_cast<void*>(exe.base), exe.preferredBase, exe.imageSize, exe.timestamp);

  if (lstrcmpiW(exe.fileName.c_str(), info.exeName) != 0) {
    VR_LOG_WARN("host is not %s; staying inactive (proxy only)", Narrow(info.exeName).c_str());
    return;
  }
  const bool knownBuild = info.knownTimestamp == 0 || exe.timestamp == info.knownTimestamp;
  if (!knownBuild) {
    VR_LOG_WARN("unknown game build (timestamp 0x%08x, expected 0x%08x): game-code hooks may not match",
                exe.timestamp, info.knownTimestamp);
  }

  if (!hook::Init()) return;
  if (!InstallD3D9Hooks()) VR_LOG_ERROR("D3D9 hooks not installed; mod inactive");
  g_active = true;
  game.OnAttach(config, knownBuild);
}

void Detach() {
  if (!g_active) return;
  GetGame().OnDetach();
  hook::Shutdown();
  VR_LOG_INFO("detached");
}

}  // namespace vrcore::detail

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      DisableThreadLibraryCalls(instance);
      vrcore::detail::Attach(instance);
      break;
    case DLL_PROCESS_DETACH:
      // reserved != nullptr: the process is exiting; other DLLs may already be gone.
      if (!reserved) vrcore::detail::Detach();
      break;
  }
  return TRUE;
}
