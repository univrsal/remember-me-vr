#include "input_capture.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

#include "vr_gamepad.h"
#include "vrcore/hook.h"
#include "vrcore/log.h"
#include "vrcore/memory.h"
#include "vrcore/process.h"

namespace rm::input_capture {
namespace {

int g_toggleKey = 0x75;  // F6
WORD g_padToggle = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;

std::atomic<bool> g_menuOpen{false};
std::atomic<bool> g_freeCam{false};

std::mutex g_mutex;  // toggle state: pad polls (XInput hook) and the frame poll
bool g_keyDown = false;
bool g_comboDown = false;
bool g_suppressCombo = false;  // combo buttons stay hidden from the game until released
std::atomic<bool> g_suppressHeld{false};  // after closing the menu: neutral pad until everything is released

std::atomic<float> g_mouseX{0.0f}, g_mouseY{0.0f}, g_wheel{0.0f};
std::atomic<bool> g_buttons[2]{{false}, {false}};

// --- Viewport client input hooks -----------------------------------------
// FViewportClient vtable: +0x14 InputKey(viewport, controllerId, FName key,
// event, amount, bGamepad), +0x1C InputAxis(viewport, controllerId, FName key,
// delta, deltaTime, bGamepad); both return UBOOL consumed. The client
// interface pointer is at FViewport+0x20 (docs/08, "Gamepad input").
constexpr uint32_t kViewportClient = 0x20;
constexpr int32_t kInputPressed = 0;   // EInputEvent IE_Pressed
constexpr int32_t kInputReleased = 1;  // IE_Released
using InputKeyFn = int(__fastcall*)(void*, void*, void*, int32_t, int32_t, int32_t, int32_t, float, int32_t);
using InputAxisFn = int(__fastcall*)(void*, void*, void*, int32_t, int32_t, int32_t, float, float, int32_t);
InputKeyFn g_origInputKey = nullptr;
InputAxisFn g_origInputAxis = nullptr;
bool g_inputHooksTried = false;
bool g_inputHooked = false;
std::string (*g_nameOf)(int32_t) = nullptr;

enum class Key : uint8_t { Other, MouseX, MouseY, LeftMouse, RightMouse, WheelUp, WheelDown };
std::unordered_map<int32_t, Key> g_keyNames;  // game thread

Key KeyOf(int32_t name) {
  const auto it = g_keyNames.find(name);
  if (it != g_keyNames.end()) return it->second;
  const std::string text = g_nameOf ? g_nameOf(name) : std::string();
  if (text.empty()) return Key::Other;  // reflection not mapped yet: ask again later
  Key key = Key::Other;
  if (text == "MouseX") key = Key::MouseX;
  else if (text == "MouseY") key = Key::MouseY;
  else if (text == "LeftMouseButton") key = Key::LeftMouse;
  else if (text == "RightMouseButton") key = Key::RightMouse;
  else if (text == "MouseScrollUp") key = Key::WheelUp;
  else if (text == "MouseScrollDown") key = Key::WheelDown;
  g_keyNames.emplace(name, key);
  return key;
}

void Add(std::atomic<float>& a, float v) { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); }

int __fastcall HookInputKey(void* self, void* edx, void* viewport, int32_t controllerId, int32_t name, int32_t number,
                            int32_t event, float amount, int32_t gamepad) {
  if (!Capturing()) return g_origInputKey(self, edx, viewport, controllerId, name, number, event, amount, gamepad);
  if (!gamepad && (event == kInputPressed || event == kInputReleased)) {
    switch (KeyOf(name)) {
      case Key::LeftMouse: g_buttons[0] = event == kInputPressed; break;
      case Key::RightMouse: g_buttons[1] = event == kInputPressed; break;
      case Key::WheelUp: if (event == kInputPressed) Add(g_wheel, 1.0f); break;
      case Key::WheelDown: if (event == kInputPressed) Add(g_wheel, -1.0f); break;
      default: break;
    }
  }
  // Releases still go through, so keys held when capturing started don't stick.
  if (event == kInputReleased) {
    return g_origInputKey(self, edx, viewport, controllerId, name, number, event, amount, gamepad);
  }
  return 1;
}

int __fastcall HookInputAxis(void* self, void* edx, void* viewport, int32_t controllerId, int32_t name, int32_t number,
                             float delta, float deltaTime, int32_t gamepad) {
  if (!Capturing()) {
    return g_origInputAxis(self, edx, viewport, controllerId, name, number, delta, deltaTime, gamepad);
  }
  if (!gamepad) {
    const Key key = KeyOf(name);
    if (key == Key::MouseX) Add(g_mouseX, delta);
    if (key == Key::MouseY) Add(g_mouseY, delta);
  }
  return 1;
}

// Pad combo edge detection; called with every pad state seen (game polls and
// the frame poll). Caller holds g_mutex.
void PadButtons(WORD buttons) {
  if (!g_padToggle) return;
  const bool combo = (buttons & g_padToggle) == g_padToggle;
  if (combo && !g_comboDown) {
    SetMenuOpen(!g_menuOpen.load());
    g_suppressCombo = true;
  }
  g_comboDown = combo;
  if (g_suppressCombo && (buttons & g_padToggle) == 0) g_suppressCombo = false;
}

}  // namespace

