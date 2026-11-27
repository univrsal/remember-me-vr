#include "vr_gamepad.h"

#include <windows.h>
#include <xinput.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include "engine.h"
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
uint64_t g_nextDiagMs = 0;
std::string g_lastDiag;

template <typename T>
T ReadVA(uint32_t va) {
  T v{};
  vrcore::mem::Read(reinterpret_cast<uintptr_t>(vrcore::Exe().FromPreferredVA(va)), v);
  return v;
}
template <typename T>
T ReadAt(uintptr_t address) {
  T v{};
  vrcore::mem::Read(address, v);
  return v;
}

// Player 0's UExamplePlayerInput, or 0.
uintptr_t PlayerInput0() {
  const uintptr_t engine = ReadVA<uint32_t>(rm::kGEngine);
  const uintptr_t players = engine ? ReadAt<uint32_t>(engine + rm::kEngineGamePlayers) : 0;
  if (!players || ReadAt<int32_t>(engine + rm::kEngineGamePlayers + 4) < 1) return 0;
  const uintptr_t player = ReadAt<uint32_t>(players);
  const uintptr_t pc = player ? ReadAt<uint32_t>(player + rm::kLocalPlayerActor) : 0;
  return pc ? ReadAt<uint32_t>(pc + rm::kControllerPlayerInput) : 0;
}

