#include "mouse_unlock.h"

#include <windows.h>

#include <atomic>
#include <iterator>

#include "engine.h"
#include "input_capture.h"
#include "vrcore/hook.h"
#include "vrcore/log.h"
#include "vrcore/memory.h"
#include "vrcore/process.h"

namespace rm::mouse_unlock {
namespace {

constexpr uint32_t kSetCursorVisible = 0x00C73E60;
constexpr uint8_t kSetCursorVisiblePrologue[] = {0x56, 0x8B, 0xF1, 0x8B, 0x06, 0x8B, 0x50, 0x3C, 0x57, 0xFF, 0xD2};
constexpr uint32_t kUpdateMouseLock = 0x00C73F60;
// Bytes 4..7 are the (relocatable) security cookie address; check around them.
constexpr uint8_t kUpdateMouseLockPrologueA[] = {0x83, 0xEC, 0x28, 0xA1};
constexpr uint8_t kUpdateMouseLockPrologueB[] = {0x33, 0xC4, 0x89, 0x44};
constexpr uint32_t kUpdateMouseCursor = 0x00C741E0;
constexpr uint8_t kUpdateMouseCursorPrologue[] = {0x53, 0x55, 0x56, 0x8B, 0xF1, 0x8B, 0x06, 0x8B, 0x90, 0xAC};
constexpr uint32_t kControllerPawn = 0x264;     // Controller.Pawn
constexpr uint32_t kActorWorldInfo = 0xD8;      // Actor.WorldInfo
constexpr uint32_t kWorldInfoPauser = 0x498;    // WorldInfo.Pauser (non-null while paused)
constexpr uint64_t kNoWorldMs = 300;            // no world frame for this long: a menu/video/loading screen

using SetCursorVisibleFn = void(__fastcall*)(void*, void*, int32_t);
using UpdateMouseLockFn = void(__fastcall*)(void*, void*);
using UpdateMouseCursorFn = int32_t(__fastcall*)(void*, void*, int32_t);
SetCursorVisibleFn g_origVisible = nullptr;
UpdateMouseLockFn g_origLock = nullptr;
UpdateMouseCursorFn g_origCursor = nullptr;

std::atomic<bool> g_enabled{true};
bool g_installed = false;
struct Flag {
  int32_t offset;
  uint32_t mask;
};
Flag g_flags[4];  // player controller bools meaning "a menu is up" (bMenuPause, bOnMainMenu)
int g_flagCount = 0;

// Game thread (CalcSceneView and the window/viewport code both run there).
bool g_worldMenu = false;       // the world frame says: pause/Sensen menu, paused, or no pawn
uint64_t g_lastWorldMs = 0;     // last main CalcSceneView (0: none yet, e.g. the main menu)
bool g_applied = false;         // cursor currently freed by us
int32_t g_gameVisible = 0;      // what the game last asked for (in game: hidden)
unsigned g_lastSignals = ~0u;

void ReleaseClip() {
  RECT clip{};
  const RECT screen{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                    GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN),
                    GetSystemMetrics(SM_YVIRTUALSCREEN) + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
  if (GetClipCursor(&clip) && (clip.left > screen.left || clip.top > screen.top || clip.right < screen.right ||
                               clip.bottom < screen.bottom)) {
    ClipCursor(nullptr);
  }
}

bool Want() {
  if (!g_enabled.load(std::memory_order_relaxed) || input_capture::Capturing()) return false;
  return g_worldMenu || g_lastWorldMs == 0 || GetTickCount64() - g_lastWorldMs > kNoWorldMs;
}

// Brings the cursor in line with Want(). `viewport` is the FViewport
// (UWindowsViewport + 4). Game thread.
void Apply(void* viewport) {
  if (!g_origCursor) return;  // still installing (the hooks go live one by one)
  const bool want = Want();
  if (want == g_applied) {
    if (want) ReleaseClip();
    return;
  }
  g_applied = want;
  if (want) {
    g_origVisible(viewport, nullptr, 1);
    ReleaseClip();
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
  } else {
    g_origVisible(viewport, nullptr, g_gameVisible);
    g_origLock(static_cast<char*>(viewport) - 4, nullptr);
    g_origCursor(viewport, nullptr, 1);
  }
  VR_LOG_INFO("mouse %s", want ? "freed (game menu / no world on screen)" : "back to the game");
}

void __fastcall HookVisible(void* self, void* edx, int32_t visible) {
  g_gameVisible = visible;
  Apply(self);
  g_origVisible(self, edx, g_applied ? 1 : visible);
}

void __fastcall HookLock(void* self, void* edx) {
  Apply(static_cast<char*>(self) + 4);
  if (g_applied) return;
  g_origLock(self, edx);
}

int32_t __fastcall HookCursor(void* self, void* edx, int32_t set) {
  Apply(self);
  if (!g_applied) return g_origCursor(self, edx, set);
  if (set) SetCursor(LoadCursorW(nullptr, IDC_ARROW));
  return 1;
}

// The world frame's view of things: pause/Sensen menu flag, world paused, or
// no pawn (main menu, between levels). Logs changes.
bool WorldMenu(void* localPlayer) {
  uintptr_t controller = 0;
  if (!vrcore::mem::Read(reinterpret_cast<uintptr_t>(localPlayer) + kLocalPlayerActor, controller) || !controller) {
    return false;
  }
  uintptr_t pawn = 1, worldInfo = 0, pauser = 0;
  bool menuFlag = false;
  for (int i = 0; i < g_flagCount; ++i) {
    uint32_t word = 0;
    if (vrcore::mem::Read(controller + static_cast<uint32_t>(g_flags[i].offset), word) && (word & g_flags[i].mask)) {
      menuFlag = true;
    }
  }
  vrcore::mem::Read(controller + kControllerPawn, pawn);
  if (vrcore::mem::Read(controller + kActorWorldInfo, worldInfo) && worldInfo) {
    vrcore::mem::Read(worldInfo + kWorldInfoPauser, pauser);
  }
  const unsigned signals = (menuFlag ? 1u : 0u) | (pauser ? 2u : 0u) | (pawn == 0 ? 4u : 0u);
  if (signals != g_lastSignals) {
    g_lastSignals = signals;
    VR_LOG_INFO("mouse unlock: menu flag %d, paused %d, no pawn %d", menuFlag ? 1 : 0, pauser ? 1 : 0,
                pawn == 0 ? 1 : 0);
  }
  return signals != 0;
}

}  // namespace

void Configure(const vrcore::Config& config) { g_enabled = config.GetBool("Input", "FreeMouseInMenus", true); }
void Save(const vrcore::Config& config) { config.SetBool("Input", "FreeMouseInMenus", g_enabled); }
bool Enabled() { return g_enabled; }
void SetEnabled(bool enabled) { g_enabled = enabled; }
bool Available() { return g_origCursor != nullptr; }

void Install() {
  if (g_installed) return;
  g_installed = true;
  const auto& exe = vrcore::Exe();
  auto* visible = exe.FromPreferredVA<uint8_t>(kSetCursorVisible);
  auto* lock = exe.FromPreferredVA<uint8_t>(kUpdateMouseLock);
  auto* cursor = exe.FromPreferredVA<uint8_t>(kUpdateMouseCursor);
  const auto at = [](const uint8_t* p) { return reinterpret_cast<uintptr_t>(p); };
  if (!vrcore::mem::Matches(at(visible), kSetCursorVisiblePrologue, sizeof(kSetCursorVisiblePrologue)) ||
      !vrcore::mem::Matches(at(lock), kUpdateMouseLockPrologueA, sizeof(kUpdateMouseLockPrologueA)) ||
      !vrcore::mem::Matches(at(lock) + 8, kUpdateMouseLockPrologueB, sizeof(kUpdateMouseLockPrologueB)) ||
      !vrcore::mem::Matches(at(cursor), kUpdateMouseCursorPrologue, sizeof(kUpdateMouseCursorPrologue))) {
    VR_LOG_WARN("mouse unlock: cursor code doesn't match this build; off");
    return;
  }
  const bool ok = vrcore::hook::Install("SetCursorVisible", visible, reinterpret_cast<void*>(&HookVisible),
                                        reinterpret_cast<void**>(&g_origVisible)) &&
                  vrcore::hook::Install("UpdateMouseLock", lock, reinterpret_cast<void*>(&HookLock),
                                        reinterpret_cast<void**>(&g_origLock)) &&
                  vrcore::hook::Install("UpdateMouseCursor", cursor, reinterpret_cast<void*>(&HookCursor),
                                        reinterpret_cast<void**>(&g_origCursor));
  if (!ok) g_origCursor = nullptr;
  VR_LOG_INFO("mouse unlock %s (cursor free in menus)", ok ? "installed" : "FAILED");
}

void AddMenuFlag(int32_t offset, uint32_t mask) {
  if (g_flagCount < static_cast<int>(std::size(g_flags))) g_flags[g_flagCount++] = {offset, mask};
}

void Update(void* localPlayer, void* viewport) {
  if (!g_origCursor || !viewport) return;
  g_lastWorldMs = GetTickCount64();
  g_worldMenu = WorldMenu(localPlayer);
  Apply(viewport);
}

}  // namespace rm::mouse_unlock
