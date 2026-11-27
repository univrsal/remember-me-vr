#include "free_cam.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "input_capture.h"
#include "vr_gamepad.h"
#include "vrcore/log.h"

namespace rm::free_cam {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kDegToUnits = 65536.0f / 360.0f;

Settings s;
std::atomic<bool> g_active{false};
std::atomic<bool> g_restart{false};  // take the game camera's pose on the next Apply

// Free camera pose (game thread only).
bool g_initialized = false;
FVector g_pos{};
float g_yaw = 0.0f, g_pitch = 0.0f;  // degrees
LARGE_INTEGER g_lastTick{};

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

float Stick(SHORT v) {
  const float f = std::clamp(v / 32767.0f, -1.0f, 1.0f);
  const float dz = std::clamp(s.deadzone, 0.0f, 0.9f);
  if (std::fabs(f) < dz) return 0.0f;
  // Rescale so movement starts at 0 just outside the deadzone.
  return (f - std::copysign(dz, f)) / (1.0f - dz);
}

float Trigger(BYTE v) { return v > XINPUT_GAMEPAD_TRIGGER_THRESHOLD ? v / 255.0f : 0.0f; }

float Normalize(int32_t units) {
  // FRotator components may be outside -32768..32767 (e.g. 65000 = looking down slightly).
  return static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(units & 0xFFFF))) / kDegToUnits;
}

}  // namespace

void Configure(const vrcore::Config& config) {
  s.speed = config.GetFloat("FreeCam", "Speed", s.speed);
  s.fastMultiplier = config.GetFloat("FreeCam", "FastMultiplier", s.fastMultiplier);
  s.slowMultiplier = config.GetFloat("FreeCam", "SlowMultiplier", s.slowMultiplier);
  s.lookSpeed = config.GetFloat("FreeCam", "LookSpeed", s.lookSpeed);
  s.mouseSensitivity = config.GetFloat("FreeCam", "MouseSensitivity", s.mouseSensitivity);
  s.invertMouseY = config.GetBool("FreeCam", "InvertMouseY", s.invertMouseY);
  s.deadzone = std::clamp(config.GetFloat("FreeCam", "Deadzone", s.deadzone), 0.0f, 0.9f);
}

void Save(const vrcore::Config& config) {
  config.SetFloat("FreeCam", "Speed", s.speed);
  config.SetFloat("FreeCam", "FastMultiplier", s.fastMultiplier);
  config.SetFloat("FreeCam", "SlowMultiplier", s.slowMultiplier);
  config.SetFloat("FreeCam", "LookSpeed", s.lookSpeed);
  config.SetFloat("FreeCam", "MouseSensitivity", s.mouseSensitivity);
  config.SetBool("FreeCam", "InvertMouseY", s.invertMouseY);
  config.SetFloat("FreeCam", "Deadzone", s.deadzone);
}

Settings& Tunables() { return s; }

bool Active() { return g_active.load(std::memory_order_relaxed); }

void SetActive(bool active) {
  if (g_active.exchange(active) == active) return;
  if (active) g_restart = true;
  input_capture::SetFreeCam(active);
  VR_LOG_INFO("free camera %s", active ? "ON" : "OFF");
}

void Update(bool vr, const FRotator* head) {
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  const float dt = g_lastTick.QuadPart
                       ? std::clamp(static_cast<float>(now.QuadPart - g_lastTick.QuadPart) / freq.QuadPart, 0.0f, 0.1f)
                       : 0.0f;
  g_lastTick = now;
  // The menu has the input while it's open.
  if (!g_active.load() || !g_initialized || input_capture::MenuOpen()) return;

  // Inputs: x right, y forward, z up (-1..1), look in degrees.
  float mx = 0, my = 0, mz = 0, lookYaw = 0, lookPitch = 0, speed = s.speed;
  if (input_capture::GameWindowFocused()) {
    my += (KeyDown('W') ? 1.0f : 0.0f) - (KeyDown('S') ? 1.0f : 0.0f);
    mx += (KeyDown('D') ? 1.0f : 0.0f) - (KeyDown('A') ? 1.0f : 0.0f);
    mz += (KeyDown('E') || KeyDown(VK_SPACE) ? 1.0f : 0.0f) - (KeyDown('Q') ? 1.0f : 0.0f);
    if (KeyDown(VK_SHIFT)) speed *= s.fastMultiplier;
    if (KeyDown(VK_CONTROL)) speed *= s.slowMultiplier;
  }
  lookYaw += input_capture::TakeMouseX() * s.mouseSensitivity;
  lookPitch += input_capture::TakeMouseY() * s.mouseSensitivity * (s.invertMouseY ? -1.0f : 1.0f);
  XINPUT_GAMEPAD pad{};
  if (vr_gamepad::ReadPad(pad)) {
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
  if (g_restart.exchange(false) || !g_initialized) {
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

}  // namespace rm::free_cam
