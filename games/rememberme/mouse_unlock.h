#pragma once

#include <cstdint>

#include "vrcore/config.h"

namespace rm::mouse_unlock {

// Frees the Windows mouse cursor while the game shows a menu (pause / Sensen
// menu: ExamplePlayerController.bMenuPause; main menu: no player pawn), so it
// can leave the window. In game the game hides the cursor and clips it to the
// window (UE3 UWindowsViewport, all on the game thread):
//   0x00C73E60  SetCursorVisible(FViewport* this, UBOOL visible): ShowCursor loop
//   0x00C73F60  UpdateMouseLock(UWindowsViewport* this): ClipCursor to the client rect
//   0x00C741E0  UpdateMouseCursor(FViewport* this, UBOOL set): asks the viewport client
//               for the cursor type; "none" = SetCursor(NULL)
// While unlocked these show an arrow and drop the clip; on leaving the menu
// the game's own state is restored. The DirectInput mouse is non-exclusive
// (cooperative level 6, FUN_00C73590), so it doesn't confine the cursor.
// [Input] FreeMouseInMenus. Not while the config menu or the free camera has
// the input.
void Configure(const vrcore::Config& config);
void Save(const vrcore::Config& config);
void Install();  // known build, once the exe is unpacked
bool Available();
bool Enabled();
void SetEnabled(bool enabled);  // any thread (config menu)
void SetMenuFlag(int32_t offset, uint32_t mask);  // bMenuPause on the player controller

// Game thread, once per frame (main CalcSceneView): `viewport` is the
// FViewport (UWindowsViewport + 4).
void Update(void* localPlayer, void* viewport);

}  // namespace rm::mouse_unlock
