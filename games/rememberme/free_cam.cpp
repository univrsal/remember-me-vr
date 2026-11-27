#include "free_cam.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>

#include "vr_gamepad.h"
#include "vrcore/hook.h"
#include "vrcore/log.h"
#include "vrcore/memory.h"
#include "vrcore/process.h"

namespace rm::free_cam {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kDegToUnits = 65536.0f / 360.0f;

struct Settings {
  bool enabled = true;
  int toggleKey = 0x75;  // F6
  WORD padToggle = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
  float speed = 600.0f;  // uu/s
  float fastMultiplier = 4.0f;
  float slowMultiplier = 0.25f;
  float lookSpeed = 120.0f;         // deg/s at full stick
  float mouseSensitivity = 0.1f;    // deg per mouse count
  bool invertMouseY = false;
  float deadzone = 0.2f;
};
Settings s;

std::atomic<bool> g_active{false};
std::mutex g_mutex;  // toggle state: pad poll and frame update

// Free camera pose (game thread only).
bool g_initialized = false;  // pose taken from the game camera on the first Apply after toggling on
FVector g_pos{};
float g_yaw = 0.0f, g_pitch = 0.0f;  // degrees
LARGE_INTEGER g_lastTick{};
bool g_keyDown = false;
bool g_comboDown = false;
bool g_suppressCombo = false;  // combo buttons stay hidden from the game until released

// Mouse look, accumulated by the InputAxis hook (game thread).
std::atomic<float> g_mouseX{0.0f}, g_mouseY{0.0f};

// --- Viewport client input hooks -----------------------------------------
// FViewportClient vtable: +0x14 InputKey(viewport, controllerId, FName key,
// event, amount, bGamepad), +0x1C InputAxis(viewport, controllerId, FName key,
// delta, deltaTime, bGamepad); both return UBOOL consumed. The client
// interface pointer is at FViewport+0x20 (docs/08, "Gamepad input").
constexpr uint32_t kViewportClient = 0x20;
constexpr int32_t kInputReleased = 1;  // EInputEvent IE_Released
using InputKeyFn = int(__fastcall*)(void*, void*, void*, int32_t, int32_t, int32_t, int32_t, float, int32_t);
using InputAxisFn = int(__fastcall*)(void*, void*, void*, int32_t, int32_t, int32_t, float, float, int32_t);
InputKeyFn g_origInputKey = nullptr;
InputAxisFn g_origInputAxis = nullptr;
bool g_inputHooksTried = false;
bool g_inputHooked = false;
std::string (*g_nameOf)(int32_t) = nullptr;

enum class Axis : uint8_t { Other, MouseX, MouseY };
std::unordered_map<int32_t, Axis> g_axisNames;

Axis AxisOf(int32_t name) {
  const auto it = g_axisNames.find(name);
  if (it != g_axisNames.end()) return it->second;
  const std::string text = g_nameOf ? g_nameOf(name) : std::string();
  if (text.empty()) return Axis::Other;  // reflection not mapped yet: ask again later
  const Axis axis = text == "MouseX" ? Axis::MouseX : text == "MouseY" ? Axis::MouseY : Axis::Other;
  g_axisNames.emplace(name, axis);
  return axis;
}

int __fastcall HookInputKey(void* self, void* edx, void* viewport, int32_t controllerId, int32_t name, int32_t number,
                            int32_t event, float amount, int32_t gamepad) {
  // Releases still go through, so keys held when the free camera started don't stick.
  if (g_active.load(std::memory_order_relaxed) && event != kInputReleased) return 1;
  return g_origInputKey(self, edx, viewport, controllerId, name, number, event, amount, gamepad);
}

int __fastcall HookInputAxis(void* self, void* edx, void* viewport, int32_t controllerId, int32_t name, int32_t number,
                             float delta, float deltaTime, int32_t gamepad) {
  if (!g_active.load(std::memory_order_relaxed)) {
    return g_origInputAxis(self, edx, viewport, controllerId, name, number, delta, deltaTime, gamepad);
  }
  if (!gamepad) {
    switch (AxisOf(name)) {
      case Axis::MouseX:
        g_mouseX.store(g_mouseX.load() + delta);
        break;
      case Axis::MouseY:
        g_mouseY.store(g_mouseY.load() + delta);
        break;
      default:
        break;
    }
  }
  return 1;
}

// --- Toggle ----------------------------------------------------------------
void Toggle(const char* how) {
  const bool now = !g_active.load();
  if (now) g_initialized = false;  // start where the game camera is
  g_active = now;
  g_mouseX = 0.0f;
  g_mouseY = 0.0f;
  VR_LOG_INFO("free camera %s (%s)%s", now ? "ON" : "OFF", how,
              now && !g_inputHooked ? "; viewport input hooks missing, the player still gets input" : "");
}

// Pad combo edge detection; called with every pad state seen (game polls and
// the frame update). Caller holds g_mutex.
void PadButtons(WORD buttons) {
  if (!s.padToggle) return;
  const bool combo = (buttons & s.padToggle) == s.padToggle;
  if (combo && !g_comboDown) {
    Toggle("pad");
    g_suppressCombo = true;
  }
  g_comboDown = combo;
  if (g_suppressCombo && (buttons & s.padToggle) == 0) g_suppressCombo = false;
}

bool GameWindowFocused() {
  DWORD pid = 0;
  const HWND fg = GetForegroundWindow();
  return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

float Stick(SHORT v) {
  const float f = std::clamp(v / 32767.0f, -1.0f, 1.0f);
  if (std::fabs(f) < s.deadzone) return 0.0f;
  // Rescale so movement starts at 0 just outside the deadzone.
  return (f - std::copysign(s.deadzone, f)) / (1.0f - s.deadzone);
}

float Trigger(BYTE v) { return v > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? v / 255.0f : 0.0f; }

float Normalize(int32_t units) {
  // FRotator components may be outside -32768..32767 (e.g. 65000 = looking down slightly).
  return static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(units & 0xFFFF))) / kDegToUnits;
}

}  // namespace

