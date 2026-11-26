#pragma once

// Conversions between OpenXR tracking space and UE3 world/view conventions.
//
// OpenXR: right-handed, metres, +X right, +Y up, -Z forward.
// UE3:    left-handed, Unreal units, +X forward, +Y right, +Z up; FRotator in
//         65536ths of a turn (pitch up positive, yaw toward +Y positive).
// UE3 view space (projection input): x right, y up, z forward; matrices are
// row-major and used as row vectors (v * M).

#include <cstdint>

#include <openxr/openxr.h>  // XrPosef/XrFovf only; keeps this host-testable

namespace vrcore::ue3 {

struct Vec {
  float x = 0, y = 0, z = 0;
};
struct Rot {
  int32_t pitch = 0, yaw = 0, roll = 0;
};

enum class BaseOrientation {
  YawOnly,  // keep the horizon level: game camera pitch/roll ignored (default, comfortable)
  Full,     // follow the game camera's full orientation
};

// Eye camera = game camera (location + orientation, reduced per `base`)
// combined with the tracked eye pose (relative to the recentred LOCAL space).
// `unitsPerMeter` scales tracked positions into Unreal units.
void EyeViewPoint(const Vec& baseLocation, const Rot& baseRotation, BaseOrientation base, const XrPosef& eye,
                  float unitsPerMeter, Vec& outLocation, Rot& outRotation);

// Overwrites the x/y terms of a UE3 FPerspectiveMatrix (16 floats, row-major)
// with the asymmetric frustum `fov`, keeping its depth terms.
void ApplyAsymmetricFov(float* matrix, const XrFovf& fov);

}  // namespace vrcore::ue3
