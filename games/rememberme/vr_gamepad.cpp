#include "vr_gamepad.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "vrcore/hook.h"
#include "vrcore/log.h"

namespace rm::vr_gamepad {
namespace {

using GetState_t = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using SetState_t = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

GetState_t g_origGetState = nullptr;
SetState_t g_origSetState = nullptr;
vrcore::Compositor* g_compositor = nullptr;
bool g_enabled = true;
float g_gripThreshold = 0.5f;
bool g_loggedUse = false;
bool g_loggedConnected = false;

std::mutex g_mutex;  // XInput may be polled from more than one thread
XINPUT_GAMEPAD g_last{};
DWORD g_packet = 0;
bool g_grip[2] = {false, false};
uint64_t g_nextLogMs = 0;
SHORT g_loggedSticks[4] = {};

SHORT Axis(float v) {
  v = std::clamp(v, -1.0f, 1.0f);
  return static_cast<SHORT>(v >= 0 ? v * 32767.0f : v * 32768.0f);
}

BYTE Trigger(float v) { return static_cast<BYTE>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

XINPUT_GAMEPAD FromControllers(const vrcore::xr::ControllerState& s) {
  XINPUT_GAMEPAD pad{};
  pad.sThumbLX = Axis(s.stick[0][0]);
  pad.sThumbLY = Axis(s.stick[0][1]);
  pad.sThumbRX = Axis(s.stick[1][0]);
  pad.sThumbRY = Axis(s.stick[1][1]);
  pad.bLeftTrigger = Trigger(s.trigger[0]);
  pad.bRightTrigger = Trigger(s.trigger[1]);
  WORD b = 0;
  if (s.primary[1]) b |= XINPUT_GAMEPAD_A;
  if (s.secondary[1]) b |= XINPUT_GAMEPAD_B;
  if (s.primary[0]) b |= XINPUT_GAMEPAD_X;
  if (s.secondary[0]) b |= XINPUT_GAMEPAD_Y;
  // Grips with a little hysteresis (analog squeeze).
  for (int h = 0; h < 2; ++h) {
    if (s.grip[h] > g_gripThreshold) g_grip[h] = true;
    if (s.grip[h] < g_gripThreshold - 0.15f) g_grip[h] = false;
  }
  if (g_grip[0]) b |= XINPUT_GAMEPAD_LEFT_SHOULDER;
  if (g_grip[1]) b |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
  if (s.stickClick[0]) b |= XINPUT_GAMEPAD_LEFT_THUMB;
  if (s.stickClick[1]) b |= XINPUT_GAMEPAD_RIGHT_THUMB;
  if (s.padPress[1] || s.menu[0]) b |= XINPUT_GAMEPAD_START;
  if (s.padPress[0]) {
    const float x = s.pad[0][0], y = s.pad[0][1];
    if (std::hypot(x, y) < 0.5f) {
      b |= XINPUT_GAMEPAD_BACK;
    } else if (std::fabs(x) > std::fabs(y)) {
      b |= x > 0 ? XINPUT_GAMEPAD_DPAD_RIGHT : XINPUT_GAMEPAD_DPAD_LEFT;
    } else {
      b |= y > 0 ? XINPUT_GAMEPAD_DPAD_UP : XINPUT_GAMEPAD_DPAD_DOWN;
    }
  }
  pad.wButtons = b;
  return pad;
}

// Per stick, the one pushed further wins.
void Merge(XINPUT_GAMEPAD& vr, const XINPUT_GAMEPAD& real) {
  vr.wButtons |= real.wButtons;
  vr.bLeftTrigger = std::max(vr.bLeftTrigger, real.bLeftTrigger);
  vr.bRightTrigger = std::max(vr.bRightTrigger, real.bRightTrigger);
  const auto mag = [](SHORT x, SHORT y) { return std::hypot(static_cast<float>(x), static_cast<float>(y)); };
  if (mag(real.sThumbLX, real.sThumbLY) > mag(vr.sThumbLX, vr.sThumbLY)) {
    vr.sThumbLX = real.sThumbLX;
    vr.sThumbLY = real.sThumbLY;
  }
  if (mag(real.sThumbRX, real.sThumbRY) > mag(vr.sThumbRX, vr.sThumbRY)) {
    vr.sThumbRX = real.sThumbRX;
    vr.sThumbRY = real.sThumbRY;
  }
}

DWORD WINAPI HookGetState(DWORD user, XINPUT_STATE* state) {
  const DWORD result = g_origGetState(user, state);
  if (!g_enabled || user != 0 || !state || !g_compositor) return result;
  const vrcore::xr::ControllerState controllers = g_compositor->Controllers();
  if (!controllers.active) {
    if (result == ERROR_SUCCESS) return result;  // a real gamepad: as is
    // Report player 0 as connected from the start: the game only reads a
    // gamepad's sticks if one was connected when it looked (buttons alone
    // seem to work regardless), and the VR controllers only become active
    // once the OpenXR session runs. Neutral state until then.
    std::lock_guard<std::mutex> lock(g_mutex);
    std::memset(state, 0, sizeof(*state));
    state->dwPacketNumber = g_packet;
    if (!g_loggedConnected) {
      g_loggedConnected = true;
      VR_LOG_INFO("vr gamepad: reporting player 0 as a connected gamepad (VR controllers not active yet)");
    }
    return ERROR_SUCCESS;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  XINPUT_GAMEPAD pad = FromControllers(controllers);
  if (result == ERROR_SUCCESS) Merge(pad, state->Gamepad);
  if (std::memcmp(&pad, &g_last, sizeof(pad)) != 0) {
    g_last = pad;
    ++g_packet;
  }
  // Diagnostics: what the game gets, when the sticks change (at most 2x/s).
  const SHORT sticks[4] = {pad.sThumbLX, pad.sThumbLY, pad.sThumbRX, pad.sThumbRY};
  bool moved = false;
  for (int i = 0; i < 4; ++i) moved = moved || std::abs(sticks[i] - g_loggedSticks[i]) > 6000;
  const uint64_t now = GetTickCount64();
  if (moved && now >= g_nextLogMs) {
    g_nextLogMs = now + 500;
    std::memcpy(g_loggedSticks, sticks, sizeof(sticks));
    VR_LOG_INFO("vr gamepad: to game L %d,%d R %d,%d buttons 0x%04x (real pad %s: L %d,%d R %d,%d)", pad.sThumbLX,
                pad.sThumbLY, pad.sThumbRX, pad.sThumbRY, pad.wButtons, result == ERROR_SUCCESS ? "present" : "absent",
                result == ERROR_SUCCESS ? state->Gamepad.sThumbLX : 0, result == ERROR_SUCCESS ? state->Gamepad.sThumbLY : 0,
                result == ERROR_SUCCESS ? state->Gamepad.sThumbRX : 0, result == ERROR_SUCCESS ? state->Gamepad.sThumbRY : 0);
  }
  state->dwPacketNumber = g_packet;
  state->Gamepad = pad;
  if (!g_loggedUse) {
    g_loggedUse = true;
    VR_LOG_INFO("vr gamepad: VR controllers reported as player 0's gamepad%s",
                result == ERROR_SUCCESS ? " (merged with a connected gamepad)" : "");
  }
  return ERROR_SUCCESS;
}

DWORD WINAPI HookSetState(DWORD user, XINPUT_VIBRATION* vibration) {
  const DWORD result = g_origSetState(user, vibration);
  if (g_enabled && user == 0 && vibration && g_compositor) {
    // Left motor: low frequency, right: high frequency; map to the hands.
    g_compositor->Vibrate(vibration->wLeftMotorSpeed / 65535.0f, vibration->wRightMotorSpeed / 65535.0f);
  }
  if (g_enabled && user == 0 && g_compositor && g_compositor->Controllers().active) return ERROR_SUCCESS;
  return result;
}

}  // namespace

void Configure(const vrcore::Config& config, vrcore::Compositor* compositor) {
  g_compositor = compositor;
  g_enabled = config.GetBool("Input", "VRControllers", true);
  g_gripThreshold = config.GetFloat("Input", "GripThreshold", g_gripThreshold);
}

void Install(bool early) {
  if (!g_enabled || !g_compositor || g_origGetState) return;
  HMODULE xinput = GetModuleHandleW(L"xinput1_3.dll");
  if (!xinput && early) return;  // DllMain: no LoadLibrary; try again later
  if (!xinput) xinput = LoadLibraryW(L"xinput1_3.dll");
  if (!xinput) {
    VR_LOG_WARN("vr gamepad: xinput1_3.dll not loaded; VR controllers off");
    return;
  }
  // The game imports these by ordinal: 2 = XInputGetState, 3 = XInputSetState.
  auto getState = reinterpret_cast<void*>(GetProcAddress(xinput, MAKEINTRESOURCEA(2)));
  auto setState = reinterpret_cast<void*>(GetProcAddress(xinput, MAKEINTRESOURCEA(3)));
  const bool ok = getState && setState &&
                  vrcore::hook::Install("XInputGetState", getState, reinterpret_cast<void*>(&HookGetState),
                                        reinterpret_cast<void**>(&g_origGetState)) &&
                  vrcore::hook::Install("XInputSetState", setState, reinterpret_cast<void*>(&HookSetState),
                                        reinterpret_cast<void**>(&g_origSetState));
  VR_LOG_INFO("vr gamepad %s%s", ok ? "installed (VR controllers act as player 0's gamepad)" : "FAILED",
              early ? " at attach" : "");
}

}  // namespace rm::vr_gamepad
