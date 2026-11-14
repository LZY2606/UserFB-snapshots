#pragma once
// Spatiotemporal RIS preview path for TinyRayTracer first-hit direct lighting.
//
// Include AFTER legacy.hpp (which provides vec3, Material, Sphere,
// ray_sphere_intersect, reflect, refract and the built-in scene). The
// implementation lives in this header as inline functions because the legacy
// translation unit already defines the non-inline legacy symbols; a separate
// src/restir*.cpp re-including legacy.hpp would duplicate them at link time.
//
// Public interface:
//   restir::PointLight  - position + non-negative finite RGB intensity.
//   restir::SceneView   - spheres + point lights; SceneView::legacy() wraps the
//                         built-in scene with intensity (1,1,1) per light.
//   restir::Camera      - position + fov; rays are generated per pixel center
//                         with the legacy convention.
//   restir::Reservoir   - read-only snapshot: selected light, S, M, W and the
//                         target density h of the selected light.
//   restir::ReservoirRenderer
//     Owns the per-pixel history and the random source; rebuilding it with the
//     same configuration reproduces the same frames.
//       render_frame()      RIS frame. Each first hit runs stage 1 (K uniform
//                           candidates with replacement, weight h/q, q=1/L) and
//                           stage 2 (merge of this frame's reservoir, the
//                           previous frame's final reservoir at the same pixel
//                           truncated to historyLimit candidates, and the four
//                           frozen stage-1 neighbour reservoirs). Exactly one
//                           visibility query is cast per first hit, for the
//                           finally selected light. Reflection/refraction keep
//                           the legacy recursive all-lights evaluation.
//       render_reference()  Sums all lights at the first hit through the same
//                           geometry and material logic; no RNG, no history.
//     Lights changing (count, order or definition) clear the history, camera
//     moves keep it, resizes are done by rebuilding the object. Invalid or
//     non-finite settings are rejected with std::invalid_argument.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <random>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace restir {

struct PointLight {
    vec3 position;
    vec3 intensity{1.f, 1.f, 1.f};
};

struct SceneView {
    std::vector<Sphere> spheres;
    std::vector<PointLight> lights;
    static SceneView legacy() { // built-in scene, old lights map to intensity (1,1,1)
        SceneView view;
        view.spheres.assign(std::begin(::spheres), std::end(::spheres));
        for (const vec3& l : ::lights)
            view.lights.push_back(PointLight{l, vec3{1.f, 1.f, 1.f}});
        return view;
    }
};

struct Camera {
    vec3 position{0.f, 0.f, 0.f};
    float fov = 1.05f;
};

struct Reservoir { // read-only snapshot of real renderer state
    int selected = -1; // index into SceneView::lights, -1 when empty
    double S = 0;      // sum of weights
    double M = 0;      // number of candidates represented
    double W = 0;      // S/(M*h_selected)
    double h = 0;      // target density of the selected light at the current surface
};

