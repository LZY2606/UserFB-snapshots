#pragma once

#include <utility>
#include <vector>

#include "geometry.h"
#include "polarization.h"

struct PolarizedSphere {
    vec3 center{0.0f, 0.0f, 0.0f};
    float radius = 1.0f;

    // Refractive indices on the two sides of the sphere surface.  Which one
    // is n1 is decided per hit by the side containing the caller/outgoing
    // reflected ray.
    double n_out = 1.0;
    double n_in = 1.5;
};

struct PolarizedScene {
    std::vector<PolarizedSphere> spheres;
    pol::StokesRGB background{};
    int max_reflections = 4;
};

struct PolarizedTraceResult {
    pol::PolarizationState state{};
    bool hit = false;
    int reflection_events = 0;
};

// Pinhole camera ray matching TinyRayTracer's pixel convention.  D points
// from the camera into the scene; the corresponding light propagation is -D.
// roll rotates the sensor x/y offsets around the viewing z axis and is kept
// as a pure argument so separate traces never share a basis.
vec3 polarized_camera_ray(int x, int y, int width, int height,
                          float fov = 1.05f, double roll = 0.0);

// Deterministic camera reference hint: rolled image-up projected onto the
// plane perpendicular to returned propagation k=-D.
pol::Vec3 polarized_camera_reference(const vec3& D, double roll = 0.0);

class PolarizedRenderer {
public:
    // Public scene/camera/background/budget fields.
    std::vector<PolarizedSphere> spheres;
    vec3 camera_origin{0.0f, 0.0f, 0.0f};
    pol::StokesRGB background{};
    int max_reflections = 4;
    float fov = 1.05f;
    double roll = 0.0;

    PolarizedRenderer() = default;
    explicit PolarizedRenderer(PolarizedScene scene)
        : spheres(std::move(scene.spheres)),
          background(scene.background),
          max_reflections(scene.max_reflections) {}

    vec3 camera_ray(int x, int y, int width, int height) const {
        return polarized_camera_ray(x, y, width, height, fov, roll);
    }

    // Trace one camera ray.  If output_e1 is null the complete Stokes state
    // is returned in the published deterministic world projection basis for
    // k=-D.  Otherwise it is returned in that requested same-direction basis.
    pol::PolarizationState trace(const vec3& D,
                                const pol::Vec3* output_e1 = nullptr) const;

    PolarizedTraceResult trace_with_count(
        const vec3& D, const pol::Vec3* output_e1 = nullptr) const;

    PolarizedTraceResult trace_ray_with_count(
        const vec3& origin, const vec3& D,
        const pol::Vec3* output_e1 = nullptr) const;

    pol::PolarizationState trace_ray(
        const vec3& origin, const vec3& D,
        const pol::Vec3* output_e1 = nullptr) const {
        return trace_ray_with_count(origin, D, output_e1).state;
    }

    PolarizedTraceResult trace_pixel(
        int x, int y, int width, int height,
        const pol::Vec3* output_e1 = nullptr) const;

private:
    PolarizedTraceResult trace_internal(const vec3& origin,
                                        const vec3& D,
                                        int reflections_left) const;
};

// Free entry point for callers that assemble the scene per call.
pol::PolarizationState trace_polarized(
    const PolarizedScene& scene,
    const vec3& origin,
    const vec3& D,
    const pol::Vec3* output_e1 = nullptr);
