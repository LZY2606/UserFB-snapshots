#pragma once

#include "Maths.h"

// Scene description shared by the CPU renderer and the GPU renderer export.
// The memory layout of this structure is part of the GPU scene format.
struct Material
{
    enum Type { Lambert, Metal, Dielectric };
    Type type;
    float3pack albedo;
    float3pack emissive;
    float roughness;
    float ri;
};

enum TestFlags
{
    kFlagAnimate = (1 << 0),
    kFlagProgressive = (1 << 1),
};

void InitializeTest();
void ShutdownTest();

void UpdateTest(float time, int frameCount, int screenWidth, int screenHeight, unsigned testFlags);
void DrawTest(float time, int frameCount, int screenWidth, int screenHeight, float* backbuffer, int& outRayCount, unsigned testFlags);

// Ray interval used by the tracer. Intersections are on the open interval
// (kMinT, kMaxT).
static const float kMinT = 0.001f;
static const float kMaxT = 1.0e7f;
static const int kMaxDepth = 10;

// Per-ray intersection statistics. These are query-local; no shared global
// counter is incremented by the traversal.
struct CpuHitStats
{
    int nodeVisits;
    int sphereTests;
};

// Total statistics for the most recent DrawTest call (summed per render
// thread, so the traversal itself still has no shared global counter).
struct CpuTraceStats
{
    long long rayCount;
    long long nodeVisits;
    long long sphereTests;
};

// ---- CPU scene interface -------------------------------------------------
//
// The built-in scene (kSphereCount spheres/materials) is used by default.
// SetCpuScene replaces the CPU scene with the given arrays. The arrays are
// copied; the caller keeps ownership and may free them immediately after the
// call. Sphere and material indices are the caller's original indices and are
// preserved by every hit result. `count` must be > 0.
void SetCpuScene(const Sphere* spheres, const Material* materials, int count);

// Empties the CPU scene (a valid scene with zero spheres).
void ClearCpuScene();

// Restores the built-in default scene (the original sphere/material
// descriptions and their original numbering) and rebuilds the acceleration
// structure.
void ResetCpuScene();

// Refits the acceleration structure after moving existing spheres. The sphere
// count and material assignments must be unchanged; this updates sphere data
// and the existing tree's bounds without re-partitioning the topology. The
// arrays are copied and remain owned by the caller. Returns false (and leaves
// the scene untouched) if `count` does not match the current scene.
bool RefitCpuScene(const Sphere* spheres, int count);

// Enables/disables the CPU acceleration structure for HitWorld/OccludedWorld
// (enabled by default). Disabled means exhaustive HitSpheres over the same
// scene, used as the reference for small scenes; it is not a post-pass and
// never runs in addition to the accelerated query.
void SetCpuAccelerationEnabled(bool enabled);
bool GetCpuAccelerationEnabled();

// Closest hit in the open interval (tMin, tMax) against the CPU scene. The
// returned id is the original sphere index (or -1). outStats is optional.
bool HitWorld(const Ray& r, float tMin, float tMax, Hit& outHit, int& outID, CpuHitStats* outStats = 0);

// Any-hit query in the open interval (tMin, tMax); does not find the closest
// sphere. outStats is optional.
bool OccludedWorld(const Ray& r, float tMin, float tMax, CpuHitStats* outStats = 0);

void GetCpuTraceStats(CpuTraceStats& outStats);

void GetObjectCount(int& outCount, int& outObjectSize, int& outMaterialSize, int& outCamSize);
void GetSceneDesc(void* outObjects, void* outMaterials, void* outCam, void* outEmissives, int* outEmissiveCount);
