#include "scene_view_hooks.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
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
using vrcore::ue3::Rot;

// Per-eye overrides, active only while our own CalcSceneView calls run (game thread).
struct EyeOverride {
  bool active = false;
  bool vr = false;           // true: absolute eye pose from OpenXR
  bool overrideFov = false;  // true: projection from `fov`
  float testOffset = 0.0f;   // side-by-side test: shift along the view's right axis (uu)
  XrPosef pose{};
  XrFovf fov{};
  bool haveRotation = false;  // set by the view-point hook
  Rot rotation{};             // eye rotation computed there
};

struct State {
  vrcore::Compositor* compositor = nullptr;
  bool vrStereo = true;
  float unitsPerMeter = 50.0f;
  vrcore::ue3::BaseOrientation base = vrcore::ue3::BaseOrientation::YawOnly;

  bool symmetricProjection = false;
  bool testFovOverride = false;
  XrFovf testFov[2]{};

  std::atomic<bool> sideBySide{false};
  float testEyeSeparation = 6.4f;
  RightEyeViewState rightViewState = RightEyeViewState::Own;
  void* ownRightViewState = nullptr;  // allocated once via AllocateViewState, never freed

  int logFirstCalls = 8;
  int logEvery = 600;
  uint64_t calls = 0;
  uint64_t stereoFrames = 0;
  bool loggedSbs = false;
  bool loggedLookAt = false;

  EyeOverride eye;

  // The last per-frame stereo call's left eye, reused (as one view, without a
  // new stereo frame) for calls that build temporary view families.
  bool haveLastLeft = false;
  EyeOverride lastLeft;
  uint64_t tempCalls = 0;
};

State g;
CalcSceneViewFn g_origCalcSceneView = nullptr;
GetPlayerViewPointFn g_origGetPlayerViewPoint = nullptr;
PerspectiveMatrixFn g_origPerspectiveMatrix = nullptr;
PovLookAtFn g_origPovLookAt = nullptr;
void* g_povLookAtReturn = nullptr;
AllocateViewStateFn g_allocateViewState = nullptr;
void* g_mainCallReturn = nullptr;

// Smallest symmetric frustum containing `fov` (diagnostic/workaround for passes
// that assume a centred projection).
XrFovf Symmetric(const XrFovf& fov) {
  const float h = std::max(-fov.angleLeft, fov.angleRight);
  const float v = std::max(fov.angleUp, -fov.angleDown);
  return {-h, h, v, -v};
}

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
    g.eye.rotation = rot;
    g.eye.haveRotation = true;
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
  if (g.eye.active && g.eye.overrideFov && result) vrcore::ue3::ApplyAsymmetricFov(result, g.eye.fov);
  return result;
}

