#include "scene_view_hooks.h"

#include <atomic>
#include <cmath>
#include <cstring>

#include "engine.h"
#include "vrcore/hook.h"
#include "vrcore/log.h"
#include "vrcore/memory.h"
#include "vrcore/process.h"
#include "vrcore/ue3_space.h"

namespace rm {
namespace {

enum class RightEyeViewState { Own, Shared, None };

// Per-eye overrides, active only while our own CalcSceneView calls run (game thread).
struct EyeOverride {
  bool active = false;
  bool vr = false;           // true: absolute eye pose + projection from OpenXR
  float testOffset = 0.0f;   // side-by-side test: shift along the view's right axis (uu)
  XrPosef pose{};
  XrFovf fov{};
};

struct State {
  vrcore::Compositor* compositor = nullptr;
  bool vrStereo = true;
  float unitsPerMeter = 50.0f;
  vrcore::ue3::BaseOrientation base = vrcore::ue3::BaseOrientation::YawOnly;

  std::atomic<bool> sideBySide{false};
  float testEyeSeparation = 6.4f;
  RightEyeViewState rightViewState = RightEyeViewState::Own;
  void* ownRightViewState = nullptr;  // allocated once via AllocateViewState, never freed

  int logFirstCalls = 8;
  int logEvery = 600;
  uint64_t calls = 0;
  uint64_t stereoFrames = 0;
  bool loggedSbs = false;

