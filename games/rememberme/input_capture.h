#pragma once

#include <windows.h>
#include <xinput.h>

#include <cstdint>
#include <string>

#include "vrcore/config.h"

namespace rm::input_capture {

// Takes the player's input away from the game while the mod uses it (the
// config menu is open, or the free camera flies), and hands it to the mod:
//  - gamepad: the XInputGetState hook gives the game a neutral pad
//    (SwallowPad); the mod reads the real one with vr_gamepad::ReadPad.
//  - keyboard/mouse: the viewport client's InputKey / InputAxis (vtable +0x14
//    / +0x1C of the client at FViewport+0x20) swallow every event except key
//    releases (so held keys don't stick); mouse movement, buttons and wheel are
//    recorded for the mod.
// Also detects the menu toggle: [Menu] ToggleKey (F6) or the PadToggle combo
// (both stick clicks; the combo stays hidden from the game until released).

void Configure(const vrcore::Config& config);
// FName text lookup for the input hooks ("" until reflection is mapped).
void SetNameLookup(std::string (*nameOf)(int32_t index));
// FViewport* (CalcSceneView's argument): hooks the viewport client once.
void InstallInputHooks(void* viewport);
bool InputHooked();

// Presenting thread, once per frame (also when no world is rendered): key /
// pad-combo toggle of the menu.
void Poll();

bool MenuOpen();
void SetMenuOpen(bool open);
// Free camera active: capture input for it (while the menu is closed it gets
// the mouse and pad).
void SetFreeCam(bool active);
bool Capturing();

// From the XInputGetState hook: true if the game should see a neutral pad.
bool SwallowPad(const XINPUT_GAMEPAD& pad);

// Recorded mouse input while capturing (any thread). Take* returns and resets.
float TakeMouseX();
float TakeMouseY();
float TakeWheel();  // notches, + = up
bool MouseButton(int index);  // 0 left, 1 right
void ClearMouse();

// The game window has focus (keyboard polling with GetAsyncKeyState).
bool GameWindowFocused();

int ToggleKey();
WORD PadToggle();

}  // namespace rm::input_capture
