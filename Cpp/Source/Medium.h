#pragma once

// Priority medium mode for the CPU tracer.
//
// When enabled, each dielectric sphere additionally carries an integer
// priority, an index of refraction (eta) and an RGB absorption coefficient
// (sigma). At any point in space the active set contains all spheres that
// contain the point; its owner is the active sphere with the highest
// priority, ties broken by the smallest original sphere index. The region
// outside every sphere is air (eta = 1, sigma = 0, owner id -1).
//
// The mode is explicitly opt-in: until a medium configuration is installed
// with SetMediumConfig, MediumEnabled() is false and the production tracer
// keeps its original single-sphere dielectric behaviour, random stream and
// scene description byte layout. ClearMediumConfig() restores that state.

#include "Maths.h"
#include <vector>
#include <cstdint>

namespace medium {

struct MediumSphere
{
    int priority = 0;
    float eta = 1.0f;
    float3pack sigma = float3(0, 0, 0);
};

// CPU-side scene material description, mirroring the fields of the legacy
// Test.cpp Material without changing that struct's GPU layout.
struct SceneMaterialDesc
{
    int type;                 // 0 Lambert, 1 Metal, 2 Dielectric
    float3pack albedo;
    float3pack emissive;
    float roughness;
    float ri;
};

struct MediumConfig
{
    std::vector<MediumSphere> spheres; // one entry per production sphere index
    float3pack sky = float3(0.15f, 0.21f, 0.3f); // constant environment radiance
};

// Number of dielectric entries in the currently installed configuration;
// zero when the mode is off.
int MediumCount();
bool MediumEnabled();

// Validates cfg against an expected sphere count: one entry per sphere,
// finite eta >= 1, finite non-negative per-channel sigma, finite sky.
bool ValidateConfig(const MediumConfig& cfg, int expectedCount);

// CPU scene setup: atomically swaps the production spheres/materials and
// installs the medium configuration. Validation happens before any scene
// state is touched, so an invalid request returns false and leaves both the
// scene and the medium mode unchanged. Use ResetToDefaultScene() to restore
// the built-in scene and turn the mode off again.
bool InstallScene(const std::vector<Sphere>& spheres,
                  const std::vector<SceneMaterialDesc>& mats,
                  const MediumConfig& cfg);

// Copies cfg into the global configuration. The current production sphere
// count must match cfg.spheres.size(); returns false and leaves the
// previously installed configuration untouched on any invalid entry.
bool SetMediumConfig(const MediumConfig& cfg);

// Restores the default (mode disabled) state.
void ClearMediumConfig();

// Constant environment radiance used while the mode is enabled.
float3 MediumSky();

// Lookups for a production sphere index. Air outside every sphere uses
// owner id -1 with eta 1 and zero absorption.
float SphereEta(int id);
float3 SphereSigma(int id);

// True if the point is strictly inside sphere id (a point exactly on the
// surface is not inside; tangential crossings never enter a region).
bool PointInside(int id, const float3& p);

// Initial active set for a ray: every sphere strictly containing its origin.
std::vector<int> InitialActive(const Ray& r);

// Owner of an active set: highest priority, smallest sphere id on ties.
// The empty set (air) has owner -1.
int OwnerOf(const std::vector<int>& active);

// Applies the entry/exit of sphere id at the next crossing to active,
// returning the hypothetical post-crossing set. The current set is not
// modified.
std::vector<int> CrossedSet(const std::vector<int>& active, int id, bool entering);

// Distance to the nearest sphere boundary ahead of the ray. Returns false
// when no sphere is hit within (tMin, tMax). On a hit, outID is the sphere
// index and outEntering reports whether the ray passes into that sphere.
bool NearestBoundary(const Ray& r, float tMin, float tMax, int& outID, bool& outEntering, float& outT);

// Exact unpolarised Fresnel reflectance for an interface from etaI to etaT.
// cosI is the cosine between the (incoming) ray direction and the surface
// normal oriented against the ray. Returns 1 on total internal reflection.
float Fresnel(float cosI, float etaI, float etaT);

// Snell refraction through a surface normal oriented against the incoming
// ray. Returns false on total internal reflection.
bool RefractDir(const float3& dirIn, const float3& normalAgainst, float etaI, float etaT, float3& outDir);

// Spectral Beer-Lambert attenuation over a world distance d inside owner.
float3 Absorption(int owner, float d);

// A single sphere boundary crossing of a ray, sorted by distance.
struct Crossing
{
    float t;
    int id;
    bool entering;
};

// All strictly positive-discriminant crossings of the ray, sorted from near
// to far. Tangent rays (discriminant == 0) produce no crossing.
std::vector<Crossing> EnumerateCrossings(const Ray& r, float tMin, float tMax);

// Result of advancing a primary ray one segment in medium mode.
struct SegmentResult
{
    float distance = 0.0f;   // world distance consumed by this segment
    float3 transmittance = float3(1, 1, 1); // piecewise Beer-Lambert attenuation over it
    int boundaryID = -1;     // sphere crossed at the segment end (-1 if none)
    bool entering = false;   // entry vs exit of boundaryID
    bool ownerChanged = false; // whether the crossing changes the owning medium
    int ownerBefore = -1;
    int ownerAfter = -1;
    float etaBefore = 1.0f;
    float etaAfter = 1.0f;
    float3 normalAgainst = float3(0, 0, 1); // surface normal oriented against ray direction
    bool hitSurface = false; // segment ends on any production sphere
    int hitID = -1;          // production sphere id at the segment end
    Hit hit;                 // hit record at the segment end
    std::vector<int> activeBeforeSurface; // membership when the segment started
    std::vector<int> activeAtSurface;     // membership at the end point
};

// Finds the next real crossing relative to the ray origin. Hidden crossings
// (boundaries that do not change the owner) are consumed internally and
// reported cumulatively through outHiddenCount; they do not consume random
// numbers or scattering depth. Returns false only when no boundary exists
// before tMax: the segment then runs to tMax (or the sky, conceptually).
bool NextSegment(const Ray& r, const std::vector<int>& initialActive, float tMin, float tMax,
                 SegmentResult& outSeg, int& inoutHiddenCount);

// Beer-Lambert attenuation along an unoccluded light/shadow segment of
// length tEnd starting at from in direction unitDir. The active set at the
// segment start is rebuilt from the origin, so hidden boundaries split the
// optical path correctly without ever adding absorptions together.
float3 ShadowAttenuation(const float3& from, const float3& unitDir, float tEnd,
                         const std::vector<int>& activeAtOrigin);

// Counters for regression tests: hidden boundaries never consume scattering
// depth and real scatter events do.
struct TraceDebugStats
{
    unsigned scatterEvents = 0;
    unsigned hiddenBoundaries = 0;
};

// Single-ray radiance entry point used by the production Trace while medium
// mode is on, and directly by the regression tests. ownerPath, when non-null,
// receives the owning medium id before every real scattering event.
float3 TraceMedium(const Ray& r, int depth, int& inoutRayCount, uint32_t& state,
                   TraceDebugStats* stats = nullptr, std::vector<int>* ownerPath = nullptr);

// Installs the nine-sphere "small ball" scene together with a valid medium
// configuration so the mode can be exercised through DrawTest. Returns false
// (without touching the scene) if the configuration would be invalid.
bool LoadSmallBallScene();

// Restores the default production scene and disables medium mode.
void ResetToDefaultScene();

} // namespace medium
