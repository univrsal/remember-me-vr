#pragma once

// Remember Me engine addresses and layouts (Steam build, PE timestamp
// 0x69d8a358). VAs are at the preferred base 0x00400000; see docs/08-engine-map.md.

#include <cstdint>

namespace rm {

struct FVector {
  float x, y, z;
};
struct FRotator {
  int32_t pitch, yaw, roll;  // 65536 units = 360 degrees
};
struct FVector2D {
  float x, y;
};

// ULocalPlayer* this (ECX) + 7 stack args; returns the new FSceneView* (or
// null). Adds the view to family->Views itself. If outProjection and outView are
// both non-null it only writes the matrices there and creates no view.
using CalcSceneViewFn = void*(__attribute__((thiscall)) *)(void* localPlayer, void* family, FVector* viewLocation,
                                                          FRotator* viewRotation, void* viewport, void* drawer,
                                                          void* outProjection, void* outView);
constexpr uint32_t kCalcSceneView = 0x008622B0;
constexpr uint8_t kCalcSceneViewPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x6A, 0xFF};

// APlayerController::eventGetPlayerViewPoint thunk: (this, FVector*, FRotator*).
using GetPlayerViewPointFn = void(__attribute__((thiscall)) *)(void* controller, FVector* location, FRotator* rotation);
constexpr uint32_t kGetPlayerViewPoint = 0x006A49F0;
constexpr uint8_t kGetPlayerViewPointPrologue[] = {0x83, 0xEC, 0x18, 0x53, 0x55, 0x56, 0x8B, 0x74, 0x24, 0x28};

// ULocalPlayer fields.
constexpr uint32_t kLocalPlayerActor = 0x40;      // PlayerController*
constexpr uint32_t kLocalPlayerOrigin = 0x6C;     // FVector2D, viewport fraction
constexpr uint32_t kLocalPlayerSize = 0x74;       // FVector2D, viewport fraction
constexpr uint32_t kLocalPlayerViewState = 0x8C;  // FSceneViewState*

// FSceneViewFamily: first member is TArray<FSceneView*> Views {data, num, max}.
constexpr uint32_t kFamilyViewsNum = 0x4;

}  // namespace rm
