// Remember Me (DONTNOD, 2013; UE3 engineVersion 8636). Steam app 228300.

#include <cinttypes>

#include "vrcore/game.h"
#include "vrcore/log.h"
#include "vrcore/process.h"
#include "vrcore/ue3.h"
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
    ue3Discover_ = config.GetBool("UE3", "Discover", true);
    ue3DumpOnStart_ = config.GetBool("UE3", "DumpObjectsOnStart", true);
    ue3DumpKey_ = config.GetInt("UE3", "DumpObjectsKey", 0x78);  // F9
    VR_LOG_INFO("VR %s: virtual screen %.1f m wide at %.1f m, recenter key 0x%02x", vrEnabled_ ? "on" : "off",
                screen.width, screen.distance, screen.recenterKey);
  }

  void OnDeviceCreated(IDirect3DDevice9*, const D3DPRESENT_PARAMETERS& params,
                       const vrcore::DxvkInterop* dxvk) override {
    VR_LOG_INFO("game device: backbuffer %ux%u, %u buffer(s), swap effect %d, %s",
                params.BackBufferWidth, params.BackBufferHeight, params.BackBufferCount, params.SwapEffect,
                dxvk ? "DXVK interop available" : "no DXVK interop");
    if (!dxvk) VR_LOG_WARN("VR output needs DXVK's d3d9.dll (Proton's default, or dxvk next to the exe on Windows)");
    // The exe is unpacked and the engine initialised by now: map UE3's reflection.
    if (ue3Discover_ && !reflection_.Valid() && reflection_.Discover()) {
      reflection_.DiscoverTypes();
      if (ue3DumpOnStart_) DumpObjects("start");
    }
  }

  void OnBeforeReset(IDirect3DDevice9*) override { screen_.OnDeviceChanged(); }

  void OnPresent(IDirect3DDevice9* device) override {
    if (ue3DumpKey_) {
      const bool down = (GetAsyncKeyState(ue3DumpKey_) & 0x8000) != 0;
      if (down && !dumpKeyDown_ && reflection_.Valid()) DumpObjects("hotkey");
      dumpKeyDown_ = down;
    }
    if (vrEnabled_) screen_.OnPresent(device);
    ++frames_;
    if (presentLogInterval_ > 0 && frames_ % static_cast<uint64_t>(presentLogInterval_) == 0) {
      VR_LOG_DEBUG("present #%" PRIu64, frames_);
    }
  }

  void OnDetach() override { VR_LOG_INFO("frames presented: %" PRIu64, frames_); }

 private:
  void DumpObjects(const char* why) {
    const std::wstring path = vrcore::ModDirectory() + L"RememberMeVR_objects.txt";
    const int count = reflection_.DumpObjects(path.c_str());
    VR_LOG_INFO("ue3: dumped %d objects (%s) to %s", count, why, vrcore::Narrow(path).c_str());
    if (reflection_.TypesValid()) {
      const std::wstring typesPath = vrcore::ModDirectory() + L"RememberMeVR_types.txt";
      const int structs = reflection_.DumpTypes(typesPath.c_str());
      VR_LOG_INFO("ue3: dumped %d classes/structs (%s) to %s", structs, why, vrcore::Narrow(typesPath).c_str());
    }
  }

  bool knownBuild_ = false;
  bool ue3Discover_ = true;
  bool ue3DumpOnStart_ = true;
  int ue3DumpKey_ = 0;
  bool dumpKeyDown_ = false;
  vrcore::ue3::Reflection reflection_;
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
