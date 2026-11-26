// Remember Me (DONTNOD, 2013; UE3 engineVersion 8636). Steam app 228300.

#include <cinttypes>

#include "vrcore/game.h"
#include "vrcore/log.h"
#include "vrcore/virtual_screen.h"

namespace {

// PE file-header timestamp (IMAGE_FILE_HEADER::TimeDateStamp) of the Steam build the analysis in docs/ was done on
// (changelist 940782). See docs/01-game-analysis.md.
constexpr uint32_t kKnownTimestamp = 0x69d8a358;

class RememberMe final : public vrcore::Game {
 public:
  vrcore::GameInfo Info() const override {
    return {"Remember Me", L"RememberMeVR", L"RememberMe.exe", kKnownTimestamp};
  }

  void OnAttach(const vrcore::Config& config, bool knownBuild) override {
    knownBuild_ = knownBuild;
    presentLogInterval_ = config.GetInt("Debug", "PresentLogInterval", 0);
    vrEnabled_ = config.GetBool("VR", "Enabled", true);
    vrcore::VirtualScreen::Settings screen;
    screen.distance = config.GetFloat("VR", "ScreenDistance", screen.distance);
    screen.width = config.GetFloat("VR", "ScreenWidth", screen.width);
    screen.recenterKey = config.GetInt("VR", "RecenterKey", screen.recenterKey);
    screen.appName = "Remember Me VR";
    screen_.Configure(screen);
    VR_LOG_INFO("VR %s: virtual screen %.1f m wide at %.1f m, recenter key 0x%02x", vrEnabled_ ? "on" : "off",
                screen.width, screen.distance, screen.recenterKey);
  }

  void OnDeviceCreated(IDirect3DDevice9*, const D3DPRESENT_PARAMETERS& params,
                       const vrcore::DxvkInterop* dxvk) override {
    VR_LOG_INFO("game device: backbuffer %ux%u, %u buffer(s), swap effect %d, %s",
                params.BackBufferWidth, params.BackBufferHeight, params.BackBufferCount, params.SwapEffect,
                dxvk ? "DXVK interop available" : "no DXVK interop");
    if (!dxvk) VR_LOG_WARN("VR output needs DXVK's d3d9.dll (Proton's default, or dxvk next to the exe on Windows)");
    // Engine (game-code) hooks will be installed here once M2 lands; gated on knownBuild_.
  }

  void OnBeforeReset(IDirect3DDevice9*) override { screen_.OnDeviceChanged(); }

  void OnPresent(IDirect3DDevice9* device) override {
    if (vrEnabled_) screen_.OnPresent(device);
    ++frames_;
    if (presentLogInterval_ > 0 && frames_ % static_cast<uint64_t>(presentLogInterval_) == 0) {
      VR_LOG_DEBUG("present #%" PRIu64, frames_);
    }
  }

  void OnDetach() override { VR_LOG_INFO("frames presented: %" PRIu64, frames_); }

 private:
  bool knownBuild_ = false;
  bool vrEnabled_ = true;
  vrcore::VirtualScreen screen_;
  int presentLogInterval_ = 0;
  uint64_t frames_ = 0;
};

}  // namespace

vrcore::Game& vrcore::GetGame() {
  static RememberMe game;
  return game;
}
