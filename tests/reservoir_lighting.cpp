#include "legacy.hpp"
#include "restir.h"
#include "check.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace {

bool approx(double a, double b, double tol) {
    return std::abs(a - b) <= tol * std::max(1.0, std::abs(b));
}

// ---- independent re-implementations used to double-check the renderer ----

double test_h(const restir::Vec3& C) { // target density, phase-1 form
    return 0.2126 * C.x + 0.7152 * C.y + 0.0722 * C.z + 1e-6;
}

restir::Vec3 test_reflect(const restir::Vec3& I, const restir::Vec3& N) {
    return I - N * (2.0 * I.dot(N));
}

restir::Vec3 test_shade(const restir::Material& m, const restir::Vec3& p, const restir::Vec3& n,
                        const restir::Vec3& d, const restir::PointLight& l) {
    restir::Vec3 ld = (l.position - p).normalized();
    double diff = std::max(0.0, ld.dot(n));
    double spec = std::pow(std::max(0.0, (-test_reflect(-ld, n)).dot(d)), m.specular_exponent);
    restir::Vec3 base = m.diffuse_color * (m.albedo[0] * diff) +
                        restir::Vec3{1, 1, 1} * (m.albedo[1] * spec);
    return base.mul(l.intensity);
}

restir::Vec3 cam_ray(int x, int y, int w, int h, double fov) {
    return restir::Vec3{(x + 0.5) - w / 2.0, -(y + 0.5) + h / 2.0, -h / (2.0 * std::tan(fov / 2.0))}
        .normalized();
}

bool test_intersect(const restir::SceneView& sc, const restir::Vec3& o, const restir::Vec3& d,
                    restir::Vec3& p, restir::Vec3& n, restir::Material& m) {
    double nearest = 1e10;
    bool found = false;
    if (std::abs(d.y) > .001) {
        double t = -(o.y + 4) / d.y;
        restir::Vec3 q = o + d * t;
        if (t > .001 && t < nearest && std::abs(q.x) < 10 && q.z < -10 && q.z > -30) {
            nearest = t;
            found = true;
            p = q;
            n = {0, 1, 0};
            m = restir::Material{};
            m.diffuse_color = (int(.5 * q.x + 1000) + int(.5 * q.z)) & 1
                                  ? restir::Vec3{.3, .3, .3}
                                  : restir::Vec3{.3, .2, .1};
        }
    }
    for (const restir::Sphere& s : sc.spheres) {
        restir::Vec3 L = s.center - o;
        double tca = L.dot(d);
        double d2 = L.dot(L) - tca * tca;
        if (d2 > s.radius * s.radius) continue;
        double thc = std::sqrt(s.radius * s.radius - d2);
        double t = tca - thc > .001 ? tca - thc : (tca + thc > .001 ? tca + thc : -1);
        if (t < 0 || t > nearest) continue;
        nearest = t;
        found = true;
        p = o + d * t;
        n = (p - s.center).normalized();
        m = s.material;
    }
    return found;
}

restir::SceneView legacy_scene_view() {
    restir::SceneView sc;
    for (const Sphere& s : spheres) {
        restir::Material m;
        m.refractive_index = s.material.refractive_index;
        for (int i = 0; i < 4; ++i) m.albedo[i] = s.material.albedo[i];
        m.diffuse_color = {s.material.diffuse_color.x, s.material.diffuse_color.y,
                           s.material.diffuse_color.z};
        m.specular_exponent = s.material.specular_exponent;
        sc.spheres.push_back({{s.center.x, s.center.y, s.center.z}, s.radius, m});
    }
    for (const vec3& l : lights) sc.lights.push_back({{l.x, l.y, l.z}, {1, 1, 1}});
    return sc;
}

bool same_frame(const std::vector<restir::Vec3>& a, const std::vector<restir::Vec3>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].x != b[i].x || a[i].y != b[i].y || a[i].z != b[i].z) return false;
    return true;
}

template <typename F> bool throws_invalid(F&& f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    } catch (...) {
    }
    return false;
}

restir::Material diffuse_only(double r, double g, double b, double a0 = 0.9) {
    restir::Material m;
    m.albedo[0] = a0;
    m.albedo[1] = m.albedo[2] = m.albedo[3] = 0;
    m.diffuse_color = {r, g, b};
    m.specular_exponent = 0;
    return m;
}

} // namespace

