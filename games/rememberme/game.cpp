// Remember Me (DONTNOD, 2013; UE3 engineVersion 8636). Steam app 228300.

#include <atomic>
#include <cinttypes>

#include "device_state_hooks.h"
#include "draw_fixes.h"
#include "engine.h"
#include "free_cam.h"
#include "hide_actors.h"
#include "input_capture.h"
#include "menu.h"
#include "mirror.h"
#include "renderer_hooks.h"
#include "scene_view_hooks.h"
#include "sensen_stereo.h"
#include "vr_gamepad.h"
#include "vrcore/compositor.h"
#include "vrcore/game.h"
#include "vrcore/log.h"
#include "vrcore/memory.h"
#include "vrcore/process.h"
#include "vrcore/scene_tap.h"
#include "vrcore/ue3.h"

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
    instance_ = this;
    knownBuild_ = knownBuild;
    presentLogInterval_ = config.GetInt("Debug", "PresentLogInterval", 0);
    vrEnabled_ = config.GetBool("VR", "Enabled", true);
    vrcore::Compositor::Settings vr;
    vr.mode = config.GetString("VR", "Mode", "stereo") == "screen" ? vrcore::Compositor::Mode::Screen
                                                                   : vrcore::Compositor::Mode::Stereo;
    vr.screenDistance = config.GetFloat("VR", "ScreenDistance", vr.screenDistance);
    vr.screenWidth = config.GetFloat("VR", "ScreenWidth", vr.screenWidth);
    vr.recenterKey = config.GetInt("VR", "RecenterKey", vr.recenterKey);
    vr.uiDistance = config.GetFloat("VR", "UiDistance", vr.uiDistance);
    vr.uiWidth = config.GetFloat("VR", "UiWidth", vr.uiWidth);
    vr.uiHeight = config.GetFloat("VR", "UiHeight", vr.uiHeight);
    vr.menuDistance = config.GetFloat("VR", "MenuDistance", vr.menuDistance);
    vr.menuWidth = config.GetFloat("VR", "MenuWidth", vr.menuWidth);
    vr.appName = "Remember Me VR";
    compositor_.Configure(vr);
    // Stereo frames from the game's scene image (internal, e.g. supersampled,
    // resolution) instead of the window-sized backbuffer.
    const bool sceneSource = config.GetString("VR", "StereoSource", "scene") == "scene";
    vrcore::scene_tap::Enable(vrEnabled_ && sceneSource);
    // Stereo frames: UI (laid out in the left view's rect) to its own panel.
    vrcore::scene_tap::EnableUiRedirect(vrEnabled_ && sceneSource && config.GetBool("VR", "UiPanel", true));
    // ...and still draw it into the window, so the monitor mirror shows the UI.
    vrcore::scene_tap::EnableUiMirror(config.GetBool("VR", "MirrorUi", true));
    rm::mirror::Configure(config);
    ue3Discover_ = config.GetBool("UE3", "Discover", true);
    ue3DumpOnStart_ = config.GetBool("UE3", "DumpObjectsOnStart", true);
    ue3DumpKey_ = config.GetInt("UE3", "DumpObjectsKey", 0x78);  // F9
    engineHooks_ = config.GetBool("Stereo", "EngineHooks", true);
    disableMotionBlur_ = config.GetBool("VR", "DisableMotionBlur", true);
    sceneViews_.Configure(config, vrEnabled_ ? &compositor_ : nullptr);
    // Before the gamepad hook: the free camera needs it too.
    rm::free_cam::Configure(config);
    rm::input_capture::Configure(config);
    rm::input_capture::SetNameLookup(&NameOf);
    rm::vr_gamepad::Configure(config, vrEnabled_ ? &compositor_ : nullptr);
    // As early as possible: the game may look for gamepads before the device exists.
    rm::vr_gamepad::Install(/*early=*/true);
    rm::renderer_hooks::Configure(config);
    rm::sensen_stereo::Configure(config);
    rm::draw_fixes::Configure(config);
    rm::hide_actors::Configure(config);
    rm::menu::Configure(config, {config, vrEnabled_ ? &compositor_ : nullptr, &sceneViews_, &disableMotionBlur_});
    deviceState_.fixDneResolution = config.GetBool("Stereo", "FixDNEResolution", true);
    VR_LOG_INFO("VR %s: mode %s, screen fallback %.1f m wide at %.1f m, recenter key 0x%02x",
                vrEnabled_ ? "on" : "off", vr.mode == vrcore::Compositor::Mode::Stereo ? "stereo" : "screen",
                vr.screenWidth, vr.screenDistance, vr.recenterKey);
  }

  void OnDeviceCreated(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS& params,
                       const vrcore::DxvkInterop* dxvk) override {
    VR_LOG_INFO("game device: backbuffer %ux%u, %u buffer(s), swap effect %d, %s",
                params.BackBufferWidth, params.BackBufferHeight, params.BackBufferCount, params.SwapEffect,
                dxvk ? "DXVK interop available" : "no DXVK interop");
    if (!dxvk) VR_LOG_WARN("VR output needs DXVK's d3d9.dll (Proton's default, or dxvk next to the exe on Windows)");
    rm::vr_gamepad::Install();
    // The exe is unpacked and the engine initialised by now: map UE3's reflection.
    // Hard-coded engine addresses: only on the build they were found in.
    if (engineHooks_ && !engineHooksInstalled_) {
      if (knownBuild_) {
        engineHooksInstalled_ = sceneViews_.Install();
        rm::vr_gamepad::InstallDeviceRegistration();
        // Stereo fixes at the D3D9 level (right-eye view offset) and the
        // per-eye diagnostics that trace through the same hooks.
        if (engineHooksInstalled_ && (deviceState_.fixDneResolution || rm::renderer_hooks::DiagnosticEnabled() ||
                                      rm::sensen_stereo::Enabled())) {
          rm::device_state::Install(device, deviceState_);
          rm::renderer_hooks::Install();
          rm::sensen_stereo::Install();
          rm::draw_fixes::Install();
        }
      } else {
        VR_LOG_WARN("engine hooks skipped: unknown game build");
      }
    }
    if (ue3Discover_ && !reflection_.Valid() && reflection_.Discover()) {
      reflection_.DiscoverTypes();
      if (engineHooksInstalled_) rm::hide_actors::Init(reflection_);
      if (engineHooksInstalled_) {
        const struct {
          const char* property;
          const char* name;
          bool menu;
        } flags[] = {{"bOnLoadingScreen", "loading screen", false}, {"bMenuPause", "menu", true}};
        // Both always: the config menu can switch them on and off.
        for (const auto& f : flags) {
          int32_t offset = -1;
          uint32_t mask = 0;
          if (reflection_.BoolProperty("Class ExampleGame.ExamplePlayerController", f.property, offset, mask)) {
            sceneViews_.AddFlatFlag(f.name, offset, mask, f.menu);
          } else {
            VR_LOG_WARN("ExamplePlayerController.%s not found; %ss stay stereo", f.property, f.name);
          }
        }
      }
      if (ue3DumpOnStart_) DumpObjects("start");
    }
  }

  void OnBeforeReset(IDirect3DDevice9*) override {
    compositor_.OnDeviceChanged();
    rm::mirror::ReleaseDeviceResources();
    rm::menu::ReleaseDeviceResources();
  }

  void OnPresent(IDirect3DDevice9* device) override {
    if (ue3DumpKey_) {
      const bool down = (GetAsyncKeyState(ue3DumpKey_) & 0x8000) != 0;
      if (down && !dumpKeyDown_ && reflection_.Valid()) DumpObjects("hotkey");
      dumpKeyDown_ = down;
    }
    if (engineHooksInstalled_ && sceneViews_.ToggleKey()) {
      const bool down = (GetAsyncKeyState(sceneViews_.ToggleKey()) & 0x8000) != 0;
      if (down && !sbsKeyDown_) {
        sceneViews_.ToggleSideBySide();
        rm::renderer_hooks::Rearm();
      }
      sbsKeyDown_ = down;
    }
    if (engineHooksInstalled_) UpdateMotionBlur();
    rm::renderer_hooks::ViewRect left{}, right{};
    const bool stereoFrame = rm::renderer_hooks::StereoViewRects(left, right);
    rm::sensen_stereo::EndFrame();
    rm::renderer_hooks::EndFrame();
    // Config menu (if open): rendered before the compositor, which shows it on
    // its own panel, and drawn into the window after the mirror.
    rm::input_capture::Poll();
    IDirect3DTexture9* menu = rm::menu::Render(device);
    if (vrEnabled_) {
      IDirect3DTexture9* scene = vrcore::scene_tap::Take();
      IDirect3DTexture9* ui = vrcore::scene_tap::TakeUi();
      compositor_.OnPresent(device, scene, ui, menu);
      if (scene) scene->Release();
      if (ui) ui->Release();
      // After the compositor copied the eyes (possibly from the backbuffer).
      rm::mirror::OnPresent(device, stereoFrame && compositor_.StereoActive());
    }
    rm::menu::DrawToWindow(device);
    ++frames_;
    if (presentLogInterval_ > 0 && frames_ % static_cast<uint64_t>(presentLogInterval_) == 0) {
      VR_LOG_DEBUG("present #%" PRIu64, frames_);
    }
  }

  void OnDetach() override { VR_LOG_INFO("frames presented: %" PRIu64, frames_); }

 private:
  // GSystemSettings.bAllowMotionBlur = false, kept that way every frame (the
  // game may reapply its settings, e.g. after a resolution change).
  // While switched off (also from the config menu) the game's value is
  // remembered and put back when it's switched on again.
  void UpdateMotionBlur() {
    const uintptr_t address =
        reinterpret_cast<uintptr_t>(vrcore::Exe().FromPreferredVA(rm::kSystemSettings)) + rm::kSystemSettingsAllowMotionBlur;
    uint32_t value = 0;
    if (!vrcore::mem::Read(address, value)) return;
    if (!disableMotionBlur_.load()) {
      if (motionBlurForced_) {
        motionBlurForced_ = false;
        *reinterpret_cast<volatile uint32_t*>(address) = motionBlurOriginal_;
        VR_LOG_INFO("motion blur: restored the game's setting (%u)", motionBlurOriginal_);
      }
      return;
    }
    if (!motionBlurForced_) {
      motionBlurForced_ = true;
      motionBlurOriginal_ = value;
      if (!motionBlurLogged_) {
        motionBlurLogged_ = true;
        VR_LOG_INFO("motion blur: GSystemSettings.bAllowMotionBlur was %u; disabled", value);
      }
    } else if (value != 0) {
      motionBlurOriginal_ = value;  // the game reapplied its settings: remember what it wants
    }
    if (value != 0) *reinterpret_cast<volatile uint32_t*>(address) = 0;
  }

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
  static inline RememberMe* instance_ = nullptr;
  // FName text for the free camera's input hooks ("" until reflection is mapped).
  static std::string NameOf(int32_t index) {
    return instance_ && instance_->reflection_.Valid() ? instance_->reflection_.Name(index) : std::string();
  }
  bool engineHooks_ = true;
  std::atomic<bool> disableMotionBlur_{true};
  bool motionBlurLogged_ = false;
  bool motionBlurForced_ = false;
  uint32_t motionBlurOriginal_ = 1;
  bool engineHooksInstalled_ = false;
  bool sbsKeyDown_ = false;
  rm::SceneViewHooks sceneViews_;
  rm::device_state::Options deviceState_;
  bool vrEnabled_ = true;
  vrcore::Compositor compositor_;
  int presentLogInterval_ = 0;
  uint64_t frames_ = 0;
};

}  // namespace

vrcore::Game& vrcore::GetGame() {
  static RememberMe game;
  return game;
}
