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

// FPerspectiveMatrix::FPerspectiveMatrix(HalfFOVX, HalfFOVY, MultFOVX, MultFOVY, MinZ, MaxZ):
// writes a row-major 4x4 into `out` (this). View space: x right, y up, z forward.
// M[0][0] = MultX/tan(HalfX), M[1][1] = MultY/tan(HalfY); with MinZ == MaxZ (as
// CalcSceneView calls it) infinite far plane: M[2][2] = 0.999, M[2][3] = 1,
// M[3][2] = -MinZ*0.999. Standard (not reversed) depth. ret 0x18.
using PerspectiveMatrixFn = float*(__attribute__((thiscall)) *)(float* out, float halfFovX, float halfFovY,
                                                                float multFovX, float multFovY, float minZ, float maxZ);
constexpr uint32_t kPerspectiveMatrix = 0x008092B0;
constexpr uint8_t kPerspectiveMatrixPrologue[] = {0xF3, 0x0F, 0x10, 0x54, 0x24, 0x14,
                                                  0xF3, 0x0F, 0x10, 0x4C, 0x24, 0x18};

// DNE camera look-at: (TPOV* this, FVector location, FVector target) by value,
// ret 0x18. Sets POV location/target and recomputes POV *pitch and yaw* from the
// direction to the target, keeping roll. CalcSceneView calls it after
// GetPlayerViewPoint whenever the PlayerCamera has a target (in game: Nilin),
// which overrides any head rotation. POV ints: [3] pitch, [4] yaw, [5] roll.
using PovLookAtFn = void(__attribute__((thiscall)) *)(void* pov, float lx, float ly, float lz, float tx, float ty,
                                                      float tz);
constexpr uint32_t kPovLookAt = 0x008091C0;
constexpr uint8_t kPovLookAtPrologue[] = {0x83, 0xEC, 0x18, 0x8B, 0x44, 0x24, 0x1C, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x28};
// Return address of the call in CalcSceneView (0x008625E4 + 5); other callers are left alone.
constexpr uint32_t kPovLookAtReturnInCalcSceneView = 0x008625E9;
constexpr uint32_t kPovRotation = 3;  // int32 index of pitch; yaw, roll follow
// float index of TargetLocation. CalcSceneView builds the view matrix from
// (TargetLocation - Location) (0x0040DC60), not from the rotation; only roll
// comes from the rotation.
constexpr uint32_t kPovTarget = 7;

// AllocateViewState(): `new FSceneViewState` (0x380 bytes, ctor 0x00A1FEE0), as
// called by the ULocalPlayer constructor (0x008698F0) for LocalPlayer->ViewState.
using AllocateViewStateFn = void* (*)();
constexpr uint32_t kAllocateViewState = 0x00A22B90;
// Bytes 3..6 are a relocatable SEH handler address; check around them.
constexpr uint8_t kAllocateViewStatePrologueA[] = {0x6A, 0xFF, 0x68};
constexpr uint32_t kAllocateViewStatePrologueBOffset = 7;
constexpr uint8_t kAllocateViewStatePrologueB[] = {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x51};

// ULocalPlayer fields.
constexpr uint32_t kLocalPlayerActor = 0x40;      // PlayerController*
constexpr uint32_t kLocalPlayerOrigin = 0x6C;     // FVector2D, viewport fraction
constexpr uint32_t kLocalPlayerSize = 0x74;       // FVector2D, viewport fraction
constexpr uint32_t kLocalPlayerViewState = 0x8C;  // FSceneViewState*

// FSceneViewFamily: first member is TArray<FSceneView*> Views {data, num, max}.
constexpr uint32_t kFamilyViewsNum = 0x4;

// UE3 DownsampleSceneDepth (FSceneRenderer member, renderer in ECX, no stack
// args): per view, renders the half-res depth (FDownsampleSceneDepthPixelShader,
// type object 0x016576C4) with SetParameters inlined: ProjectionScaleBias =
// (View+0xC8, View+0xD8), SourceTexelOffsets from the buffer size, then the
// scene-texture parameters (0x00A48170). Called from 0x00A81590. docs/09.
using DownsampleSceneDepthFn = void(__attribute__((thiscall)) *)(void* renderer);
constexpr uint32_t kDownsampleSceneDepth = 0x00A60D30;
// Bytes 9..12 are a relocatable SEH handler address; check around them.
constexpr uint8_t kDownsampleSceneDepthPrologueA[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x6A, 0xFF, 0x68};
constexpr uint32_t kDownsampleSceneDepthPrologueBOffset = 13;
constexpr uint8_t kDownsampleSceneDepthPrologueB[] = {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50};

// FSceneRenderer: TArray<FViewInfo> Views {data, num, max}; FViewInfo is inline, 0x1210 bytes.
constexpr uint32_t kRendererViewsData = 0x5C;
constexpr uint32_t kRendererViewsNum = 0x60;
constexpr uint32_t kViewInfoStride = 0x1210;
// FSceneView fields (from the draw code above; unconfirmed names in brackets).
constexpr uint32_t kViewRenderTargetX = 0x40;      // int X, Y, SizeX, SizeY
constexpr uint32_t kViewProjectionMatrix = 0xA0;   // [ProjectionMatrix] row-major 4x4 (M[2][2] at 0xC8, M[3][2] at 0xD8)
// Non-zero: the scene colour is bound with sampler 0x01657C3C instead of the
// caller's (0x00A48170). Zeroed per frame in InitViews (0x00A745B0).
constexpr uint32_t kViewSceneColorSamplerSwitch = 0x53C;

}  // namespace rm