// The player input's stick values (raw, filtered, gameplay), for the log.
std::string PlayerInputSticks() {
  const uintptr_t pi = PlayerInput0();
  if (!pi) return "no player input";
  float v[12];
  for (int i = 0; i < 4; ++i) {
    v[i] = ReadAt<float>(pi + rm::kPlayerInputRawJoyLeft + 4u * i);
    v[4 + i] = ReadAt<float>(pi + rm::kPlayerInputJoyLeft + 4u * i);
    v[8 + i] = ReadAt<float>(pi + rm::kPlayerInputGameplayJoyLeft + 4u * i);
  }
  char buf[200];
  std::snprintf(buf, sizeof(buf), "raw L %.2f,%.2f R %.2f,%.2f / joy L %.2f,%.2f R %.2f,%.2f / gameplay L %.2f,%.2f R %.2f,%.2f",
                v[1], v[0], v[3], v[2], v[5], v[4], v[7], v[6], v[9], v[8], v[11], v[10]);
  return buf;
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

// --- Input trace (diagnostics) -------------------------------------------
// FUN_00C75600 (thiscall on UWindowsViewport): processes one controller's
// XInput state and calls the viewport client's InputKey (FViewportClient
// vtable +0x14) / InputAxis (+0x1C). The client interface pointer is at
// viewport+0x20.
constexpr uint32_t kProcessControllerVA = 0x00C75600;
constexpr uint8_t kProcessControllerPrologue[] = {0x6A, 0xFF, 0x68, 0x7A, 0xCD, 0x1A, 0x01};
using ProcessControllerFn = int(__fastcall*)(void*, void*, uint32_t, int32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                             uint32_t);
using InputKeyFn = int(__fastcall*)(void*, void*, void*, int32_t, int32_t, int32_t, int32_t, float, int32_t);
using InputAxisFn = int(__fastcall*)(void*, void*, void*, int32_t, int32_t, int32_t, float, float, int32_t);
ProcessControllerFn g_origProcess = nullptr;
InputKeyFn g_origInputKey = nullptr;
InputAxisFn g_origInputAxis = nullptr;
bool g_traceHooked = false;
std::string (*g_nameOf)(int32_t) = nullptr;
uint64_t g_nextAxisLogMs = 0;
int g_keyLogs = 0;

std::string KeyName(int32_t index, int32_t number) {
  std::string n = g_nameOf ? g_nameOf(index) : std::string();
  if (n.empty()) n = "#" + std::to_string(index);
  if (number) n += "_" + std::to_string(number - 1);
  return n;
}

int __fastcall HookInputKey(void* self, void* edx, void* viewport, int32_t controllerId, int32_t name, int32_t number,
                            int32_t event, float amount, int32_t gamepad) {
  const int r = g_origInputKey(self, edx, viewport, controllerId, name, number, event, amount, gamepad);
  if (event != 2 && g_keyLogs < 400) {  // skip auto-repeat (IE_Repeat)
    ++g_keyLogs;
    VR_LOG_INFO("input trace: key %s event %d controller %d gamepad %d -> %s", KeyName(name, number).c_str(), event,
                controllerId, gamepad, r ? "consumed" : "not consumed");
  }
  return r;
}

int __fastcall HookInputAxis(void* self, void* edx, void* viewport, int32_t controllerId, int32_t name, int32_t number,
                             float delta, float deltaTime, int32_t gamepad) {
  const int r = g_origInputAxis(self, edx, viewport, controllerId, name, number, delta, deltaTime, gamepad);
  const uint64_t now = GetTickCount64();
  if (std::fabs(delta) > 0.25f && now >= g_nextAxisLogMs) {
    g_nextAxisLogMs = now + 250;
    VR_LOG_INFO("input trace: axis %s %.2f controller %d gamepad %d -> %s", KeyName(name, number).c_str(), delta,
                controllerId, gamepad, r ? "consumed" : "not consumed");
  }
  return r;
}

int __fastcall HookProcessController(void* viewport, void* edx, uint32_t deltaTime, int32_t index, uint32_t s0,
                                     uint32_t s1, uint32_t s2, uint32_t s3, uint32_t s4) {
  if (!g_traceHooked) {
    // First call: hook the viewport client's InputKey / InputAxis (shared
    // vtable, so every call from the engine goes through them).
    g_traceHooked = true;
    void* client = nullptr;
    vrcore::mem::Read(reinterpret_cast<uintptr_t>(viewport) + 0x20, client);
    bool ok = false;
    if (client) {
      ok = vrcore::hook::InstallVTable("ViewportClient::InputKey", client, 0x14 / 4, &HookInputKey, &g_origInputKey) &&
           vrcore::hook::InstallVTable("ViewportClient::InputAxis", client, 0x1C / 4, &HookInputAxis,
                                       &g_origInputAxis);
    }
    VR_LOG_INFO("input trace: viewport client %p hooks %s", client, ok ? "installed" : "FAILED");
  }
  return g_origProcess(viewport, edx, deltaTime, index, s0, s1, s2, s3, s4);
}

// --- Script event trace (diagnostics) ------------------------------------
// UObject::ProcessEvent(UFunction*, void* parms, void* result), thiscall.
constexpr uint32_t kProcessEventVA = 0x00458BA0;
constexpr uint8_t kProcessEventPrologue[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, 0x08, 0x42, 0x15, 0x01};
using ProcessEventFn = void(__fastcall*)(void*, void*, void*, void*, void*);
ProcessEventFn g_origProcessEvent = nullptr;
std::string (*g_fullName)(uintptr_t) = nullptr;
std::mutex g_eventMutex;
struct EventInfo {
  bool traced = false;
  int logged = 0;
  std::string name;
};
std::unordered_map<void*, EventInfo> g_events;

bool IsTracedEvent(const std::string& fullName) {
  const size_t dot = fullName.rfind('.');
  std::string n = dot == std::string::npos ? fullName : fullName.substr(dot + 1);
  for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  static const char* const kWords[] = {"controllerchange", "gamepad", "binding", "pad", "joy", "device",
                                       "inputtype", "connected", "unpause", "pausemenu", "focus", "controllertype",
                                       "inputmode", "usingmouse", "keyboard"};
  for (const char* w : kWords) {
    if (n.find(w) != std::string::npos) return true;
  }
  return false;
}

void __fastcall HookProcessEvent(void* self, void* edx, void* function, void* parms, void* result) {
  if (function && g_fullName) {
    std::lock_guard<std::mutex> lock(g_eventMutex);
    auto it = g_events.find(function);
    if (it == g_events.end()) {
      EventInfo info;
      info.name = g_fullName(reinterpret_cast<uintptr_t>(function));
      info.traced = !info.name.empty() && IsTracedEvent(info.name);
      it = g_events.emplace(function, std::move(info)).first;
    }
    if (it->second.traced && it->second.logged < 30) {
      ++it->second.logged;
      uint32_t p[4] = {};
      if (parms) vrcore::mem::Read(reinterpret_cast<uintptr_t>(parms), p);
      VR_LOG_INFO("event trace: %s on %s parms %08x %08x %08x %08x", it->second.name.c_str(),
                  g_fullName(reinterpret_cast<uintptr_t>(self)).c_str(), p[0], p[1], p[2], p[3]);
    }
  }
  g_origProcessEvent(self, edx, function, parms, result);
}

// Diagnostics (sticks ignored without a real gamepad): the game's joystick
// table, its "current" controller and the players' ControllerIds, logged when
// they change (checked once a second).
void LogInputState() {
  const uint64_t now = GetTickCount64();
  if (now < g_nextDiagMs) return;
  g_nextDiagMs = now + 1000;
  char buf[96];
  std::string text = "joysticks:";
  const int32_t count = ReadVA<int32_t>(rm::kJoystickCount);
  const uintptr_t table = ReadVA<uint32_t>(rm::kJoysticks);
  for (int32_t i = 0; i < count && i < 8 && table; ++i) {
    const uintptr_t e = table + static_cast<uintptr_t>(i) * rm::kJoystickStride;
    std::snprintf(buf, sizeof(buf), " [%d dev=%08x id=%d conn=%d]", i, ReadAt<uint32_t>(e),
                  ReadAt<int32_t>(e + rm::kJoystickId), ReadAt<int32_t>(e + rm::kJoystickConnected));
    text += buf;
  }
  std::snprintf(buf, sizeof(buf), " (count %d) current=%d players:", count, ReadVA<int32_t>(rm::kCurrentController));
  text += buf;
  const uintptr_t engine = ReadVA<uint32_t>(rm::kGEngine);
  const uintptr_t players = engine ? ReadAt<uint32_t>(engine + rm::kEngineGamePlayers) : 0;
  const int32_t numPlayers = engine ? ReadAt<int32_t>(engine + rm::kEngineGamePlayers + 4) : 0;
  for (int32_t i = 0; i < numPlayers && i < 4 && players; ++i) {
    const uintptr_t player = ReadAt<uint32_t>(players + 4u * static_cast<uint32_t>(i));
    std::snprintf(buf, sizeof(buf), " [%d controllerId=%d]", i,
                  player ? ReadAt<int32_t>(player + rm::kLocalPlayerControllerId) : -1);
    text += buf;
  }
  if (const uintptr_t pi = PlayerInput0()) {
    std::snprintf(buf, sizeof(buf), " input: activeControllerId=%d flags=%08x",
                  ReadAt<int32_t>(pi + rm::kPlayerInputActiveControllerId), ReadAt<uint32_t>(pi + rm::kPlayerInputFlags));
    text += buf;
  }
  if (text != g_lastDiag) {
    g_lastDiag = text;
    VR_LOG_INFO("vr gamepad: %s", text.c_str());
  }
}
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
  if (user == 0 && g_compositor) RegisterVirtualPad();
  const DWORD result = g_origGetState(user, state);
  if (user == 0) LogInputState();
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
    VR_LOG_INFO("vr gamepad: player input %s", PlayerInputSticks().c_str());
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

void InstallInputTrace(std::string (*nameOf)(int32_t index)) {
  g_nameOf = nameOf;
  if (g_origProcess) return;
  void* fn = vrcore::Exe().FromPreferredVA(kProcessControllerVA);
  if (!vrcore::mem::Matches(reinterpret_cast<uintptr_t>(fn), kProcessControllerPrologue,
                            sizeof(kProcessControllerPrologue))) {
    VR_LOG_WARN("input trace: controller processing prologue doesn't match this build");
    return;
  }
  const bool ok = vrcore::hook::Install("ProcessController", fn, reinterpret_cast<void*>(&HookProcessController),
                                        reinterpret_cast<void**>(&g_origProcess));
  VR_LOG_INFO("input trace %s", ok ? "installed" : "FAILED");
}

void InstallEventTrace(std::string (*fullName)(uintptr_t object)) {
  g_fullName = fullName;
  if (g_origProcessEvent) return;
  void* fn = vrcore::Exe().FromPreferredVA(kProcessEventVA);
  if (!vrcore::mem::Matches(reinterpret_cast<uintptr_t>(fn), kProcessEventPrologue, sizeof(kProcessEventPrologue))) {
    VR_LOG_WARN("event trace: ProcessEvent prologue doesn't match this build");
    return;
  }
  const bool ok = vrcore::hook::Install("ProcessEvent", fn, reinterpret_cast<void*>(&HookProcessEvent),
                                        reinterpret_cast<void**>(&g_origProcessEvent));
  VR_LOG_INFO("event trace %s", ok ? "installed" : "FAILED");
}

}  // namespace rm::vr_gamepad
