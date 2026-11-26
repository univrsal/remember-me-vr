#include <cmath>

#include "vrcore/ue3_space.h"

namespace vrcore::ue3 {
namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kUnitsToRad = kTwoPi / 65536.0f;

struct Basis {
  Vec f, r, u;  // forward, right, up axes in UE3 world space
};

Vec Add(const Vec& a, const Vec& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec Scale(const Vec& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

// Express a vector given in a basis' local UE3 coordinates (x fwd, y right, z up) in world space.
Vec ToWorld(const Basis& b, const Vec& local) {
  return Add(Add(Scale(b.f, local.x), Scale(b.r, local.y)), Scale(b.u, local.z));
}

// UE3 FRotationMatrix axes for a rotator (radians).
Basis BasisFromRotator(float pitch, float yaw, float roll) {
  const float sp = std::sin(pitch), cp = std::cos(pitch);
  const float sy = std::sin(yaw), cy = std::cos(yaw);
  const float sr = std::sin(roll), cr = std::cos(roll);
  Basis b;
  b.f = {cp * cy, cp * sy, sp};
  b.r = {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
  b.u = {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
  return b;
}

int32_t ToUnits(float radians) { return static_cast<int32_t>(std::lround(radians / kUnitsToRad)); }

// Rotate an OpenXR vector by an OpenXR quaternion.
XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
  // v' = v + 2w(q x v) + 2 q x (q x v)
  const XrVector3f u{q.x, q.y, q.z};
  const XrVector3f t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
  return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
          v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

// OpenXR (x right, y up, z back) -> UE3 local (x forward, y right, z up).
Vec FromXr(const XrVector3f& v) { return {-v.z, v.x, v.y}; }

}  // namespace

void EyeViewPoint(const Vec& baseLocation, const Rot& baseRotation, BaseOrientation base, const XrPosef& eye,
                  float unitsPerMeter, Vec& outLocation, Rot& outRotation) {
  const float pitch = base == BaseOrientation::Full ? baseRotation.pitch * kUnitsToRad : 0.0f;
  const float roll = base == BaseOrientation::Full ? baseRotation.roll * kUnitsToRad : 0.0f;
  const Basis body = BasisFromRotator(pitch, baseRotation.yaw * kUnitsToRad, roll);

  // Eye axes in the body's local frame, then in world space.
  const Basis eyeLocal{FromXr(Rotate(eye.orientation, {0, 0, -1})), FromXr(Rotate(eye.orientation, {1, 0, 0})),
                       FromXr(Rotate(eye.orientation, {0, 1, 0}))};
  const Basis world{ToWorld(body, eyeLocal.f), ToWorld(body, eyeLocal.r), ToWorld(body, eyeLocal.u)};

  outLocation = Add(baseLocation, ToWorld(body, Scale(FromXr(eye.position), unitsPerMeter)));
  // Rotator from axes (inverse of BasisFromRotator).
  outRotation.yaw = ToUnits(std::atan2(world.f.y, world.f.x));
  outRotation.pitch = ToUnits(std::atan2(world.f.z, std::sqrt(world.f.x * world.f.x + world.f.y * world.f.y)));
  outRotation.roll = ToUnits(std::atan2(-world.r.z, world.u.z));
}

void ApplyAsymmetricFov(float* m, const XrFovf& fov) {
  const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
  const float t = std::tan(fov.angleUp), b = std::tan(fov.angleDown);
  // clip.x = x * 2/(r-l) + z * -(r+l)/(r-l); clip.w = z  (row vectors, z forward)
  m[0] = 2.0f / (r - l);
  m[8] = -(r + l) / (r - l);
  m[5] = 2.0f / (t - b);
  m[9] = -(t + b) / (t - b);
}

}  // namespace vrcore::ue3
