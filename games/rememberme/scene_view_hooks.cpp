#include "scene_view_hooks.h"

#include <cmath>
#include <cstring>

#include "engine.h"
#include "vrcore/hook.h"
#include "vrcore/log.h"
#include "vrcore/memory.h"
#include "vrcore/process.h"

namespace rm {
namespace {

enum class RightEyeViewState { Own, Shared, None };

struct State {
  std::atomic<bool> sideBySide{false};
  float eyeSeparation = 6.4f;  // Unreal units between the eyes (test value)
  RightEyeViewState rightViewState = RightEyeViewState::Own;
  void* ownRightViewState = nullptr;  // allocated once via AllocateViewState, never freed
  int logFirstCalls = 8;
  int logEvery = 600;

  // Set only while our own CalcSceneView calls run (game thread).
  bool eyeActive = false;
  float eyeOffset = 0.0f;  // along the view's right axis, Unreal units

  uint64_t calls = 0;
  bool loggedSbs = false;
};

State g;
CalcSceneViewFn g_origCalcSceneView = nullptr;
GetPlayerViewPointFn g_origGetPlayerViewPoint = nullptr;
AllocateViewStateFn g_allocateViewState = nullptr;

template <typename T>
T& Field(void* object, uint32_t offset) {
  return *reinterpret_cast<T*>(static_cast<char*>(object) + offset);
}

int32_t ViewCount(void* family) {
  int32_t n = -1;
  vrcore::mem::Read(reinterpret_cast<uintptr_t>(family) + kFamilyViewsNum, n);
  return n;
}

void __fastcall HookGetPlayerViewPoint(void* controller, void* /*edx*/, FVector* location, FRotator* rotation) {
  g_origGetPlayerViewPoint(controller, location, rotation);
  if (!g.eyeActive || !location || !rotation) return;
  // UE3: X forward, Y right, Z up; yaw in 65536ths of a turn. Right = (-sin, cos, 0).
  const float yaw = static_cast<float>(rotation->yaw) * (6.2831853f / 65536.0f);
  location->x += -std::sin(yaw) * g.eyeOffset;
  location->y += std::cos(yaw) * g.eyeOffset;
}

void* CallEye(void* self, float originX, float sizeX, float offset, void* viewState, void* family, FVector* loc,
              FRotator* rot, void* viewport, void* drawer) {
  Field<FVector2D>(self, kLocalPlayerOrigin).x = originX;
  Field<FVector2D>(self, kLocalPlayerSize).x = sizeX;
  Field<void*>(self, kLocalPlayerViewState) = viewState;
  g.eyeActive = true;
  g.eyeOffset = offset;
  void* view = g_origCalcSceneView(self, family, loc, rot, viewport, drawer, nullptr, nullptr);
  g.eyeActive = false;
  return view;
}

void* __fastcall HookCalcSceneView(void* self, void* /*edx*/, void* family, FVector* loc, FRotator* rot,
                                   void* viewport, void* drawer, void* outProjection, void* outView) {
  const uint64_t call = ++g.calls;
  const bool matricesOnly = outProjection && outView;
  const bool sbs = g.sideBySide.load(std::memory_order_relaxed) && !matricesOnly;
  const FVector2D origin = Field<FVector2D>(self, kLocalPlayerOrigin);
  const FVector2D size = Field<FVector2D>(self, kLocalPlayerSize);
  void* const viewState = Field<void*>(self, kLocalPlayerViewState);
  const int32_t viewsBefore = ViewCount(family);

  void* view;
  void* rightView = nullptr;
  if (!sbs) {
    view = g_origCalcSceneView(self, family, loc, rot, viewport, drawer, outProjection, outView);
  } else {
    const float half = size.x * 0.5f;
    void* rightState = nullptr;
    if (g.rightViewState == RightEyeViewState::Shared) {
      rightState = viewState;
    } else if (g.rightViewState == RightEyeViewState::Own) {
      // Same call the LocalPlayer constructor uses for its own ViewState (game thread).
      if (!g.ownRightViewState) {
        g.ownRightViewState = g_allocateViewState();
        VR_LOG_INFO("allocated right-eye FSceneViewState %p", g.ownRightViewState);
      }
      rightState = g.ownRightViewState;
    }
    view = CallEye(self, origin.x, half, -0.5f * g.eyeSeparation, viewState, family, loc, rot, viewport, drawer);
    rightView = CallEye(self, origin.x + half, half, 0.5f * g.eyeSeparation, rightState, family, loc, rot, viewport,
                        drawer);
    // Everything after us (HUD, audio listener, ...) sees the normal player again.
    Field<FVector2D>(self, kLocalPlayerOrigin) = origin;
    Field<FVector2D>(self, kLocalPlayerSize) = size;
    Field<void*>(self, kLocalPlayerViewState) = viewState;
  }

  const bool log = static_cast<int64_t>(call) <= g.logFirstCalls || (g.logEvery > 0 && call % g.logEvery == 0) ||
                   (sbs && !g.loggedSbs);
  if (log) {
    VR_LOG_INFO(
        "CalcSceneView #%llu: player=%p family=%p views %d->%d viewport=%p drawer=%p outProj=%p outView=%p | "
        "origin=(%.3f,%.3f) size=(%.3f,%.3f) viewState=%p | loc=(%.1f,%.1f,%.1f) rot=(%d,%d,%d) -> view=%p%s",
        static_cast<unsigned long long>(call), self, family, viewsBefore, ViewCount(family), viewport, drawer,
        outProjection, outView, origin.x, origin.y, size.x, size.y, viewState, loc ? loc->x : 0.f, loc ? loc->y : 0.f,
        loc ? loc->z : 0.f, rot ? rot->pitch : 0, rot ? rot->yaw : 0, rot ? rot->roll : 0, view,
        sbs ? (rightView ? " [side-by-side: right view ok]" : " [side-by-side: RIGHT VIEW NULL]") : "");
    if (sbs) g.loggedSbs = true;
  }
  return view;
}

}  // namespace

void SceneViewHooks::Configure(const vrcore::Config& config) {
  g.sideBySide = config.GetBool("Stereo", "TestSideBySide", false);
  g.eyeSeparation = config.GetFloat("Stereo", "EyeSeparation", g.eyeSeparation);
  const std::string rightState = config.GetString("Stereo", "RightEyeViewState", "own");
  g.rightViewState = rightState == "shared" ? RightEyeViewState::Shared
                     : rightState == "none" ? RightEyeViewState::None
                                            : RightEyeViewState::Own;
  g.logFirstCalls = config.GetInt("Stereo", "LogFirstCalls", g.logFirstCalls);
  g.logEvery = config.GetInt("Stereo", "LogEvery", g.logEvery);
  toggleKey_ = config.GetInt("Stereo", "ToggleKey", 0x79);  // F10
}

bool SceneViewHooks::Install() {
  const auto& exe = vrcore::Exe();
  void* calcSceneView = exe.FromPreferredVA(kCalcSceneView);
  void* getPlayerViewPoint = exe.FromPreferredVA(kGetPlayerViewPoint);
  const auto allocateViewState = exe.FromPreferredVA<uint8_t>(kAllocateViewState);
  if (!vrcore::mem::Matches(reinterpret_cast<uintptr_t>(allocateViewState), kAllocateViewStatePrologueA,
                            sizeof(kAllocateViewStatePrologueA)) ||
      !vrcore::mem::Matches(reinterpret_cast<uintptr_t>(allocateViewState + kAllocateViewStatePrologueBOffset),
                            kAllocateViewStatePrologueB, sizeof(kAllocateViewStatePrologueB))) {
    VR_LOG_ERROR("scene view hooks: AllocateViewState prologue doesn't match this build; not hooking");
    return false;
  }
  g_allocateViewState = reinterpret_cast<AllocateViewStateFn>(allocateViewState);
  if (!vrcore::mem::Matches(reinterpret_cast<uintptr_t>(calcSceneView), kCalcSceneViewPrologue,
                            sizeof(kCalcSceneViewPrologue)) ||
      !vrcore::mem::Matches(reinterpret_cast<uintptr_t>(getPlayerViewPoint), kGetPlayerViewPointPrologue,
                            sizeof(kGetPlayerViewPointPrologue))) {
    VR_LOG_ERROR("scene view hooks: function prologues don't match this build; not hooking");
    return false;
  }
  const bool ok =
      vrcore::hook::Install("ULocalPlayer::CalcSceneView", calcSceneView, reinterpret_cast<void*>(&HookCalcSceneView),
                            reinterpret_cast<void**>(&g_origCalcSceneView)) &&
      vrcore::hook::Install("eventGetPlayerViewPoint", getPlayerViewPoint,
                            reinterpret_cast<void*>(&HookGetPlayerViewPoint),
                            reinterpret_cast<void**>(&g_origGetPlayerViewPoint));
  VR_LOG_INFO("scene view hooks %s; side-by-side test %s (toggle key 0x%02x), eye separation %.1f uu, right view state %s",
              ok ? "installed" : "FAILED", g.sideBySide ? "on" : "off", toggleKey_, g.eyeSeparation,
              g.rightViewState == RightEyeViewState::Own      ? "own"
              : g.rightViewState == RightEyeViewState::Shared ? "shared"
                                                              : "none");
  return ok;
}

void SceneViewHooks::ToggleSideBySide() {
  const bool now = !g.sideBySide.load();
  g.sideBySide = now;
  g.loggedSbs = false;
  VR_LOG_INFO("side-by-side test %s", now ? "ON" : "OFF");
}

}  // namespace rm