void Configure(const vrcore::Config& config) {
  s.enabled = config.GetBool("FreeCam", "Enabled", s.enabled);
  s.toggleKey = config.GetInt("FreeCam", "ToggleKey", s.toggleKey);
  s.padToggle = static_cast<WORD>(config.GetInt("FreeCam", "PadToggle", s.padToggle));
  s.speed = config.GetFloat("FreeCam", "Speed", s.speed);
  s.fastMultiplier = config.GetFloat("FreeCam", "FastMultiplier", s.fastMultiplier);
  s.slowMultiplier = config.GetFloat("FreeCam", "SlowMultiplier", s.slowMultiplier);
  s.lookSpeed = config.GetFloat("FreeCam", "LookSpeed", s.lookSpeed);
  s.mouseSensitivity = config.GetFloat("FreeCam", "MouseSensitivity", s.mouseSensitivity);
  s.invertMouseY = config.GetBool("FreeCam", "InvertMouseY", s.invertMouseY);
  s.deadzone = std::clamp(config.GetFloat("FreeCam", "Deadzone", s.deadzone), 0.0f, 0.9f);
  if (s.enabled) {
    VR_LOG_INFO("free camera: toggle key 0x%02x, pad combo 0x%04x, %.0f uu/s", s.toggleKey, s.padToggle, s.speed);
  }
}

bool Enabled() { return s.enabled; }
bool Active() { return g_active.load(std::memory_order_relaxed); }

void SetNameLookup(std::string (*nameOf)(int32_t index)) { g_nameOf = nameOf; }

bool SwallowPad(const XINPUT_GAMEPAD& pad) {
  if (!s.enabled) return false;
  std::lock_guard<std::mutex> lock(g_mutex);
  PadButtons(pad.wButtons);
  return g_active.load() || g_suppressCombo;
}

