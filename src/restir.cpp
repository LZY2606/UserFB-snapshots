#include "restir.h"

#include <algorithm>
#include <random>
#include <stdexcept>
#include <utility>

namespace restir {
namespace {

constexpr Vec3 kBackground{0.2, 0.7, 0.8};
constexpr double kPi = 3.14159265358979323846;

bool finite3(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Vec3 reflect(const Vec3& I, const Vec3& N) {
    return I - N * (2.0 * I.dot(N));
}

Vec3 refract(const Vec3& I, const Vec3& N, const double eta_t, const double eta_i = 1.0) {
    double cosi = -std::max(-1.0, std::min(1.0, I.dot(N)));
    if (cosi < 0) return refract(I, -N, eta_i, eta_t);
    double eta = eta_i / eta_t;
    double k = 1 - eta * eta * (1 - cosi * cosi);
    return k < 0 ? Vec3{1, 0, 0} : I * eta + N * (eta * cosi - std::sqrt(k));
}

bool ray_sphere_intersect(const Vec3& orig, const Vec3& dir, const Sphere& s, double& t) {
    Vec3 L = s.center - orig;
    double tca = L.dot(dir);
    double d2 = L.dot(L) - tca * tca;
    if (d2 > s.radius * s.radius) return false;
    double thc = std::sqrt(s.radius * s.radius - d2);
    double t0 = tca - thc, t1 = tca + thc;
    if (t0 > .001) { t = t0; return true; } // offset to avoid self-occlusion
    if (t1 > .001) { t = t1; return true; }
    return false;
}

struct Hit {
    Vec3 point;
    Vec3 normal;
    Material material;
};

// Same geometry as the legacy scene_intersect: checkerboard ground + spheres.
bool scene_intersect(const SceneView& scene, const Vec3& orig, const Vec3& dir, Hit& out) {
    double nearest = 1e10;
    bool found = false;
    if (std::abs(dir.y) > .001) { // checkerboard plane y = -4
        double d = -(orig.y + 4) / dir.y;
        Vec3 p = orig + dir * d;
        if (d > .001 && d < nearest && std::abs(p.x) < 10 && p.z < -10 && p.z > -30) {
            nearest = d;
            found = true;
            out.point = p;
            out.normal = {0, 1, 0};
            out.material = Material{};
            out.material.diffuse_color =
                (int(.5 * p.x + 1000) + int(.5 * p.z)) & 1 ? Vec3{.3, .3, .3} : Vec3{.3, .2, .1};
        }
    }
    for (const Sphere& s : scene.spheres) {
        double d;
        if (!ray_sphere_intersect(orig, dir, s, d) || d > nearest) continue;
        nearest = d;
        found = true;
        out.point = orig + dir * d;
        out.normal = (out.point - s.center).normalized();
        out.material = s.material;
    }
    return found;
}

// Single-light RGB at a surface: legacy diffuse + Phong, per-channel times the
// light intensity, no distance falloff, visibility NOT included.
Vec3 shade_light(const Material& m, const Vec3& point, const Vec3& N, const Vec3& dir,
                 const PointLight& light) {
    Vec3 light_dir = (light.position - point).normalized();
    double diffuse = std::max(0.0, light_dir.dot(N));
    double specular =
        std::pow(std::max(0.0, (-reflect(-light_dir, N)).dot(dir)), m.specular_exponent);
    Vec3 base = m.diffuse_color * (m.albedo[0] * diffuse) + Vec3{1, 1, 1} * (m.albedo[1] * specular);
    return base.mul(light.intensity);
}

// Phase-1 target density: luminance of the unoccluded contribution plus a small
// positive floor that keeps the support shared across surfaces. The floor is
// part of h only; it is never added to the output color.
double target_density(const Vec3& C) {
    return 0.2126 * C.x + 0.7152 * C.y + 0.0722 * C.z + 1e-6;
}

// Real finite shadow ray towards a point light.
bool visible(const SceneView& scene, const Vec3& point, const PointLight& light) {
    Vec3 to_light = light.position - point;
    double dist = to_light.norm();
    if (dist <= 0) return true;
    Hit h;
    return !(scene_intersect(scene, point, to_light * (1.0 / dist), h) &&
             (h.point - point).norm() < dist);
}

// Legacy recursive path, unchanged in behavior: every hit loops over ALL lights
// with one finite shadow query each. Shadow queries are counted when a counter
// is provided (the reference entry passes nullptr).
Vec3 cast_ray_legacy(const SceneView& scene, const Vec3& orig, const Vec3& dir, int depth,
                     std::uint64_t* shadow_counter) {
    Hit h;
    if (depth > 4 || !scene_intersect(scene, orig, dir, h)) return kBackground;
    Vec3 reflect_dir = reflect(dir, h.normal).normalized();
    Vec3 refract_dir = refract(dir, h.normal, h.material.refractive_index).normalized();
    Vec3 reflect_color = cast_ray_legacy(scene, h.point, reflect_dir, depth + 1, shadow_counter);
    Vec3 refract_color = cast_ray_legacy(scene, h.point, refract_dir, depth + 1, shadow_counter);
    Vec3 direct{0, 0, 0};
    for (const PointLight& light : scene.lights) {
        if (shadow_counter) ++*shadow_counter;
        if (visible(scene, h.point, light))
            direct = direct + shade_light(h.material, h.point, h.normal, dir, light);
    }
    return direct + reflect_color * h.material.albedo[2] + refract_color * h.material.albedo[3];
}

void validate_material(const Material& m) {
    if (!std::isfinite(m.refractive_index) || m.refractive_index <= 0)
        throw std::invalid_argument("material.refractive_index must be positive and finite");
    for (double a : m.albedo)
        if (!std::isfinite(a) || a < 0)
            throw std::invalid_argument("material.albedo must be non-negative and finite");
    if (!finite3(m.diffuse_color))
        throw std::invalid_argument("material.diffuse_color must be finite");
    if (!std::isfinite(m.specular_exponent) || m.specular_exponent < 0)
        throw std::invalid_argument("material.specular_exponent must be non-negative and finite");
}

void validate_scene(const SceneView& scene) {
    for (const Sphere& s : scene.spheres) {
        if (!finite3(s.center) || !std::isfinite(s.radius) || s.radius <= 0)
            throw std::invalid_argument("sphere center/radius must be finite, radius positive");
        validate_material(s.material);
    }
    for (const PointLight& l : scene.lights) {
        if (!finite3(l.position))
            throw std::invalid_argument("light position must be finite");
        if (!finite3(l.intensity) || l.intensity.x < 0 || l.intensity.y < 0 || l.intensity.z < 0)
            throw std::invalid_argument("light intensity must be non-negative and finite");
    }
}

void validate_camera(const Camera& c) {
    if (!finite3(c.origin)) throw std::invalid_argument("camera origin must be finite");
    if (!std::isfinite(c.fov) || c.fov <= 0 || c.fov >= kPi)
        throw std::invalid_argument("camera fov must be finite and inside (0, pi)");
}

bool same_lights(const std::vector<PointLight>& a, const std::vector<PointLight>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const PointLight& x = a[i];
        const PointLight& y = b[i];
        if (x.position.x != y.position.x || x.position.y != y.position.y ||
            x.position.z != y.position.z || x.intensity.x != y.intensity.x ||
            x.intensity.y != y.intensity.y || x.intensity.z != y.intensity.z)
            return false;
    }
    return true;
}

struct ReservoirState {
    bool valid = false; // belongs to a surface (first hit existed)
    int selected = -1;
    double S = 0, M = 0, W = 0, h_selected = 0;
};

struct PixelGeometry {
    bool hit = false;
    Vec3 point, normal, dir;
    Material material;
    Vec3 reflect_color, refract_color;
};

} // namespace

struct ReservoirRenderer::Impl {
    SceneView scene;
    Camera camera;
    int width, height, K, history_limit;
    std::mt19937_64 rng;
    std::vector<ReservoirState> phase1; // frozen phase-1 pools of the current frame
    std::vector<ReservoirState> final;  // phase-2 output pools of the current frame
    std::vector<ReservoirState> history; // previous frame's final pools
    std::vector<PixelGeometry> geometry; // transient, rebuilt every frame
    std::vector<Vec3> framebuffer;
    FrameStats stats{};

