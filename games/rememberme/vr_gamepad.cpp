#include "vr_gamepad.h"

#include <windows.h>
#include <xinput.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "vrcore/hook.h"
#include "vrcore/log.h"
#include "vrcore/memory.h"
#include "vrcore/process.h"

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

template <typename T>
T ReadVA(uint32_t va) {
  T v{};
  vrcore::mem::Read(reinterpret_cast<uintptr_t>(vrcore::Exe().FromPreferredVA(va)), v);
  return v;
}

// --- Device registration -------------------------------------------------
// FUN_00C77850: the game's IDirectInput8::EnumDevices callback (stdcall,
// DIDEVICEINSTANCEW*, ref). It asks FUN_00C73700 (cdecl, const GUID*
// guidProduct; WMI "IG_" VID/PID check) whether the device is an XInput pad
// (0 = yes) and if so registers it as XInput pad number DAT_0166E4E4 (< 5,
// then incremented) in the game's device list. WM_DEVICECHANGE resets the
// count to 0 and re-enumerates.
constexpr uint32_t kEnumCallbackVA = 0x00C77850;
constexpr uint8_t kEnumCallbackPrologue[] = {0x6A, 0xFF, 0x68, 0xB8, 0xCE, 0x1A, 0x01};
constexpr uint32_t kIsXInputDeviceVA = 0x00C73700;
constexpr uint8_t kIsXInputDevicePrologue[] = {0x81, 0xEC, 0x8C, 0x00, 0x00, 0x00, 0x53, 0x56, 0x33, 0xDB};
constexpr uint32_t kXInputPadCountVA = 0x0166E4E4;
using EnumCallbackFn = BOOL(WINAPI*)(const DIDEVICEINSTANCEW*, void*);
using IsXInputDeviceFn = int(__cdecl*)(const GUID*);
EnumCallbackFn g_enumCallback = nullptr;
IsXInputDeviceFn g_origIsXInputDevice = nullptr;
bool g_loggedRegistration = false;

// Xbox 360 controller's DirectInput product GUID (VID 045E, PID 028E).
constexpr GUID kVirtualPadProduct = {0x028E045E, 0x0000, 0x0000, {0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44}};
// Any instance GUID unique to the virtual pad.
constexpr GUID kVirtualPadInstance = {0x5652504D, 0x4F44, 0x4E54, {0x56, 0x52, 0x50, 0x41, 0x44, 0x00, 0x00, 0x01}};

int __cdecl HookIsXInputDevice(const GUID* product) {
  if (product && std::memcmp(product, &kVirtualPadProduct, sizeof(GUID)) == 0) return 0;  // XInput
  return g_origIsXInputDevice(product);
}

// Game thread (called from XInputGetState): if no XInput pad is registered
// (none found, or a rescan just reset the count), register the virtual pad.
void RegisterVirtualPad() {
  if (!g_enumCallback || !g_origIsXInputDevice) return;
  if (ReadVA<int32_t>(kXInputPadCountVA) != 0) return;
  static DIDEVICEINSTANCEW instance = [] {
    DIDEVICEINSTANCEW d{};
    d.dwSize = sizeof(d);
    d.guidInstance = kVirtualPadInstance;
    d.guidProduct = kVirtualPadProduct;
    d.dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8) | DIDEVTYPE_HID;
    wcscpy(d.tszInstanceName, L"Controller (XBOX 360 For Windows)");
    wcscpy(d.tszProductName, L"Controller (XBOX 360 For Windows)");
    return d;
  }();
  g_enumCallback(&instance, nullptr);
  const int32_t count = ReadVA<int32_t>(kXInputPadCountVA);
  if (!g_loggedRegistration || count == 0) {
    g_loggedRegistration = true;
    VR_LOG_INFO("vr gamepad: registered player 0's gamepad with the game's device list (%s)",
                count > 0 ? "ok" : "FAILED, count still 0");
  }
}

// --- XInput emulation ----------------------------------------------------
XINPUT_GAMEPAD g_last{};
DWORD g_packet = 0;
bool g_grip[2] = {false, false};

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
  if (user == 0 && g_compositor) RegisterVirtualPad();
  const DWORD result = g_origGetState(user, state);
  if (!g_enabled || user != 0 || !state || !g_compositor) return result;
  const vrcore::xr::ControllerState controllers = g_compositor->Controllers();
  if (!controllers.active) {
    if (result == ERROR_SUCCESS) return result;  // a real gamepad: as is
    // Report player 0 as connected from the start (the VR controllers only
    // become active once the OpenXR session is focused): neutral state.
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

void InstallDeviceRegistration() {
  if (!g_enabled || !g_compositor || g_origIsXInputDevice) return;
  void* callback = vrcore::Exe().FromPreferredVA(kEnumCallbackVA);
  void* isXInput = vrcore::Exe().FromPreferredVA(kIsXInputDeviceVA);
  if (!vrcore::mem::Matches(reinterpret_cast<uintptr_t>(callback), kEnumCallbackPrologue,
                            sizeof(kEnumCallbackPrologue)) ||
      !vrcore::mem::Matches(reinterpret_cast<uintptr_t>(isXInput), kIsXInputDevicePrologue,
                            sizeof(kIsXInputDevicePrologue))) {
    VR_LOG_WARN("vr gamepad: device enumeration code doesn't match this build; VR sticks need a real gamepad");
    return;
  }
  if (!vrcore::hook::Install("IsXInputDevice", isXInput, reinterpret_cast<void*>(&HookIsXInputDevice),
                             reinterpret_cast<void**>(&g_origIsXInputDevice))) {
    return;
  }
  g_enumCallback = reinterpret_cast<EnumCallbackFn>(callback);
  VR_LOG_INFO("vr gamepad: device registration installed");
}

}  // namespace rm::vr_gamepad