CASE(reservoir) {
    // One diffuse sphere straight ahead of a 1x1 camera: the first hit is fully
    // controlled, so an independent weighted discrete enumeration predicts the
    // reservoir behavior exactly.
    restir::SceneView sc;
    restir::Material mat = diffuse_only(0.4, 0.4, 0.3);
    mat.specular_exponent = 50;
    sc.spheres.push_back({{0, 0, -10}, 3.0, mat});
    sc.lights.push_back({{-5, 8, 2}, {2.0, 0.2, 0.2}});
    sc.lights.push_back({{6, 4, 0}, {0.3, 1.5, 0.3}});
    sc.lights.push_back({{0, -6, 4}, {0.5, 0.5, 1.0}});
    const restir::Camera cam{{0, 0, 0}, 1.0};
    const int K = 8, HL = 5, L = 3;

    // Independent expectation: the 1x1 ray is (0,0,-1), the hit is (0,0,-7).
    const restir::Vec3 p{0, 0, -7}, n{0, 0, 1}, d{0, 0, -1};
    double h[3], hsum = 0;
    for (int l = 0; l < L; ++l) {
        h[l] = test_h(test_shade(mat, p, n, d, sc.lights[l]));
        hsum += h[l];
    }
    CHECK(h[0] != h[1] && h[1] != h[2] && h[0] != h[2]);

    { // single frozen 1x1 pool: the final pool is the phase-1 pool reweighted by itself
        restir::ReservoirRenderer r(sc, cam, 1, 1, 123, K, HL);
        r.render_frame();
        restir::Reservoir s = r.reservoir(0, 0);
        CHECK(s.hit);
        CHECK(s.selected >= 0 && s.selected < L);
        CHECK(approx(s.M, K, 1e-12));
        CHECK(approx(s.h_selected, h[s.selected], 1e-12));
        CHECK(approx(s.W * s.M * s.h_selected, s.S, 1e-12)); // W == S/(M*h)
        CHECK(r.last_frame_stats().valid_pixels == 1);
        CHECK(r.last_frame_stats().direct_shadow_rays == 1); // one finite shadow ray
    }

    // Selection frequency and normalization vs. independent enumeration.
    const int R = 4000;
    double count[3] = {0, 0, 0}, sm_sum = 0;
    for (int s = 0; s < R; ++s) {
        restir::ReservoirRenderer r(sc, cam, 1, 1, std::uint64_t(9000 + s), K, HL);
        r.render_frame();
        restir::Reservoir snap = r.reservoir(0, 0);
        CHECK(snap.selected >= 0 && snap.selected < L);
        count[snap.selected] += 1;
        sm_sum += snap.S / snap.M;
    }
    for (int l = 0; l < L; ++l) {
        const double expected = h[l] / hsum; // reservoir sampling picks proportionally to h
        const double tol = 5 * std::sqrt(expected * (1 - expected) / R);
        CHECK(std::abs(count[l] / R - expected) < tol);
    }
    double ew = 0, ew2 = 0; // candidate weights are h/q with q = 1/L
    for (int l = 0; l < L; ++l) {
        ew += h[l];
        ew2 += h[l] * L * h[l] * L / L;
    }
    const double se = std::sqrt(ew2 - ew * ew) / std::sqrt(double(K) * R);
    CHECK(std::abs(sm_sum / R - hsum) < 5 * se); // E[S/M] == sum of target densities

    // Invalid or non-finite settings are rejected.
    CHECK(throws_invalid([&] { restir::ReservoirRenderer r(sc, cam, 0, 1, 1, K, HL); }));
    CHECK(throws_invalid([&] { restir::ReservoirRenderer r(sc, cam, 1, -2, 1, K, HL); }));
    CHECK(throws_invalid([&] { restir::ReservoirRenderer r(sc, cam, 1, 1, 1, 0, HL); }));
    CHECK(throws_invalid([&] { restir::ReservoirRenderer r(sc, cam, 1, 1, 1, -3, HL); }));
    CHECK(throws_invalid([&] { restir::ReservoirRenderer r(sc, cam, 1, 1, 1, K, 0); }));
    CHECK(throws_invalid([&] { restir::ReservoirRenderer r(sc, cam, 1, 1, 1, K, -1); }));
    CHECK(throws_invalid([&] { restir::ReservoirRenderer r(sc, {{0, 0, 0}, 0.0}, 1, 1, 1, K, HL); }));
    CHECK(throws_invalid([&] { restir::ReservoirRenderer r(sc, {{0, 0, 0}, -1.0}, 1, 1, 1, K, HL); }));
    CHECK(throws_invalid([&] {
        restir::ReservoirRenderer r(sc, {{0, 0, 0}, std::nan("")}, 1, 1, 1, K, HL);
    }));
    CHECK(throws_invalid([&] {
        restir::ReservoirRenderer r(sc, {{std::nan(""), 0, 0}, 1.0}, 1, 1, 1, K, HL);
    }));
    CHECK(throws_invalid([&] {
        restir::SceneView bad = sc;
        bad.lights[0].intensity = {-1, 0, 0};
        restir::ReservoirRenderer r(bad, cam, 1, 1, 1, K, HL);
    }));
    CHECK(throws_invalid([&] {
        restir::SceneView bad = sc;
        bad.lights[1].intensity.y = std::nan("");
        restir::ReservoirRenderer r(bad, cam, 1, 1, 1, K, HL);
    }));
    CHECK(throws_invalid([&] {
        restir::SceneView bad = sc;
        bad.lights[2].position = {std::numeric_limits<double>::infinity(), 0, 0};
        restir::ReservoirRenderer r(bad, cam, 1, 1, 1, K, HL);
    }));
    CHECK(throws_invalid([&] {
        restir::SceneView bad = sc;
        bad.spheres[0].radius = 0;
        restir::ReservoirRenderer r(bad, cam, 1, 1, 1, K, HL);
    }));
    CHECK(throws_invalid([&] {
        restir::SceneView bad = sc;
        bad.spheres[0].material.albedo[2] = -0.5;
        restir::ReservoirRenderer r(bad, cam, 1, 1, 1, K, HL);
    }));
    {
        restir::ReservoirRenderer r(sc, cam, 1, 1, 1, K, HL);
        CHECK(throws_invalid([&] { r.set_camera({{0, 0, 0}, 7.0}); }));
        CHECK(throws_invalid([&] { r.resize(-1, 4); }));
        CHECK(throws_invalid([&] {
            restir::SceneView bad = sc;
            bad.spheres[0].material.refractive_index = 0;
            r.set_scene(bad);
        }));
    }

    // No lights: direct light is zero, no degenerate uniform distribution is built.
    {
        restir::SceneView dark = sc;
        dark.lights.clear();
        restir::ReservoirRenderer r(dark, cam, 1, 1, 5, K, HL);
        std::vector<restir::Vec3> f = r.render_frame();
        CHECK(f[0].x == 0 && f[0].y == 0 && f[0].z == 0); // pure diffuse material, no lights
        restir::Reservoir s = r.reservoir(0, 0);
        CHECK(s.hit && s.selected == -1 && s.M == 0 && s.S == 0 && s.W == 0);
        CHECK(r.last_frame_stats().valid_pixels == 1);
        CHECK(r.last_frame_stats().direct_shadow_rays == 0);
        CHECK(r.last_frame_stats().recursive_shadow_rays == 0);
        restir::Vec3 ref = r.render_reference_pixel(0, 0);
        CHECK(ref.x == 0 && ref.y == 0 && ref.z == 0);
    }

    // All-zero intensities: uniform selection, exactly zero contribution.
    {
        restir::SceneView zero = sc;
        for (restir::PointLight& l : zero.lights) l.intensity = {0, 0, 0};
        double c0 = 0;
        const int RZ = 2000;
        for (int s = 0; s < RZ; ++s) {
            restir::ReservoirRenderer r(zero, cam, 1, 1, std::uint64_t(300 + s), 16, HL);
            std::vector<restir::Vec3> f = r.render_frame();
            CHECK(f[0].x == 0 && f[0].y == 0 && f[0].z == 0);
            restir::Reservoir snap = r.reservoir(0, 0);
            CHECK(snap.selected >= 0 && snap.selected < L);
            CHECK(approx(snap.h_selected, 1e-6, 1e-9)); // only the density floor remains
            CHECK(approx(snap.M, 16, 1e-12));
            if (snap.selected == 0) c0 += 1;
        }
        CHECK(std::abs(c0 / RZ - 1.0 / L) < 5 * std::sqrt(0.25 / RZ)); // uniform
    }

    // Legacy entry points are untouched.
    {
        vec3 c = cast_ray(vec3{0, 0, 0}, vec3{-3, 0, -16}.normalized());
        CHECK(std::isfinite(c.x) && c.x > 0);
        CHECK(approx(c.x, 0.189585, 1e-4) && approx(c.y, 0.275145, 1e-4) &&
              approx(c.z, 0.292257, 1e-4));
        auto [hit, hp, hn, hm] = scene_intersect(vec3{0, 0, 0}, vec3{-3, 0, -16}.normalized());
        CHECK(hit);
    }

    // Instance isolation: same config + seed reproduces, different seed does not,
    // and advancing one instance never touches another.
    {
        restir::SceneView legacy = legacy_scene_view();
        restir::Camera lc{{0, 0, 0}, 1.05};
        restir::ReservoirRenderer a(legacy, lc, 8, 6, 42, 4, 8);
        restir::ReservoirRenderer b(legacy, lc, 8, 6, 42, 4, 8);
        restir::ReservoirRenderer c(legacy, lc, 8, 6, 43, 4, 8);
        std::vector<restir::Vec3> fa1 = a.render_frame();
        std::vector<restir::Vec3> fb1 = b.render_frame();
        std::vector<restir::Vec3> fc1 = c.render_frame();
        CHECK(same_frame(fa1, fb1));
        CHECK(!same_frame(fa1, fc1));
        a.render_frame();
        std::vector<restir::Vec3> fa3 = a.render_frame();
        CHECK(!same_frame(fa3, fb1)); // a moved on, b did not
        restir::ReservoirRenderer d(legacy, lc, 8, 6, 42, 4, 8);
        d.render_frame();
        d.render_frame();
        CHECK(same_frame(fa3, d.render_frame())); // rebuild + replay reproduces exactly
    }
}