void __fastcall HookPovLookAt(void* pov, void* /*edx*/, float lx, float ly, float lz, float tx, float ty, float tz) {
  void* const caller = __builtin_return_address(0);
  g_origPovLookAt(pov, lx, ly, lz, tx, ty, tz);
  // Only the look-at in CalcSceneView, only during our eye calls. The view
  // matrix is built from (target - location) (0x0040DC60), so move the target
  // onto the eye's forward axis, at the game's target distance (kept for e.g.
  // depth-of-field focus), and put the eye rotation back.
  if (caller != g_povLookAtReturn || !g.eye.active || !g.eye.vr || !g.eye.haveRotation) return;
  auto* words = static_cast<int32_t*>(pov);
  auto* floats = static_cast<float*>(pov);
  words[kPovRotation + 0] = g.eye.rotation.pitch;
  words[kPovRotation + 1] = g.eye.rotation.yaw;
  words[kPovRotation + 2] = g.eye.rotation.roll;
  const float dx = tx - lx, dy = ty - ly, dz = tz - lz;
  const float distance = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 100.0f);
  constexpr float kUnitsToRad = 6.28318530718f / 65536.0f;
  const float pitch = g.eye.rotation.pitch * kUnitsToRad, yaw = g.eye.rotation.yaw * kUnitsToRad;
  floats[kPovTarget + 0] = lx + std::cos(pitch) * std::cos(yaw) * distance;
  floats[kPovTarget + 1] = ly + std::cos(pitch) * std::sin(yaw) * distance;
  floats[kPovTarget + 2] = lz + std::sin(pitch) * distance;
  if (!g.loggedLookAt) {
    g.loggedLookAt = true;
    VR_LOG_INFO("look-at override active: game target %.0f uu away, eye rotation (%d, %d, %d)", distance,
                g.eye.rotation.pitch, g.eye.rotation.yaw, g.eye.rotation.roll);
  }
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
  const bool matricesOnly = outProjection && outView;
  // Temporary view families (screen-bounds checks, DeProject, ...): one view
  // matching the current left eye, so screen-space results agree with View[0];
  // never a new stereo frame (that would queue an extra frame for Present).
  if (__builtin_return_address(0) != g_mainCallReturn && !matricesOnly) {
    if (!g.haveLastLeft) {
      return g_origCalcSceneView(self, family, loc, rot, viewport, drawer, outProjection, outView);
    }
    const FVector2D origin = Field<FVector2D>(self, kLocalPlayerOrigin);
    const FVector2D size = Field<FVector2D>(self, kLocalPlayerSize);
    void* const viewState = Field<void*>(self, kLocalPlayerViewState);
    void* view = CallEye(self, origin.x, size.x * 0.5f, viewState, g.lastLeft, family, loc, rot, viewport, drawer);
    Field<FVector2D>(self, kLocalPlayerOrigin) = origin;
    Field<FVector2D>(self, kLocalPlayerSize) = size;
    if (g.tempCalls++ == 0) {
      VR_LOG_INFO("CalcSceneView from %p (temporary view family %p): left-eye view, no stereo frame",
                  __builtin_return_address(0), family);
    }
    return view;
  }
  const uint64_t call = ++g.calls;
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
    if (!matricesOnly) g.haveLastLeft = false;
    view = g_origCalcSceneView(self, family, loc, rot, viewport, drawer, outProjection, outView);
  } else {
    const float half = size.x * 0.5f;
    EyeOverride left, right;
    if (vr) {
      if (g.symmetricProjection) {
        // Submitted with the same FOV, so the image stays geometrically correct.
        frame.fov[0] = Symmetric(frame.fov[0]);
        frame.fov[1] = Symmetric(frame.fov[1]);
      }
      left.vr = right.vr = true;
      left.overrideFov = right.overrideFov = true;
      left.pose = frame.pose[0];
      left.fov = frame.fov[0];
      right.pose = frame.pose[1];
      right.fov = frame.fov[1];
    } else {
      left.testOffset = -0.5f * g.testEyeSeparation;
      right.testOffset = 0.5f * g.testEyeSeparation;
      if (g.testFovOverride) {  // flat test rendered like the headset (for captures)
        left.overrideFov = right.overrideFov = true;
        left.fov = g.symmetricProjection ? Symmetric(g.testFov[0]) : g.testFov[0];
        right.fov = g.symmetricProjection ? Symmetric(g.testFov[1]) : g.testFov[1];
      }
    }
    view = CallEye(self, origin.x, half, viewState, left, family, loc, rot, viewport, drawer);
    g.lastLeft = left;
    g.haveLastLeft = true;
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
      constexpr float kDeg = 57.29578f;
      VR_LOG_INFO("first VR stereo frame: eye L pos (%.3f, %.3f, %.3f) m, R pos (%.3f, %.3f, %.3f) m, %.0f uu/m",
                  frame.pose[0].position.x, frame.pose[0].position.y, frame.pose[0].position.z,
                  frame.pose[1].position.x, frame.pose[1].position.y, frame.pose[1].position.z, g.unitsPerMeter);
      VR_LOG_INFO("eye FOV (left,right,up,down deg): L %.1f,%.1f,%.1f,%.1f  R %.1f,%.1f,%.1f,%.1f%s",
                  frame.fov[0].angleLeft * kDeg, frame.fov[0].angleRight * kDeg, frame.fov[0].angleUp * kDeg,
                  frame.fov[0].angleDown * kDeg, frame.fov[1].angleLeft * kDeg, frame.fov[1].angleRight * kDeg,
                  frame.fov[1].angleUp * kDeg, frame.fov[1].angleDown * kDeg,
                  g.symmetricProjection ? " (symmetric projection)" : "");
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
  g.symmetricProjection = config.GetBool("VR", "SymmetricProjection", false);
  // Flat test FOV: "game" (the game's own projection) or four angles in degrees
  // for the left eye "left,right,up,down"; the right eye is mirrored.
  const std::string testFov = config.GetString("Stereo", "TestFov", "game");
  float l, r, u, d;
  if (std::sscanf(testFov.c_str(), "%f,%f,%f,%f", &l, &r, &u, &d) == 4) {
    constexpr float kRad = 0.01745329f;
    g.testFov[0] = {l * kRad, r * kRad, u * kRad, d * kRad};
    g.testFov[1] = {-r * kRad, -l * kRad, u * kRad, d * kRad};
    g.testFovOverride = true;
  }
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
  void* povLookAt = exe.FromPreferredVA(kPovLookAt);
  g_povLookAtReturn = exe.FromPreferredVA(kPovLookAtReturnInCalcSceneView);
  g_mainCallReturn = exe.FromPreferredVA(kCalcSceneViewMainReturn);
  auto* allocateViewState = exe.FromPreferredVA<uint8_t>(kAllocateViewState);
  if (!CheckPrologue(calcSceneView, kCalcSceneViewPrologue, sizeof(kCalcSceneViewPrologue), "CalcSceneView") ||
      !CheckPrologue(getPlayerViewPoint, kGetPlayerViewPointPrologue, sizeof(kGetPlayerViewPointPrologue),
                     "GetPlayerViewPoint") ||
      !CheckPrologue(perspectiveMatrix, kPerspectiveMatrixPrologue, sizeof(kPerspectiveMatrixPrologue),
                     "FPerspectiveMatrix") ||
      !CheckPrologue(povLookAt, kPovLookAtPrologue, sizeof(kPovLookAtPrologue), "POV look-at") ||
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
                            reinterpret_cast<void**>(&g_origPerspectiveMatrix)) &&
      vrcore::hook::Install("POV look-at", povLookAt, reinterpret_cast<void*>(&HookPovLookAt),
                            reinterpret_cast<void**>(&g_origPovLookAt));
  VR_LOG_INFO("scene view hooks %s; VR stereo %s (world scale %.0f uu/m, camera orientation %s%s); side-by-side test "
              "%s (toggle key 0x%02x, %s FOV); right view state %s",
              ok ? "installed" : "FAILED", g.vrStereo ? "on" : "off", g.unitsPerMeter,
              g.base == vrcore::ue3::BaseOrientation::Full ? "full" : "yaw only",
              g.symmetricProjection ? ", symmetric projection" : "", g.sideBySide ? "on" : "off", toggleKey_,
              g.testFovOverride ? "headset-like" : "game",
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