namespace detail {

inline const vec3 background{0.2f, 0.7f, 0.8f};

inline bool finite(const vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Same geometry logic as the legacy scene_intersect, over an explicit sphere list.
inline std::tuple<bool, vec3, vec3, Material>
scene_intersect(const std::vector<Sphere>& spheres, const vec3& orig, const vec3& dir) {
    vec3 pt, N;
    Material material;

    float nearest_dist = 1e10;
    if (std::abs(dir.y) > .001) { // intersect the ray with the checkerboard, avoid division by zero
        float d = -(orig.y + 4) / dir.y; // the checkerboard plane has equation y = -4
        vec3 p = orig + dir * d;
        if (d > .001 && d < nearest_dist && std::abs(p.x) < 10 && p.z < -10 && p.z > -30) {
            nearest_dist = d;
            pt = p;
            N = {0, 1, 0};
            material.diffuse_color = (int(.5*pt.x + 1000) + int(.5*pt.z)) & 1 ? vec3{.3, .3, .3} : vec3{.3, .2, .1};
        }
    }

    for (const Sphere& s : spheres) {
        auto [intersection, d] = ::ray_sphere_intersect(orig, dir, s);
        if (!intersection || d > nearest_dist) continue;
        nearest_dist = d;
        pt = orig + dir * nearest_dist;
        N = (pt - s.center).normalized();
        material = s.material;
    }
    return {nearest_dist < 1000, pt, N, material};
}

// Unoccluded single-light RGB at a receiver: the legacy diffuse + Phong terms,
// scaled per channel by the light intensity. No distance falloff, no visibility.
inline vec3 light_contribution(const Material& material, const vec3& N, const vec3& dir,
                               const vec3& point, const PointLight& light) {
    vec3 light_dir = (light.position - point).normalized();
    float diffuse  = std::max(0.f, light_dir * N);
    float specular = std::pow(std::max(0.f, -::reflect(-light_dir, N) * dir), material.specular_exponent);
    vec3 c = material.diffuse_color * diffuse * material.albedo[0]
           + vec3{1.f, 1.f, 1.f} * specular * material.albedo[1];
    return {c.x * light.intensity.x, c.y * light.intensity.y, c.z * light.intensity.z};
}

// Stage-1 target density; the positive floor keeps a shared support across
// surfaces and is never added to the output color.
inline double target_density(const vec3& C) {
    return 0.2126 * C.x + 0.7152 * C.y + 0.0722 * C.z + 1e-6;
}

// Legacy finite shadow ray.
inline bool occluded(const std::vector<Sphere>& spheres, const vec3& point, const vec3& lightPos) {
    vec3 light_dir = (lightPos - point).normalized();
    auto [hit, shadow_pt, trashnrm, trashmat] = scene_intersect(spheres, point, light_dir);
    return hit && (shadow_pt - point).norm() < (lightPos - point).norm();
}

// The legacy recursive path: per bounce it sums every light with its own shadow
// ray, exactly like the original cast_ray. Used for reflection/refraction and
// by the reference entry; never reuses screen-space neighbourhoods.
inline vec3 cast_ray(const SceneView& view, const vec3& orig, const vec3& dir, int depth,
                     std::uint64_t& shadow_rays) {
    auto [hit, point, N, material] = scene_intersect(view.spheres, orig, dir);
    if (depth > 4 || !hit)
        return background;

    vec3 reflect_dir = ::reflect(dir, N).normalized();
    vec3 refract_dir = ::refract(dir, N, material.refractive_index).normalized();
    vec3 reflect_color = cast_ray(view, point, reflect_dir, depth + 1, shadow_rays);
    vec3 refract_color = cast_ray(view, point, refract_dir, depth + 1, shadow_rays);

    vec3 direct{0, 0, 0};
    for (const PointLight& light : view.lights) {
        ++shadow_rays;
        if (occluded(view.spheres, point, light.position)) continue;
        direct = direct + light_contribution(material, N, dir, point, light);
    }
    return direct + reflect_color * material.albedo[2] + refract_color * material.albedo[3];
}

// Legacy pixel-center camera ray.
inline vec3 camera_ray(const Camera& cam, int width, int height, int x, int y) {
    float dir_x =  (x + 0.5) -  width / 2.;
    float dir_y = -(y + 0.5) +  height / 2.; // this flips the image at the same time
    float dir_z = -height / (2. * std::tan(double(cam.fov) / 2.));
    return vec3{dir_x, dir_y, dir_z}.normalized();
}

} // namespace detail

class ReservoirRenderer {
public:
    ReservoirRenderer(SceneView scene, Camera camera, int width, int height,
                      int candidates, int historyLimit, std::uint64_t seed)
        : scene_(std::move(scene)), camera_(camera), width_(width), height_(height),
          candidates_(candidates), historyLimit_(historyLimit), rng_(seed) {
        if (width_ <= 0)        throw std::invalid_argument("restir: width must be positive");
        if (height_ <= 0)       throw std::invalid_argument("restir: height must be positive");
        if (candidates_ < 1)    throw std::invalid_argument("restir: candidate count K must be positive");
        if (historyLimit_ < 1)  throw std::invalid_argument("restir: historyLimit must be positive");
        validate_camera(camera_);
        validate_scene(scene_);
        const size_t n = size_t(width_) * size_t(height_);
        history_.assign(n, Reservoir{});
        phase1_.assign(n, Reservoir{});
        final_.assign(n, Reservoir{});
        hits_.assign(n, HitInfo{});
        framebuffer_.assign(n, vec3{});
        reference_.assign(n, vec3{});
    }