    Impl(SceneView s, Camera c, int w, int h, std::uint64_t seed, int k, int hl)
        : scene(std::move(s)), camera(c), width(w), height(h), K(k), history_limit(hl), rng(seed) {
        allocate();
    }

    void allocate() {
        size_t n = size_t(width) * size_t(height);
        phase1.assign(n, ReservoirState{});
        final.assign(n, ReservoirState{});
        history.assign(n, ReservoirState{});
        framebuffer.assign(n, kBackground);
    }

    void clear_history() { std::fill(history.begin(), history.end(), ReservoirState{}); }

    Vec3 camera_ray(int x, int y) const {
        double dir_x = (x + 0.5) - width / 2.0;
        double dir_y = -(y + 0.5) + height / 2.0; // flips the image at the same time
        double dir_z = -height / (2.0 * std::tan(camera.fov / 2.0));
        return Vec3{dir_x, dir_y, dir_z}.normalized();
    }

    double uniform01() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng); }

    const std::vector<Vec3>& render_frame() {
        const int n = width * height;
        stats = FrameStats{};
        geometry.assign(n, PixelGeometry{});
        std::fill(phase1.begin(), phase1.end(), ReservoirState{});
        std::fill(final.begin(), final.end(), ReservoirState{});

        // Pass A: first hit, legacy recursion, and the frozen phase-1 reservoirs.
        for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int i = y * width + x;
            const Vec3 dir = camera_ray(x, y);
            Hit h;
            if (!scene_intersect(scene, camera.origin, dir, h)) {
                framebuffer[i] = kBackground;
                continue;
            }
            ++stats.valid_pixels;
            PixelGeometry& g = geometry[i];
            g.hit = true;
            g.point = h.point;
            g.normal = h.normal;
            g.dir = dir;
            g.material = h.material;
            const Vec3 reflect_dir = reflect(dir, h.normal).normalized();
            const Vec3 refract_dir = refract(dir, h.normal, h.material.refractive_index).normalized();
            g.reflect_color =
                cast_ray_legacy(scene, h.point, reflect_dir, 1, &stats.recursive_shadow_rays);
            g.refract_color =
                cast_ray_legacy(scene, h.point, refract_dir, 1, &stats.recursive_shadow_rays);

            ReservoirState r;
            r.valid = true;
            const int L = (int)scene.lights.size();
            if (L > 0) { // no lights -> direct light is zero, no uniform distribution is built
                std::uniform_int_distribution<int> pick(0, L - 1);
                const double q = 1.0 / L;
                for (int k = 0; k < K; ++k) { // uniform, with replacement
                    const int l = pick(rng);
                    const Vec3 C = shade_light(g.material, g.point, g.normal, dir, scene.lights[l]);
                    const double w = target_density(C) / q;
                    r.S += w;
                    r.M += 1; // zero-contribution candidates count as well
                    if (uniform01() < w / r.S) r.selected = l;
                }
                r.h_selected =
                    target_density(shade_light(g.material, g.point, g.normal, dir, scene.lights[r.selected]));
                r.W = r.S / (r.M * r.h_selected);
            }
            phase1[i] = r;
        }

        // Pass B: merge this frame's pool, the previous frame's final pool at the
        // same pixel, and the 4-neighbors' FROZEN phase-1 pools. Everything is
        // re-evaluated against the current surface; nothing is copied over.
        for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int i = y * width + x;
            const PixelGeometry& g = geometry[i];
            if (!g.hit) continue;
            ReservoirState merged;
            auto contribute = [&](const ReservoirState& src, double m) {
                if (!src.valid || src.selected < 0 || m <= 0) return; // out of bounds / no hit
                const Vec3 C =
                    shade_light(g.material, g.point, g.normal, g.dir, scene.lights[src.selected]);
                const double w = target_density(C) * src.W * m; // super-candidate
                merged.S += w;
                merged.M += m;
                if (uniform01() < w / merged.S) merged.selected = src.selected;
            };
            contribute(phase1[i], phase1[i].M); // this frame's own pool
            contribute(history[i], std::min(history[i].M, double(history_limit))); // truncated, W kept
            if (x > 0)          contribute(phase1[i - 1],     phase1[i - 1].M);
            if (x + 1 < width)  contribute(phase1[i + 1],     phase1[i + 1].M);
            if (y > 0)          contribute(phase1[i - width], phase1[i - width].M);
            if (y + 1 < height) contribute(phase1[i + width], phase1[i + width].M);

            Vec3 direct{0, 0, 0};
            ReservoirState out;
            out.valid = true;
            if (merged.selected >= 0 && merged.M > 0) {
                // Recompute C and h at the current surface, then one real shadow ray.
                const Vec3 C =
                    shade_light(g.material, g.point, g.normal, g.dir, scene.lights[merged.selected]);
                out.selected = merged.selected;
                out.S = merged.S;
                out.M = merged.M;
                out.h_selected = target_density(C);
                out.W = merged.S / (merged.M * out.h_selected);
                ++stats.direct_shadow_rays;
                if (visible(scene, g.point, scene.lights[merged.selected]))
                    direct = C * out.W; // C * visibility * S / (M * h)
            }
            final[i] = out;
            framebuffer[i] = direct + g.reflect_color * g.material.albedo[2] +
                             g.refract_color * g.material.albedo[3];
        }
        history = final;
        return framebuffer;
    }

    Vec3 reference_pixel(int x, int y) const {
        const Vec3 dir = camera_ray(x, y);
        Hit h;
        if (!scene_intersect(scene, camera.origin, dir, h)) return kBackground;
        const Vec3 reflect_dir = reflect(dir, h.normal).normalized();
        const Vec3 refract_dir = refract(dir, h.normal, h.material.refractive_index).normalized();
        Vec3 reflect_color = cast_ray_legacy(scene, h.point, reflect_dir, 1, nullptr);
        Vec3 refract_color = cast_ray_legacy(scene, h.point, refract_dir, 1, nullptr);
        Vec3 direct{0, 0, 0};
        for (const PointLight& light : scene.lights)
            if (visible(scene, h.point, light))
                direct = direct + shade_light(h.material, h.point, h.normal, dir, light);
        return direct + reflect_color * h.material.albedo[2] +
               refract_color * h.material.albedo[3];
    }
};