void Update(bool vr, const FRotator* head) {
  if (!s.enabled) return;
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  const float dt = g_lastTick.QuadPart
                       ? std::clamp(static_cast<float>(now.QuadPart - g_lastTick.QuadPart) / freq.QuadPart, 0.0f, 0.1f)
                       : 0.0f;
  g_lastTick = now;

  const bool focused = GameWindowFocused();
  if (s.toggleKey) {
    const bool down = focused && KeyDown(s.toggleKey);
    if (down && !g_keyDown) {
      std::lock_guard<std::mutex> lock(g_mutex);
      Toggle("key");
    }
    g_keyDown = down;
  }
  XINPUT_GAMEPAD pad{};
  const bool havePad = vr_gamepad::ReadPad(pad);
  if (havePad) {
    std::lock_guard<std::mutex> lock(g_mutex);
    PadButtons(pad.wButtons);
  }
  if (!g_active.load() || !g_initialized) return;

  // Inputs: x right, y forward, z up (-1..1), look in degrees.
  float mx = 0, my = 0, mz = 0, lookYaw = 0, lookPitch = 0, speed = s.speed;
  if (focused) {
    my += (KeyDown('W') ? 1.0f : 0.0f) - (KeyDown('S') ? 1.0f : 0.0f);
    mx += (KeyDown('D') ? 1.0f : 0.0f) - (KeyDown('A') ? 1.0f : 0.0f);
    mz += (KeyDown('E') || KeyDown(VK_SPACE) ? 1.0f : 0.0f) - (KeyDown('Q') ? 1.0f : 0.0f);
    if (KeyDown(VK_SHIFT)) speed *= s.fastMultiplier;
    if (KeyDown(VK_CONTROL)) speed *= s.slowMultiplier;
  }
  lookYaw += g_mouseX.exchange(0.0f) * s.mouseSensitivity;
  lookPitch += g_mouseY.exchange(0.0f) * s.mouseSensitivity * (s.invertMouseY ? -1.0f : 1.0f);
  if (havePad) {
    mx += Stick(pad.sThumbLX);
    my += Stick(pad.sThumbLY);
    mz += Trigger(pad.bRightTrigger) - Trigger(pad.bLeftTrigger);
    lookYaw += Stick(pad.sThumbRX) * s.lookSpeed * dt;
    if (vr) {
      mz += Stick(pad.sThumbRY);  // the head does the pitch
    } else {
      lookPitch += Stick(pad.sThumbRY) * s.lookSpeed * dt;
    }
    if (pad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) speed *= s.fastMultiplier;
    if (pad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) speed *= s.slowMultiplier;
  }
  mx = std::clamp(mx, -1.0f, 1.0f);
  my = std::clamp(my, -1.0f, 1.0f);
  mz = std::clamp(mz, -1.0f, 1.0f);

  g_yaw = std::fmod(g_yaw + lookYaw, 360.0f);
  if (!vr) g_pitch = std::clamp(g_pitch + lookPitch, -89.0f, 89.0f);

  // Movement direction: where the view looks (VR: the head), strafing level.
  float yaw = g_yaw * kDegToRad, pitch = g_pitch * kDegToRad;
  if (vr && head) {
    yaw = Normalize(head->yaw) * kDegToRad;
    pitch = Normalize(head->pitch) * kDegToRad;
  }
  const float step = speed * dt;
  const float fx = std::cos(pitch) * std::cos(yaw), fy = std::cos(pitch) * std::sin(yaw), fz = std::sin(pitch);
  const float rx = -std::sin(yaw), ry = std::cos(yaw);
  g_pos.x += (fx * my + rx * mx) * step;
  g_pos.y += (fy * my + ry * mx) * step;
  g_pos.z += (fz * my + mz) * step;
}

bool Apply(FVector* location, FRotator* rotation, bool vr) {
  if (!g_active.load(std::memory_order_relaxed)) return false;
  if (!g_initialized) {
    g_initialized = true;
    g_pos = *location;
    g_yaw = Normalize(rotation->yaw);
    g_pitch = vr ? 0.0f : std::clamp(Normalize(rotation->pitch), -89.0f, 89.0f);
    VR_LOG_INFO("free camera starts at (%.0f, %.0f, %.0f), yaw %.1f, pitch %.1f", g_pos.x, g_pos.y, g_pos.z, g_yaw,
                g_pitch);
  }
  *location = g_pos;
  rotation->pitch = static_cast<int32_t>(std::lround(g_pitch * kDegToUnits));
  rotation->yaw = static_cast<int32_t>(std::lround(g_yaw * kDegToUnits));
  rotation->roll = 0;
  return true;
}

void InstallInputHooks(void* viewport) {
  if (!s.enabled || g_inputHooksTried || !viewport) return;
  g_inputHooksTried = true;
  uintptr_t client = 0, vtable = 0;
  vrcore::mem::Read(reinterpret_cast<uintptr_t>(viewport) + kViewportClient, client);
  if (client) vrcore::mem::Read(client, vtable);
  const auto& exe = vrcore::Exe();
  const uintptr_t base = reinterpret_cast<uintptr_t>(exe.base);
  if (!vtable || vtable < base || vtable >= base + exe.imageSize) {
    VR_LOG_WARN("free camera: no viewport client at viewport %p (+0x20 = %p); the player keeps getting input",
                viewport, reinterpret_cast<void*>(client));
    return;
  }
  const bool ok = vrcore::hook::InstallVTable("ViewportClient::InputKey", reinterpret_cast<void*>(client), 0x14 / 4,
                                              &HookInputKey, &g_origInputKey) &&
                  vrcore::hook::InstallVTable("ViewportClient::InputAxis", reinterpret_cast<void*>(client), 0x1C / 4,
                                              &HookInputAxis, &g_origInputAxis);
  g_inputHooked = ok;
  VR_LOG_INFO("free camera: viewport client %p input hooks %s", reinterpret_cast<void*>(client),
              ok ? "installed" : "FAILED");
}

}  // namespace rm::free_cam