  EyeOverride eye;
};

State g;
CalcSceneViewFn g_origCalcSceneView = nullptr;
GetPlayerViewPointFn g_origGetPlayerViewPoint = nullptr;
PerspectiveMatrixFn g_origPerspectiveMatrix = nullptr;
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
  if (!g.eye.active || !location || !rotation) return;
  if (g.eye.vr) {
    vrcore::ue3::Vec loc;
    vrcore::ue3::Rot rot;
    vrcore::ue3::EyeViewPoint({location->x, location->y, location->z}, {rotation->pitch, rotation->yaw, rotation->roll},
                              g.base, g.eye.pose, g.unitsPerMeter, loc, rot);
    *location = {loc.x, loc.y, loc.z};
    *rotation = {rot.pitch, rot.yaw, rot.roll};
  } else {
    // UE3: X forward, Y right, Z up; yaw in 65536ths of a turn. Right = (-sin, cos, 0).
    const float yaw = static_cast<float>(rotation->yaw) * (6.2831853f / 65536.0f);
    location->x += -std::sin(yaw) * g.eye.testOffset;
    location->y += std::cos(yaw) * g.eye.testOffset;
  }
}

float* __fastcall HookPerspectiveMatrix(float* out, void* /*edx*/, float halfFovX, float halfFovY, float multFovX,
                                        float multFovY, float minZ, float maxZ) {
  float* result = g_origPerspectiveMatrix(out, halfFovX, halfFovY, multFovX, multFovY, minZ, maxZ);
  if (g.eye.active && g.eye.vr && result) vrcore::ue3::ApplyAsymmetricFov(result, g.eye.fov);
  return result;
}

void* RightEyeState(void* leftState) {
  switch (g.rightViewState) {
    case RightEyeViewState::Shared:
      return leftState;
    case RightEyeViewState::None:
      return nullptr;
    case RightEyeViewState::Own:
    default:
      // Same call the LocalPlayer constructor uses for its own ViewState (game thread).
      if (!g.ownRightViewState) {
        g.ownRightViewState = g_allocateViewState();
        VR_LOG_INFO("allocated right-eye FSceneViewState %p", g.ownRightViewState);
      }
      return g.ownRightViewState;
  }
}

void* CallEye(void* self, float originX, float sizeX, void* viewState, const EyeOverride& eye, void* family,
              FVector* loc, FRotator* rot, void* viewport, void* drawer) {
  Field<FVector2D>(self, kLocalPlayerOrigin).x = originX;
  Field<FVector2D>(self, kLocalPlayerSize).x = sizeX;
  Field<void*>(self, kLocalPlayerViewState) = viewState;
  g.eye = eye;
  g.eye.active = true;
  void* view = g_origCalcSceneView(self, family, loc, rot, viewport, drawer, nullptr, nullptr);
  g.eye.active = false;
  return view;
}

void* __fastcall HookCalcSceneView(void* self, void* /*edx*/, void* family, FVector* loc, FRotator* rot,
                                   void* viewport, void* drawer, void* outProjection, void* outView) {
  const uint64_t call = ++g.calls;
  const bool matricesOnly = outProjection && outView;
  const FVector2D origin = Field<FVector2D>(self, kLocalPlayerOrigin);
  const FVector2D size = Field<FVector2D>(self, kLocalPlayerSize);
  void* const viewState = Field<void*>(self, kLocalPlayerViewState);
  const int32_t viewsBefore = ViewCount(family);

  vrcore::StereoFrame frame;
  const bool vr = !matricesOnly && g.vrStereo && g.compositor && g.compositor->BeginStereoFrame(frame);
  const bool sbs = !matricesOnly && !vr && g.sideBySide.load(std::memory_order_relaxed);

  void* view;
  void* rightView = nullptr;
  if (!vr && !sbs) {
    view = g_origCalcSceneView(self, family, loc, rot, viewport, drawer, outProjection, outView);
  } else {
    const float half = size.x * 0.5f;
    EyeOverride left, right;
    if (vr) {
      left.vr = right.vr = true;
      left.pose = frame.pose[0];
      left.fov = frame.fov[0];
      right.pose = frame.pose[1];
      right.fov = frame.fov[1];
    } else {
      left.testOffset = -0.5f * g.testEyeSeparation;
      right.testOffset = 0.5f * g.testEyeSeparation;
    }
    view = CallEye(self, origin.x, half, viewState, left, family, loc, rot, viewport, drawer);
    rightView = CallEye(self, origin.x + half, half, RightEyeState(viewState), right, family, loc, rot, viewport,
                        drawer);
    // Everything after us (HUD, audio listener, ...) sees the normal player again.
    Field<FVector2D>(self, kLocalPlayerOrigin) = origin;
    Field<FVector2D>(self, kLocalPlayerSize) = size;
    Field<void*>(self, kLocalPlayerViewState) = viewState;
    if (vr) {
      g.compositor->SubmitStereoFrame(frame);
      ++g.stereoFrames;
    }
  }

  const bool firstStereo = vr && g.stereoFrames == 1;
  const bool log = static_cast<int64_t>(call) <= g.logFirstCalls || (g.logEvery > 0 && call % g.logEvery == 0) ||
                   (sbs && !g.loggedSbs) || firstStereo;
  if (log) {
    VR_LOG_INFO(
        "CalcSceneView #%llu%s: player=%p family=%p views %d->%d viewport=%p drawer=%p outProj=%p outView=%p | "
        "origin=(%.3f,%.3f) size=(%.3f,%.3f) viewState=%p | loc=(%.1f,%.1f,%.1f) rot=(%d,%d,%d) -> view=%p%s",
        static_cast<unsigned long long>(call), vr ? " [VR stereo]" : sbs ? " [side-by-side test]" : "", self, family,
        viewsBefore, ViewCount(family), viewport, drawer, outProjection, outView, origin.x, origin.y, size.x, size.y,
        viewState, loc ? loc->x : 0.f, loc ? loc->y : 0.f, loc ? loc->z : 0.f, rot ? rot->pitch : 0,
        rot ? rot->yaw : 0, rot ? rot->roll : 0, view,
        (vr || sbs) ? (rightView ? " [right view ok]" : " [RIGHT VIEW NULL]") : "");
    if (sbs) g.loggedSbs = true;
    if (firstStereo) {
      VR_LOG_INFO("first VR stereo frame: eye L pos (%.3f, %.3f, %.3f) m, R pos (%.3f, %.3f, %.3f) m, %.0f uu/m",
                  frame.pose[0].position.x, frame.pose[0].position.y, frame.pose[0].position.z,
                  frame.pose[1].position.x, frame.pose[1].position.y, frame.pose[1].position.z, g.unitsPerMeter);
    }
  }
  return view;
}

bool CheckPrologue(void* address, const uint8_t* bytes, size_t size, const char* name) {
  if (vrcore::mem::Matches(reinterpret_cast<uintptr_t>(address), bytes, size)) return true;
  VR_LOG_ERROR("scene view hooks: %s prologue doesn't match this build; not hooking", name);
  return false;
}

}  // namespace