ReservoirRenderer::ReservoirRenderer(SceneView scene, Camera camera, int width, int height,
                                     std::uint64_t seed, int candidates_per_pixel,
                                     int history_limit) {
    validate_scene(scene);
    validate_camera(camera);
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("frame dimensions must be positive");
    if (candidates_per_pixel <= 0)
        throw std::invalid_argument("candidates_per_pixel must be positive");
    if (history_limit <= 0)
        throw std::invalid_argument("history_limit must be positive");
    impl_ = std::make_unique<Impl>(std::move(scene), camera, width, height, seed,
                                   candidates_per_pixel, history_limit);
}

ReservoirRenderer::~ReservoirRenderer() = default;
ReservoirRenderer::ReservoirRenderer(ReservoirRenderer&&) noexcept = default;
ReservoirRenderer& ReservoirRenderer::operator=(ReservoirRenderer&&) noexcept = default;

const SceneView& ReservoirRenderer::scene() const { return impl_->scene; }
Camera ReservoirRenderer::camera() const { return impl_->camera; }
int ReservoirRenderer::width() const { return impl_->width; }
int ReservoirRenderer::height() const { return impl_->height; }
int ReservoirRenderer::candidates_per_pixel() const { return impl_->K; }
int ReservoirRenderer::history_limit() const { return impl_->history_limit; }