void Configure(const vrcore::Config& config) {
  // The toggle used to be the free camera's ([FreeCam] ToggleKey / PadToggle).
  g_toggleKey = config.GetInt("Menu", "ToggleKey", config.GetInt("FreeCam", "ToggleKey", g_toggleKey));
  g_padToggle =
      static_cast<WORD>(config.GetInt("Menu", "PadToggle", config.GetInt("FreeCam", "PadToggle", g_padToggle)));
  VR_LOG_INFO("menu toggle: key 0x%02x, pad combo 0x%04x", g_toggleKey, g_padToggle);
  // Testing (e.g. the smoke test): the menu starts open.
  if (config.GetBool("Debug", "OpenMenuAtStart", false)) g_menuOpen = true;
}

void SetNameLookup(std::string (*nameOf)(int32_t index)) { g_nameOf = nameOf; }

void InstallInputHooks(void* viewport) {
  if (g_inputHooksTried || !viewport) return;
  g_inputHooksTried = true;
  uintptr_t client = 0, vtable = 0;
  vrcore::mem::Read(reinterpret_cast<uintptr_t>(viewport) + kViewportClient, client);
  if (client) vrcore::mem::Read(client, vtable);
  const auto& exe = vrcore::Exe();
  const uintptr_t base = reinterpret_cast<uintptr_t>(exe.base);
  if (!vtable || vtable < base || vtable >= base + exe.imageSize) {
    VR_LOG_WARN("input capture: no viewport client at viewport %p (+0x20 = %p); the game keeps getting keyboard/mouse",
                viewport, reinterpret_cast<void*>(client));
    return;
  }
  g_inputHooked = vrcore::hook::InstallVTable("ViewportClient::InputKey", reinterpret_cast<void*>(client), 0x14 / 4,
                                              &HookInputKey, &g_origInputKey) &&
                  vrcore::hook::InstallVTable("ViewportClient::InputAxis", reinterpret_cast<void*>(client), 0x1C / 4,
                                              &HookInputAxis, &g_origInputAxis);
  VR_LOG_INFO("input capture: viewport client %p input hooks %s", reinterpret_cast<void*>(client),
              g_inputHooked ? "installed" : "FAILED");
}

bool InputHooked() { return g_inputHooked; }

void Poll() {
  if (g_toggleKey) {
    const bool down = GameWindowFocused() && (GetAsyncKeyState(g_toggleKey) & 0x8000) != 0;
    if (down && !g_keyDown) SetMenuOpen(!g_menuOpen.load());
    g_keyDown = down;
  }
  XINPUT_GAMEPAD pad{};
  if (vr_gamepad::ReadPad(pad)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    PadButtons(pad.wButtons);
  }
}

bool MenuOpen() { return g_menuOpen.load(std::memory_order_relaxed); }

void SetMenuOpen(bool open) {
  if (g_menuOpen.exchange(open) == open) return;
  ClearMouse();
  // Whatever closed it (Start, the trigger on "Close", ...) may still be held.
  if (!open) g_suppressHeld = true;
  VR_LOG_INFO("menu %s", open ? "opened" : "closed");
}

void SetFreeCam(bool active) {
  if (g_freeCam.exchange(active) != active) ClearMouse();
}

bool Capturing() { return g_menuOpen.load(std::memory_order_relaxed) || g_freeCam.load(std::memory_order_relaxed); }

bool SwallowPad(const XINPUT_GAMEPAD& pad) {
  std::lock_guard<std::mutex> lock(g_mutex);
  PadButtons(pad.wButtons);
  if (g_suppressHeld && pad.wButtons == 0 && pad.bLeftTrigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD &&
      pad.bRightTrigger <= XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
    g_suppressHeld = false;
  }
  return Capturing() || g_suppressCombo || g_suppressHeld;
}

float TakeMouseX() { return g_mouseX.exchange(0.0f); }
float TakeMouseY() { return g_mouseY.exchange(0.0f); }
float TakeWheel() { return g_wheel.exchange(0.0f); }
bool MouseButton(int index) { return index >= 0 && index < 2 && g_buttons[index].load(); }

void ClearMouse() {
  g_mouseX = 0.0f;
  g_mouseY = 0.0f;
  g_wheel = 0.0f;
  g_buttons[0] = false;
  g_buttons[1] = false;
}

bool GameWindowFocused() {
  DWORD pid = 0;
  const HWND fg = GetForegroundWindow();
  return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

int ToggleKey() { return g_toggleKey; }
WORD PadToggle() { return g_padToggle; }

}  // namespace rm::input_capture
