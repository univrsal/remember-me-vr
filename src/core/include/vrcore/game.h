#pragma once

#include <cstdint>

#include <d3d9.h>

#include "vrcore/config.h"
#include "vrcore/dxvk_interop.h"

namespace vrcore {

struct GameInfo {
  const char* name;           // e.g. "Remember Me"
  const wchar_t* modName;     // base name for <modName>.log / <modName>.ini
  const wchar_t* exeName;     // expected host executable, e.g. L"RememberMe.exe"
  uint32_t knownTimestamp;    // PE timestamp of the build the mod was written for (0 = don't check)
};

// Implemented once per game module. vrcore calls these from its hooks; all
// D3D9 callbacks run on the thread that issued the D3D9 call (UE3: render thread).
class Game {
 public:
  virtual ~Game() = default;

  virtual GameInfo Info() const = 0;

  // DllMain context (loader lock held): keep it to reading config.
  // `knownBuild` is false when the exe's PE timestamp differs from knownTimestamp.
  virtual void OnAttach(const Config& config, bool knownBuild) { (void)config; (void)knownBuild; }

  // Direct3DCreate9 was called (once, on the thread that called it). The
  // game's code is unpacked by now, but its D3D9 device and (UE3) its
  // viewport don't exist yet: the point to hook code that sizes them.
  virtual void OnDirect3DCreated() {}

  // After IDirect3D9::CreateDevice succeeded. `dxvk` is null when not on DXVK.
  // By now the game's code is unpacked (SteamStub has run), so this is the
  // first safe point to hook game code.
  virtual void OnDeviceCreated(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& params,
                               const DxvkInterop* dxvk) { (void)device; (void)params; (void)dxvk; }

  virtual void OnBeforeReset(IDirect3DDevice9* device) { (void)device; }
  virtual void OnAfterReset(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& params, HRESULT result) {
    (void)device; (void)params; (void)result;
  }

  // Before the frame is presented (device or swapchain Present).
  virtual void OnPresent(IDirect3DDevice9* device) { (void)device; }

  // True: when the game presents its whole backbuffer into a window of a
  // different aspect ratio, present it scaled to fit and centred
  // (letterboxed) instead of stretched (see vrcore/letterbox.h).
  virtual bool LetterboxPresent() const { return false; }

  virtual void OnDetach() {}
};

// Provided by the game module.
Game& GetGame();

}  // namespace vrcore