void ReservoirRenderer::set_camera(const Camera& camera) {
    validate_camera(camera);
    impl_->camera = camera; // history is kept
}

void ReservoirRenderer::set_scene(SceneView scene) {
    validate_scene(scene);
    if (!same_lights(impl_->scene.lights, scene.lights))
        impl_->clear_history(); // light count, order or definition changed
    impl_->scene = std::move(scene);
}

void ReservoirRenderer::resize(int width, int height) {
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("frame dimensions must be positive");
    if (width == impl_->width && height == impl_->height) return;
    impl_->width = width;
    impl_->height = height;
    impl_->allocate(); // rebuilds the history
}

const std::vector<Vec3>& ReservoirRenderer::render_frame() { return impl_->render_frame(); }

Vec3 ReservoirRenderer::render_reference_pixel(int x, int y) const {
    if (x < 0 || y < 0 || x >= impl_->width || y >= impl_->height)
        throw std::out_of_range("pixel out of range");
    return impl_->reference_pixel(x, y);
}

std::vector<Vec3> ReservoirRenderer::render_reference_frame() const {
    std::vector<Vec3> frame(size_t(impl_->width) * size_t(impl_->height));
    for (int y = 0; y < impl_->height; ++y)
        for (int x = 0; x < impl_->width; ++x)
            frame[size_t(y) * size_t(impl_->width) + size_t(x)] = impl_->reference_pixel(x, y);
    return frame;
}

Reservoir ReservoirRenderer::reservoir(int x, int y) const {
    if (x < 0 || y < 0 || x >= impl_->width || y >= impl_->height)
        throw std::out_of_range("pixel out of range");
    const ReservoirState& r = impl_->final[size_t(y) * size_t(impl_->width) + size_t(x)];
    Reservoir snap;
    snap.hit = r.valid;
    snap.selected = r.selected;
    snap.S = r.S;
    snap.M = r.M;
    snap.W = r.W;
    snap.h_selected = r.h_selected;
    return snap;
}

FrameStats ReservoirRenderer::last_frame_stats() const { return impl_->stats; }

} // namespace restir