CASE(reuse) {
    // Two spheres with different diffuse colors -> two surfaces whose target
    // densities differ for every light, so any h carried over from a foreign
    // surface is detectable.
    restir::SceneView sc;
    sc.spheres.push_back({{-2, 0, -10}, 1.5, diffuse_only(0.6, 0.1, 0.1, 1.0)});
    sc.spheres.push_back({{2, 0, -10}, 1.5, diffuse_only(0.1, 0.1, 0.6, 1.0)});
    sc.lights.push_back({{-20, 10, 20}, {1.0, 1.0, 1.0}});
    sc.lights.push_back({{20, 10, 20}, {1.2, 0.8, 0.4}});
    const double fov = 2 * std::atan(0.2);
    const restir::Camera cam{{0, 0, 0}, fov};
    const int K = 2, HL = 3;
    restir::ReservoirRenderer r(sc, cam, 2, 1, 777, K, HL);

    // Independent per-pixel surfaces and per-light densities.
    restir::Vec3 p[2], n[2];
    restir::Material m[2];
    double h[2][2];
    for (int x = 0; x < 2; ++x) {
        restir::Vec3 d = cam_ray(x, 0, 2, 1, fov);
        CHECK(test_intersect(sc, cam.origin, d, p[x], n[x], m[x]));
        for (int l = 0; l < 2; ++l) h[x][l] = test_h(test_shade(m[x], p[x], n[x], d, sc.lights[l]));
    }
    CHECK(h[0][0] != h[1][0] && h[0][1] != h[1][1]);

    auto check_snap = [&](const restir::Reservoir& s, int x, double M) {
        CHECK(s.hit);
        CHECK(s.selected == 0 || s.selected == 1);
        CHECK(approx(s.h_selected, h[x][s.selected], 1e-9)); // current surface's h, not a foreign one
        CHECK(approx(s.M, M, 1e-12));
        CHECK(approx(s.W * s.M * s.h_selected, s.S, 1e-9));
    };

    r.render_frame();
    check_snap(r.reservoir(0, 0), 0, 2.0 * K); // own + right neighbor; left is out of bounds
    check_snap(r.reservoir(1, 0), 1, 2.0 * K); // frozen pools: an in-place update would give 3K

    r.render_frame();
    check_snap(r.reservoir(0, 0), 0, 2.0 * K + HL); // + min(2K, HL): history is truncated
    check_snap(r.reservoir(1, 0), 1, 2.0 * K + HL);

    r.render_frame();
    check_snap(r.reservoir(0, 0), 0, 2.0 * K + HL); // history is the FINAL pool (min(7,3)=3, not 2)

    r.set_camera({{0.05, 0, 0}, fov}); // camera motion keeps the history
    r.render_frame();
    CHECK(r.reservoir(0, 0).hit && r.reservoir(1, 0).hit);
    CHECK(approx(r.reservoir(0, 0).M, 2.0 * K + HL, 1e-12));
    CHECK(approx(r.reservoir(1, 0).M, 2.0 * K + HL, 1e-12));

    restir::SceneView swapped = sc; // light order changed -> history cleared
    std::swap(swapped.lights[0], swapped.lights[1]);
    r.set_scene(swapped);
    r.render_frame();
    CHECK(approx(r.reservoir(0, 0).M, 2.0 * K, 1e-12));
    r.render_frame(); // history refills: M = 2K + min(2K, HL)
    CHECK(approx(r.reservoir(0, 0).M, 2.0 * K + HL, 1e-12));
    r.set_scene(swapped); // identical lights -> history kept
    r.render_frame();
    CHECK(approx(r.reservoir(0, 0).M, 2.0 * K + HL, 1e-12));

    restir::SceneView one_light = sc; // light count changed -> history cleared
    one_light.lights.pop_back();
    r.set_scene(one_light);
    r.render_frame();
    CHECK(approx(r.reservoir(0, 0).M, 2.0 * K, 1e-12));

    r.set_scene(sc); // back to two lights (a change -> cleared), then rebuild by resize
    r.resize(3, 2);  // the middle column of this frame misses every surface
    r.render_frame();
    CHECK(!r.reservoir(1, 0).hit && !r.reservoir(1, 1).hit); // background pixels
    CHECK(r.reservoir(1, 1).selected == -1 && r.reservoir(1, 1).M == 0);
    CHECK(approx(r.reservoir(0, 0).M, 2.0 * K, 1e-12)); // own + down; right neighbor missed
    CHECK(approx(r.reservoir(0, 1).M, 2.0 * K, 1e-12)); // own + up; right neighbor missed
    CHECK(approx(r.reservoir(2, 0).M, 2.0 * K, 1e-12)); // own + down; left neighbor missed
}