void SceneViewHooks::Configure(const vrcore::Config& config, vrcore::Compositor* compositor) {
  g.compositor = compositor;
  g.vrStereo = config.GetString("VR", "Mode", "stereo") == "stereo";
  g.unitsPerMeter = config.GetFloat("VR", "WorldScale", g.unitsPerMeter);
  g.base = config.GetString("VR", "CameraOrientation", "yaw") == "full" ? vrcore::ue3::BaseOrientation::Full
                                                                       : vrcore::ue3::BaseOrientation::YawOnly;
  g.sideBySide = config.GetBool("Stereo", "TestSideBySide", false);
  g.testEyeSeparation = config.GetFloat("Stereo", "EyeSeparation", g.testEyeSeparation);
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
  void* perspectiveMatrix = exe.FromPreferredVA(kPerspectiveMatrix);
  auto* allocateViewState = exe.FromPreferredVA<uint8_t>(kAllocateViewState);
  if (!CheckPrologue(calcSceneView, kCalcSceneViewPrologue, sizeof(kCalcSceneViewPrologue), "CalcSceneView") ||
      !CheckPrologue(getPlayerViewPoint, kGetPlayerViewPointPrologue, sizeof(kGetPlayerViewPointPrologue),
                     "GetPlayerViewPoint") ||
      !CheckPrologue(perspectiveMatrix, kPerspectiveMatrixPrologue, sizeof(kPerspectiveMatrixPrologue),
                     "FPerspectiveMatrix") ||
      !CheckPrologue(allocateViewState, kAllocateViewStatePrologueA, sizeof(kAllocateViewStatePrologueA),
                     "AllocateViewState") ||
      !CheckPrologue(allocateViewState + kAllocateViewStatePrologueBOffset, kAllocateViewStatePrologueB,
                     sizeof(kAllocateViewStatePrologueB), "AllocateViewState")) {
    return false;
  }
  g_allocateViewState = reinterpret_cast<AllocateViewStateFn>(allocateViewState);
  const bool ok =
      vrcore::hook::Install("ULocalPlayer::CalcSceneView", calcSceneView, reinterpret_cast<void*>(&HookCalcSceneView),
                            reinterpret_cast<void**>(&g_origCalcSceneView)) &&
      vrcore::hook::Install("eventGetPlayerViewPoint", getPlayerViewPoint,
                            reinterpret_cast<void*>(&HookGetPlayerViewPoint),
                            reinterpret_cast<void**>(&g_origGetPlayerViewPoint)) &&
      vrcore::hook::Install("FPerspectiveMatrix", perspectiveMatrix, reinterpret_cast<void*>(&HookPerspectiveMatrix),
                            reinterpret_cast<void**>(&g_origPerspectiveMatrix));
  VR_LOG_INFO("scene view hooks %s; VR stereo %s (world scale %.0f uu/m, camera orientation %s); side-by-side test "
              "%s (toggle key 0x%02x); right view state %s",
              ok ? "installed" : "FAILED", g.vrStereo ? "on" : "off", g.unitsPerMeter,
              g.base == vrcore::ue3::BaseOrientation::Full ? "full" : "yaw only", g.sideBySide ? "on" : "off",
              toggleKey_,
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