    // RIS preview frame; updates the temporal history.
    const std::vector<vec3>& render_frame() {
        directShadow_ = 0;
        recursiveShadow_ = 0;
        frameHits_ = 0;
        const size_t L = scene_.lights.size();

        // Stage 1: per valid pixel, K uniform candidates with replacement.
        for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x) {
            const size_t i = size_t(y) * size_t(width_) + size_t(x);
            HitInfo hi;
            hi.dir = detail::camera_ray(camera_, width_, height_, x, y);
            auto [hit, point, N, material] = detail::scene_intersect(scene_.spheres, camera_.position, hi.dir);
            hi.hit = hit; hi.point = point; hi.N = N; hi.material = material;
            if (hit) {
                ++frameHits_;
                const vec3 reflect_dir = ::reflect(hi.dir, N).normalized();
                const vec3 refract_dir = ::refract(hi.dir, N, material.refractive_index).normalized();
                hi.reflect_color = detail::cast_ray(scene_, point, reflect_dir, 1, recursiveShadow_);
                hi.refract_color = detail::cast_ray(scene_, point, refract_dir, 1, recursiveShadow_);
            }
            hits_[i] = hi;
            Reservoir r;
            if (hit && L > 0) { // no lights: direct light is zero, no empty uniform distribution
                std::uniform_int_distribution<size_t> pick(0, L - 1);
                for (int k = 0; k < candidates_; ++k) {
                    const size_t j = pick(rng_);
                    const double h = detail::target_density(
                        detail::light_contribution(material, N, hi.dir, point, scene_.lights[j]));
                    const double w = h * double(L); // h/q with q = 1/L
                    r.S += w;
                    r.M += 1; // zero-contribution candidates still count
                    if (uni_(rng_) < w / r.S) r.selected = int(j);
                }
                finalize(r, hi);
            }
            phase1_[i] = r;
            if (!hit) framebuffer_[i] = detail::background;
        }

        // Stage 2: merge this frame's reservoir, the truncated previous-frame
        // reservoir and the four frozen stage-1 neighbour reservoirs.
        static const int dx[4] = {-1, 1, 0, 0};
        static const int dy[4] = { 0, 0,-1, 1};
        for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x) {
            const size_t i = size_t(y) * size_t(width_) + size_t(x);
            const HitInfo& hi = hits_[i];
            if (!hi.hit) { final_[i] = Reservoir{}; continue; }
            Reservoir f;
            merge(f, phase1_[i], phase1_[i].M, hi);                                    // this frame
            merge(f, history_[i], std::min(history_[i].M, double(historyLimit_)), hi); // previous frame
            for (int n = 0; n < 4; ++n) {                                              // frozen neighbours
                const int nx = x + dx[n], ny = y + dy[n];
                if (nx < 0 || ny < 0 || nx >= width_ || ny >= height_) continue;       // out of range: skipped
                const Reservoir& src = phase1_[size_t(ny) * size_t(width_) + size_t(nx)];
                merge(f, src, src.M, hi);
            }
            vec3 direct{0, 0, 0};
            if (f.selected >= 0) {
                finalize(f, hi); // h and C are re-evaluated on the current surface
                ++directShadow_; // the single visibility query of this first hit
                const PointLight& light = scene_.lights[size_t(f.selected)];
                if (!detail::occluded(scene_.spheres, hi.point, light.position))
                    direct = detail::light_contribution(hi.material, hi.N, hi.dir, hi.point, light)
                           * float(f.W); // C*visibility*S/(M*h), no extra 1/K
            }
            framebuffer_[i] = direct + hi.reflect_color * hi.material.albedo[2]
                                     + hi.refract_color * hi.material.albedo[3];
            final_[i] = f;
        }
        history_.swap(final_); // final_ is fully rewritten on the next frame
        return framebuffer_;
    }

    // Reference entry: first-hit direct light sums every light through the same
    // geometry and material logic. No RNG, no history.
    const std::vector<vec3>& render_reference() {
        referenceShadow_ = 0;
        for (int y = 0; y < height_; ++y)
        for (int x = 0; x < width_; ++x) {
            const size_t i = size_t(y) * size_t(width_) + size_t(x);
            const vec3 dir = detail::camera_ray(camera_, width_, height_, x, y);
            auto [hit, point, N, material] = detail::scene_intersect(scene_.spheres, camera_.position, dir);
            if (!hit) { reference_[i] = detail::background; continue; }
            const vec3 reflect_dir = ::reflect(dir, N).normalized();
            const vec3 refract_dir = ::refract(dir, N, material.refractive_index).normalized();
            const vec3 reflect_color = detail::cast_ray(scene_, point, reflect_dir, 1, referenceShadow_);
            const vec3 refract_color = detail::cast_ray(scene_, point, refract_dir, 1, referenceShadow_);
            vec3 direct{0, 0, 0};
            for (const PointLight& light : scene_.lights) {
                ++referenceShadow_;
                if (!detail::occluded(scene_.spheres, point, light.position))
                    direct = direct + detail::light_contribution(material, N, dir, point, light);
            }
            reference_[i] = direct + reflect_color * material.albedo[2]
                                  + refract_color * material.albedo[3];
        }
        return reference_;
    }

    void set_camera(const Camera& camera) { // camera moves keep the history
        validate_camera(camera);
        camera_ = camera;
    }

    void set_scene(SceneView scene) { // changed lights clear the history
        validate_scene(scene);
        if (!same_lights(scene_.lights, scene.lights))
            std::fill(history_.begin(), history_.end(), Reservoir{});
        scene_ = std::move(scene);
    }

    const std::vector<vec3>& framebuffer() const { return framebuffer_; }
    const std::vector<vec3>& reference_framebuffer() const { return reference_; }

    Reservoir reservoir(int x, int y) const { return history_.at(index(x, y)); }      // final reservoir of the last frame
    Reservoir phase1_reservoir(int x, int y) const { return phase1_.at(index(x, y)); } // frozen stage-1 reservoir
    bool first_hit(int x, int y) const { return hits_.at(index(x, y)).hit; }

    std::uint64_t direct_shadow_rays() const { return directShadow_; }       // last render_frame
    std::uint64_t recursive_shadow_rays() const { return recursiveShadow_; } // last render_frame
    std::uint64_t reference_shadow_rays() const { return referenceShadow_; } // last render_reference
    std::uint64_t last_frame_hits() const { return frameHits_; }

    int width() const { return width_; }
    int height() const { return height_; }
    int candidates() const { return candidates_; }
    int history_limit() const { return historyLimit_; }

