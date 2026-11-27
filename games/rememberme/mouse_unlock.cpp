#include "mouse_unlock.h"

#include <windows.h>

#include <atomic>

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
constexpr uint32_t kControllerPawn = 0x264;  // Controller.Pawn

using SetCursorVisibleFn = void(__fastcall*)(void*, void*, int32_t);
using UpdateMouseLockFn = void(__fastcall*)(void*, void*);
using UpdateMouseCursorFn = int32_t(__fastcall*)(void*, void*, int32_t);
SetCursorVisibleFn g_origVisible = nullptr;
UpdateMouseLockFn g_origLock = nullptr;
UpdateMouseCursorFn g_origCursor = nullptr;

std::atomic<bool> g_enabled{true};
bool g_installed = false;
int32_t g_menuOffset = -1;
uint32_t g_menuMask = 0;
std::atomic<bool> g_unlocked{false};
int32_t g_gameVisible = 0;  // what the game last asked for (in game: hidden)

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

void __fastcall HookVisible(void* self, void* edx, int32_t visible) {
  g_gameVisible = visible;
  g_origVisible(self, edx, g_unlocked.load(std::memory_order_relaxed) ? 1 : visible);
}

void __fastcall HookLock(void* self, void* edx) {
  if (g_unlocked.load(std::memory_order_relaxed)) {
    ReleaseClip();
    return;
  }
  g_origLock(self, edx);
}

int32_t __fastcall HookCursor(void* self, void* edx, int32_t set) {
  if (!g_unlocked.load(std::memory_order_relaxed)) return g_origCursor(self, edx, set);
  if (set) SetCursor(LoadCursorW(nullptr, IDC_ARROW));
  return 1;
}

// The game shows a menu: the pause / Sensen menu flag, or no pawn (main menu).
bool InMenu(void* localPlayer) {
  uintptr_t controller = 0, pawn = 0;
  if (!vrcore::mem::Read(reinterpret_cast<uintptr_t>(localPlayer) + kLocalPlayerActor, controller) || !controller) {
    return false;
  }
  uint32_t word = 0;
  if (g_menuOffset >= 0 && vrcore::mem::Read(controller + static_cast<uint32_t>(g_menuOffset), word) &&
      (word & g_menuMask) != 0) {
    return true;
  }
  return vrcore::mem::Read(controller + kControllerPawn, pawn) && pawn == 0;
}

}  // namespace

void Configure(const vrcore::Config& config) { g_enabled = config.GetBool("Input", "FreeMouseInMenus", true); }

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
  if (!ok) g_origCursor = nullptr;  // Update stays off
  VR_LOG_INFO("mouse unlock %s (cursor free in menus)", ok ? "installed" : "FAILED");
}

void Save(const vrcore::Config& config) { config.SetBool("Input", "FreeMouseInMenus", g_enabled); }
bool Enabled() { return g_enabled; }
void SetEnabled(bool enabled) { g_enabled = enabled; }
bool Available() { return g_origCursor != nullptr; }

void SetMenuFlag(int32_t offset, uint32_t mask) {
  g_menuOffset = offset;
  g_menuMask = mask;
}

void Update(void* localPlayer, void* viewport) {
  if (!g_origCursor || !viewport) return;
  const bool want = g_enabled.load() && InMenu(localPlayer) && !input_capture::Capturing();
  if (want == g_unlocked.load()) return;
  g_unlocked = want;
  void* const windowsViewport = static_cast<char*>(viewport) - 4;
  // Apply now (the game only updates these on its own changes).
  if (want) {
    g_origVisible(viewport, nullptr, 1);
    ReleaseClip();
    g_origCursor(viewport, nullptr, 1);
  } else {
    g_origVisible(viewport, nullptr, g_gameVisible);
    g_origLock(windowsViewport, nullptr);
    g_origCursor(viewport, nullptr, 1);
  }
  VR_LOG_INFO("mouse %s", want ? "freed (game menu)" : "back to the game");
}

}  // namespace rm::mouse_unlock
