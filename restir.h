#pragma once
//
// restir.h — spatiotemporal RIS (resampled importance sampling) preview path
// for the tinyraytracer scene.
//
// ReservoirRenderer replaces ONLY the direct lighting of the camera's first
// hit surface: per valid first hit it casts at most one shadow ray per frame,
// reusing light candidates found by the previous frame at the same pixel and
// by this frame's 4-neighborhood. Reflection/refraction still run through the
// legacy recursive all-lights path, untouched. The checkerboard ground plane
// (y = -4) is always part of the scene, exactly as in the legacy renderer.
//
// Interface overview:
//
//   restir::SceneView scene;       // spheres + colored point lights (read-only snapshot)
//   restir::Camera camera;         // origin + vertical fov in radians
//   restir::ReservoirRenderer r(scene, camera, width, height, seed, K, historyLimit);
//   const std::vector<restir::Vec3>& frame = r.render_frame(); // RIS frame
//   restir::Reservoir res = r.reservoir(x, y);        // final pool of the last frame
//   restir::FrameStats st = r.last_frame_stats();     // real shadow-ray counts
//   restir::Vec3 ref = r.render_reference_pixel(x, y); // all-lights reference,
//                                                      // same geometry/material code
//
// The renderer owns its history and its random source: rebuilding an instance
// with the same configuration and replaying the same calls reproduces the same
// frames bit for bit. Changing the lights (count, order or definition) clears
// the history, moving the camera keeps it, resizing the frame rebuilds it.
//
// All configuration is validated: non-finite values, non-positive dimensions,
// K < 1, historyLimit < 1 or negative light intensities are rejected with
// std::invalid_argument.

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace restir {

struct Vec3 {
    double x = 0, y = 0, z = 0;
          double& operator[](const int i)       { return i == 0 ? x : (1 == i ? y : z); }
    const double& operator[](const int i) const { return i == 0 ? x : (1 == i ? y : z); }
    Vec3   operator+(const Vec3& v) const { return {x + v.x, y + v.y, z + v.z}; }
    Vec3   operator-(const Vec3& v) const { return {x - v.x, y - v.y, z - v.z}; }
    Vec3   operator-()              const { return {-x, -y, -z}; }
    Vec3   operator*(const double s) const { return {x * s, y * s, z * s}; }
    double dot(const Vec3& v) const { return x * v.x + y * v.y + z * v.z; }
    Vec3   mul(const Vec3& v) const { return {x * v.x, y * v.y, z * v.z}; } // per-channel
    double norm() const { return std::sqrt(dot(*this)); }
    Vec3   normalized() const { return *this * (1.0 / norm()); }
};

struct Material {
    double refractive_index = 1;
    double albedo[4] = {2, 0, 0, 0};
    Vec3 diffuse_color = {0, 0, 0};
    double specular_exponent = 0;
};

struct Sphere {
    Vec3 center;
    double radius = 1;
    Material material;
};

struct PointLight {
    Vec3 position;
    Vec3 intensity = {1, 1, 1}; // RGB, non-negative and finite; legacy lights are (1,1,1)
};

struct Camera {
    Vec3 origin = {0, 0, 0};
    double fov = 1.05; // vertical field of view in radians, rays pass through pixel centers
};

// Read-only snapshot of the scene the renderer was built with.
struct SceneView {
    std::vector<Sphere> spheres;
    std::vector<PointLight> lights;
};

// Read-only snapshot of one pixel's final reservoir after the last render_frame().
struct Reservoir {
    bool hit = false;      // the pixel's first hit exists
    int selected = -1;     // index into SceneView::lights, -1 when the pool is empty
    double S = 0;          // sum of weights
    double M = 0;          // number of candidates the pool represents
    double W = 0;          // S / (M * h_selected)
    double h_selected = 0; // target density of the selected light at the current surface
};

// Real production counters of the last render_frame() call.
struct FrameStats {
    std::uint64_t valid_pixels = 0;          // pixels with a first hit
    std::uint64_t direct_shadow_rays = 0;    // new path: at most one per valid first hit
    std::uint64_t recursive_shadow_rays = 0; // legacy recursion: counted separately
};

class ReservoirRenderer {
public:
    ReservoirRenderer(SceneView scene, Camera camera, int width, int height,
                      std::uint64_t seed, int candidates_per_pixel, int history_limit);
    ~ReservoirRenderer();
    ReservoirRenderer(ReservoirRenderer&&) noexcept;
    ReservoirRenderer& operator=(ReservoirRenderer&&) noexcept;
    ReservoirRenderer(const ReservoirRenderer&) = delete;
    ReservoirRenderer& operator=(const ReservoirRenderer&) = delete;

    const SceneView& scene() const;
    Camera camera() const;
    int width() const;
    int height() const;
    int candidates_per_pixel() const;
    int history_limit() const;

    void set_camera(const Camera& camera); // camera motion keeps the history
    void set_scene(SceneView scene);       // any light change clears the history
    void resize(int width, int height);    // a size change rebuilds the history

    // RIS preview frame: phase 1 (K uniform candidates per valid pixel) frozen,
    // then phase 2 (temporal + spatial reuse, one visibility ray per valid pixel).
    const std::vector<Vec3>& render_frame();

    // Reference entry: sums ALL lights at the first hit through the very same
    // geometry and material logic; deterministic, no reservoirs involved.
    Vec3 render_reference_pixel(int x, int y) const;
    std::vector<Vec3> render_reference_frame() const;

    Reservoir reservoir(int x, int y) const; // final pool of the last render_frame()
    FrameStats last_frame_stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace restir