private:
    struct HitInfo {
        bool hit = false;
        vec3 point, N, dir;
        Material material;
        vec3 reflect_color, refract_color;
    };

    size_t index(int x, int y) const {
        if (x < 0 || y < 0 || x >= width_ || y >= height_)
            throw std::out_of_range("restir: pixel out of range");
        return size_t(y) * size_t(width_) + size_t(x);
    }

    static void validate_camera(const Camera& camera) {
        if (!detail::finite(camera.position))
            throw std::invalid_argument("restir: camera position must be finite");
        if (!std::isfinite(camera.fov) || camera.fov <= 0.f || camera.fov >= 3.14159265f)
            throw std::invalid_argument("restir: camera fov must be finite and in (0, pi)");
    }

    static void validate_scene(const SceneView& scene) {
        for (const PointLight& l : scene.lights) {
            if (!detail::finite(l.position) || !detail::finite(l.intensity))
                throw std::invalid_argument("restir: light position/intensity must be finite");
            if (l.intensity.x < 0 || l.intensity.y < 0 || l.intensity.z < 0)
                throw std::invalid_argument("restir: light intensity must be non-negative");
        }
        for (const Sphere& s : scene.spheres) {
            if (!detail::finite(s.center) || !std::isfinite(s.radius) || s.radius <= 0)
                throw std::invalid_argument("restir: spheres must be finite with positive radius");
        }
    }

    static bool same_lights(const std::vector<PointLight>& a, const std::vector<PointLight>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            const vec3& ap = a[i].position; const vec3& bp = b[i].position;
            const vec3& ai = a[i].intensity; const vec3& bi = b[i].intensity;
            if (ap.x != bp.x || ap.y != bp.y || ap.z != bp.z ||
                ai.x != bi.x || ai.y != bi.y || ai.z != bi.z) return false;
        }
        return true;
    }

    // One super-candidate per source: weight h_current(selected)*W_source*m.
    void merge(Reservoir& dst, const Reservoir& src, double m, const HitInfo& hi) {
        if (src.selected < 0 || m <= 0) return; // empty or missed sources never join
        const double h = detail::target_density(detail::light_contribution(
            hi.material, hi.N, hi.dir, hi.point, scene_.lights[size_t(src.selected)]));
        const double w = h * src.W * m;
        dst.S += w;
        dst.M += m;
        if (uni_(rng_) < w / dst.S) dst.selected = src.selected;
    }

    void finalize(Reservoir& r, const HitInfo& hi) {
        const double h = detail::target_density(detail::light_contribution(
            hi.material, hi.N, hi.dir, hi.point, scene_.lights[size_t(r.selected)]));
        r.h = h;
        r.W = r.S / (r.M * h);
    }

    SceneView scene_;
    Camera camera_;
    int width_, height_, candidates_, historyLimit_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uni_{0.0, 1.0};
    std::vector<Reservoir> history_, phase1_, final_;
    std::vector<HitInfo> hits_;
    std::vector<vec3> framebuffer_, reference_;
    std::uint64_t directShadow_ = 0, recursiveShadow_ = 0, referenceShadow_ = 0, frameHits_ = 0;
};

} // namespace restir
