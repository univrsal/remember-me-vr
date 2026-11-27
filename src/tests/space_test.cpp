// Host-side unit test for vrcore/ue3_space (OpenXR <-> UE3 conventions).
// Build and run natively (not cross-compiled), after a configure so the
// OpenXR/Vulkan headers have been fetched:
//
//   meta/tools/space-test.sh

#include <cmath>
#include <cstdio>

#include "vrcore/ue3_space.h"

using namespace vrcore::ue3;

namespace {

int failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  failures += ok ? 0 : 1;
}
bool Near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) < eps; }
bool NearUnits(int32_t a, int32_t b) { return std::abs(a - b) <= 2; }

XrQuaternionf AxisAngle(float x, float y, float z, float degrees) {
  const float h = degrees * 3.14159265f / 360.0f;
  return {x * std::sin(h), y * std::sin(h), z * std::sin(h), std::cos(h)};
}
XrPosef Pose(XrQuaternionf q, XrVector3f p = {0, 0, 0}) { return {q, p}; }
const XrQuaternionf kIdentity{0, 0, 0, 1};
constexpr int32_t kQuarter = 16384;  // 90 degrees in rotator units

}  // namespace

int main() {
  Vec loc;
  Rot rot;

  EyeViewPoint({100, 200, 300}, {0, 0, 0}, BaseOrientation::YawOnly, Pose(kIdentity), 50, loc, rot);
  Check(Near(loc.x, 100) && Near(loc.y, 200) && Near(loc.z, 300) && rot.pitch == 0 && rot.yaw == 0 && rot.roll == 0,
        "identity eye pose leaves the game camera unchanged");

  EyeViewPoint({0, 0, 0}, {0, 0, 0}, BaseOrientation::YawOnly, Pose(kIdentity, {0.032f, 0, 0}), 50, loc, rot);
  Check(Near(loc.x, 0) && Near(loc.y, 1.6f) && Near(loc.z, 0), "eye 3.2 cm right -> UE +Y 1.6 uu at 50 uu/m");

  EyeViewPoint({0, 0, 0}, {0, 0, 0}, BaseOrientation::YawOnly, Pose(kIdentity, {0, 0.1f, -0.2f}), 100, loc, rot);
  Check(Near(loc.x, 20) && Near(loc.y, 0) && Near(loc.z, 10), "eye up 10 cm, forward 20 cm -> UE +Z 10, +X 20");

  EyeViewPoint({0, 0, 0}, {0, kQuarter, 0}, BaseOrientation::YawOnly, Pose(kIdentity, {0.032f, 0, 0}), 50, loc, rot);
  Check(Near(loc.x, -1.6f) && Near(loc.y, 0) && rot.yaw == kQuarter,
        "camera facing +Y: eye right offset -> UE -X, yaw kept");

  EyeViewPoint({0, 0, 0}, {0, 0, 0}, BaseOrientation::YawOnly, Pose(AxisAngle(0, 1, 0, 90)), 50, loc, rot);
  Check(NearUnits(rot.yaw, -kQuarter) && NearUnits(rot.pitch, 0), "head turned 90 deg left -> UE yaw -90");

  EyeViewPoint({0, 0, 0}, {0, 0, 0}, BaseOrientation::YawOnly, Pose(AxisAngle(1, 0, 0, 30)), 50, loc, rot);
  Check(NearUnits(rot.pitch, 5461) && NearUnits(rot.yaw, 0), "head pitched 30 deg up -> UE pitch +30");

  EyeViewPoint({0, 0, 0}, {3000, 1000, 500}, BaseOrientation::YawOnly, Pose(kIdentity), 50, loc, rot);
  Check(rot.pitch == 0 && rot.roll == 0 && NearUnits(rot.yaw, 1000), "yaw-only base drops camera pitch and roll");

  EyeViewPoint({0, 0, 0}, {3000, 1000, 500}, BaseOrientation::Full, Pose(kIdentity), 50, loc, rot);
  Check(NearUnits(rot.pitch, 3000) && NearUnits(rot.yaw, 1000) && NearUnits(rot.roll, 500),
        "full base with identity head reproduces the game rotator (round trip)");

  // Head roll: tilting the head toward the right shoulder (clockwise as seen
  // from behind) is a rotation about XR +Z (backward) by -angle. Positive UE
  // roll is the same direction (right side down).
  EyeViewPoint({0, 0, 0}, {0, 0, 0}, BaseOrientation::YawOnly, Pose(AxisAngle(0, 0, 1, -20)), 50, loc, rot);
  Check(NearUnits(std::abs(rot.roll), 3641) && NearUnits(rot.pitch, 0) && NearUnits(rot.yaw, 0),
        "head roll 20 deg maps to UE roll magnitude 20 deg");
  std::printf("      (head tilted toward right shoulder -> UE roll %+d units)\n", rot.roll);

  // Projection: compare with UE3's FPerspectiveMatrix for a symmetric FOV.
  float m[16] = {};
  ApplyAsymmetricFov(m, {-0.785398f, 0.785398f, 0.785398f, -0.785398f});
  Check(Near(m[0], 1) && Near(m[5], 1) && Near(m[8], 0) && Near(m[9], 0),
        "symmetric 90 deg FOV equals FPerspectiveMatrix (1/tan 45 = 1, no offset)");

  const XrFovf fov{-0.87f, 0.70f, 0.80f, -0.92f};
  ApplyAsymmetricFov(m, fov);
  const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
  const float t = std::tan(fov.angleUp), b = std::tan(fov.angleDown);
  // A view-space point on each frustum edge at z = 1 must land on NDC -1/+1.
  Check(Near(l * m[0] + m[8], -1) && Near(r * m[0] + m[8], 1), "asymmetric FOV: left/right edges -> NDC x -1/+1");
  Check(Near(b * m[5] + m[9], -1) && Near(t * m[5] + m[9], 1), "asymmetric FOV: bottom/top edges -> NDC y -1/+1");

  std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
  return failures ? 1 : 0;
}