CASE(lighting) {
    // Small scene: diffuse main sphere, an occluder sphere, a mirror sphere
    // (exercises the legacy recursion), four differently colored lights, and
    // the checkerboard ground. The camera moves between the two frames of
    // every seed while the history is kept.
    restir::SceneView sc;
    restir::Material main_mat;
    main_mat.albedo[0] = 0.9;
    main_mat.albedo[1] = 0.3;
    main_mat.albedo[2] = main_mat.albedo[3] = 0;
    main_mat.diffuse_color = {0.5, 0.4, 0.3};
    main_mat.specular_exponent = 30;
    restir::Material occ_mat = diffuse_only(0.7, 0.7, 0.7, 1.0);
    restir::Material mir_mat;
    mir_mat.albedo[0] = 0.0;
    mir_mat.albedo[1] = 10.0;
    mir_mat.albedo[2] = 0.8;
    mir_mat.albedo[3] = 0.0;
    mir_mat.diffuse_color = {1, 1, 1};
    mir_mat.specular_exponent = 1425;
    sc.spheres.push_back({{0, -1, -12}, 2.0, main_mat});
    sc.spheres.push_back({{2.5, 1.5, -8}, 1.5, occ_mat});
    sc.spheres.push_back({{-3, 0.5, -10}, 1.0, mir_mat});
    sc.lights.push_back({{-15, 15, 10}, {3.0, 0.6, 0.6}});
    sc.lights.push_back({{10, 20, 5}, {0.5, 2.5, 0.8}});
    sc.lights.push_back({{0, 6, 25}, {0.6, 0.8, 2.5}});
    sc.lights.push_back({{4, -3, -6}, {0.8, 0.9, 1.0}});
    const int W = 12, H = 9, K = 4, HL = 16, SEEDS = 64;
    const restir::Camera cam0{{0, 0, 0}, 0.9};
    const restir::Camera cam1{{0.4, 0.2, 0.3}, 0.9};

    // Deterministic all-lights reference through the same geometry/material logic.
    restir::ReservoirRenderer ref(sc, cam0, W, H, 7, K, HL);
    std::vector<restir::Vec3> ref0 = ref.render_reference_frame();
    ref.set_camera(cam1);
    std::vector<restir::Vec3> ref1 = ref.render_reference_frame();

    double ris[3] = {0, 0, 0}, want[3] = {0, 0, 0};
    std::uint64_t direct_total = 0, rec_total = 0, valid_total = 0, rec_per_frame = 0;
    for (int s = 0; s < SEEDS; ++s) {
        restir::ReservoirRenderer r(sc, cam0, W, H, std::uint64_t(5000 + s), K, HL);
        std::vector<restir::Vec3> f0 = r.render_frame();
        restir::FrameStats st0 = r.last_frame_stats();
        r.set_camera(cam1);
        std::vector<restir::Vec3> f1 = r.render_frame();
        restir::FrameStats st1 = r.last_frame_stats();
        CHECK(st0.valid_pixels > 0 && st1.valid_pixels > 0);
        CHECK(st0.direct_shadow_rays == st0.valid_pixels); // exactly one per valid first hit
        CHECK(st1.direct_shadow_rays == st1.valid_pixels);
        CHECK(st0.direct_shadow_rays <= std::uint64_t(W * H));
        CHECK(st0.recursive_shadow_rays > 0); // the mirror sphere recurses
        if (s == 0) rec_per_frame = st0.recursive_shadow_rays;
        CHECK(st0.recursive_shadow_rays == rec_per_frame); // recursion is deterministic
        direct_total += st0.direct_shadow_rays + st1.direct_shadow_rays;
        rec_total += st0.recursive_shadow_rays + st1.recursive_shadow_rays;
        valid_total += st0.valid_pixels + st1.valid_pixels;
        for (int i = 0; i < W * H; ++i)
            for (int c = 0; c < 3; ++c) ris[c] += f0[i][c] + f1[i][c];
    }
    for (int i = 0; i < W * H; ++i)
        for (int c = 0; c < 3; ++c) want[c] += ref0[i][c] + ref1[i][c];

    const double n = double(SEEDS) * 2 * W * H;
    double err[3];
    for (int c = 0; c < 3; ++c) {
        const double mean_ris = ris[c] / n;
        const double mean_ref = want[c] / (2.0 * W * H);
        err[c] = std::abs(mean_ris - mean_ref) / std::max(1e-9, mean_ref);
        CHECK(err[c] < 0.08); // RIS mean matches the independent all-lights result
    }
    std::cout << "RIS_MEAN " << ris[0] / n << " " << ris[1] / n << " " << ris[2] / n << "\n";
    std::cout << "REF_MEAN " << want[0] / (2.0 * W * H) << " " << want[1] / (2.0 * W * H) << " "
              << want[2] / (2.0 * W * H) << "\n";
    std::cout << "REL_ERR " << err[0] << " " << err[1] << " " << err[2] << "\n";
    std::cout << "SHADOW_RAYS direct=" << direct_total << " recursive=" << rec_total
              << " valid_pixels=" << valid_total << "\n";

    // The reference entry reproduces the original cast_ray results on the
    // original scene (legacy lights == intensity (1,1,1)).
    restir::ReservoirRenderer leg(legacy_scene_view(), restir::Camera{{0, 0, 0}, 1.05}, 32, 24,
                                  3, 1, 1);
    for (int y = 0; y < 24; ++y)
    for (int x = 0; x < 32; ++x) {
        float dx = (x + 0.5f) - 32 / 2.f;
        float dy = -(y + 0.5f) + 24 / 2.f;
        float dz = -24 / (2.f * std::tan(1.05f / 2.f));
        vec3 c0 = cast_ray(vec3{0, 0, 0}, vec3{dx, dy, dz}.normalized());
        restir::Vec3 c1 = leg.render_reference_pixel(x, y);
        CHECK(approx(c1.x, c0.x, 2e-3) && approx(c1.y, c0.y, 2e-3) && approx(c1.z, c0.z, 2e-3));
    }
}
