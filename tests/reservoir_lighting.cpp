#include "legacy.hpp"
#include "restir.h"
#include "check.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace {

// Same pixel-center ray convention as the renderer.
vec3 camera_dir(int x, int y, int w, int h, float fov) {
    float dir_x =  (x + 0.5) -  w / 2.;
    float dir_y = -(y + 0.5) +  h / 2.;
    float dir_z = -h / (2. * std::tan(double(fov) / 2.));
    return vec3{dir_x, dir_y, dir_z}.normalized();
}

struct Surf {
    bool hit = false;
    vec3 point, N, dir;
    Material mat;
};

// Independent first-hit evaluation against an explicit sphere list, mirroring
// the legacy scene equations (checkerboard included).
Surf first_surface(const restir::SceneView& sc, const restir::Camera& cam, int x, int y, int w, int h) {
    Surf s;
    s.dir = camera_dir(x, y, w, h, cam.fov);
    float best = 1e10f;
    const Sphere* hit_sphere = nullptr;
    for (const Sphere& sp : sc.spheres) {
        auto [ok, d] = ray_sphere_intersect(cam.position, s.dir, sp);
        if (ok && d < best) { best = d; hit_sphere = &sp; }
    }
    if (std::abs(s.dir.y) > .001f) {
        float d = -(cam.position.y + 4) / s.dir.y;
        vec3 p = cam.position + s.dir * d;
        if (d > .001f && d < best && std::abs(p.x) < 10 && p.z < -10 && p.z > -30) {
            s.hit = true;
            s.point = p;
            s.N = {0, 1, 0};
            s.mat = Material{};
            s.mat.diffuse_color = (int(.5*p.x + 1000) + int(.5*p.z)) & 1 ? vec3{.3, .3, .3} : vec3{.3, .2, .1};
            return s;
        }
    }
    if (hit_sphere) {
        s.hit = true;
        s.point = cam.position + s.dir * best;
        s.N = (s.point - hit_sphere->center).normalized();
        s.mat = hit_sphere->material;
    }
    return s;
}

// Independent unoccluded single-light RGB (legacy diffuse + Phong, per-channel intensity).
vec3 indie_C(const Surf& s, const restir::PointLight& l) {
    vec3 ld = (l.position - s.point).normalized();
    float diff = std::max(0.f, ld * s.N);
    float spec = std::pow(std::max(0.f, -reflect(-ld, s.N) * s.dir), s.mat.specular_exponent);
    vec3 c = s.mat.diffuse_color * diff * s.mat.albedo[0] + vec3{1, 1, 1} * spec * s.mat.albedo[1];
    return {c.x * l.intensity.x, c.y * l.intensity.y, c.z * l.intensity.z};
}

double indie_h(const Surf& s, const restir::PointLight& l) {
    vec3 c = indie_C(s, l);
    return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z + 1e-6;
}

bool approx(double a, double b, double rel = 1e-9, double abs = 1e-12) {
    return std::fabs(a - b) <= abs + rel * std::max(std::fabs(a), std::fabs(b));
}

bool vec_approx(const vec3& a, const vec3& b, double rel, double abs) {
    return approx(a.x, b.x, rel, abs) && approx(a.y, b.y, rel, abs) && approx(a.z, b.z, rel, abs);
}

bool vec_exact(const vec3& a, const vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

template <class F> bool throws_bad(F&& f) {
    try { f(); } catch (const std::exception&) { return true; }
    return false;
}

void addto(vec3& a, const vec3& b) { a.x += b.x; a.y += b.y; a.z += b.z; }
void addsq(vec3& a, const vec3& b) { a.x += b.x*b.x; a.y += b.y*b.y; a.z += b.z*b.z; }

} // namespace

CASE(reservoir) {
    // Invalid or non-finite settings are rejected.
    restir::SceneView empty;
    CHECK(throws_bad([&]{ restir::ReservoirRenderer r(empty, restir::Camera{}, 4, 4, 0, 1, 1); }));   // K = 0
    CHECK(throws_bad([&]{ restir::ReservoirRenderer r(empty, restir::Camera{}, 4, 4, -2, 1, 1); }));  // K < 0
    CHECK(throws_bad([&]{ restir::ReservoirRenderer r(empty, restir::Camera{}, 4, 4, 1, 0, 1); }));   // historyLimit = 0
    CHECK(throws_bad([&]{ restir::ReservoirRenderer r(empty, restir::Camera{}, 0, 4, 1, 1, 1); }));   // width = 0
    CHECK(throws_bad([&]{ restir::ReservoirRenderer r(empty, restir::Camera{}, 4, -1, 1, 1, 1); }));  // height < 0
    CHECK(throws_bad([&]{ restir::Camera c; c.fov = std::nanf("");
                          restir::ReservoirRenderer r(empty, c, 4, 4, 1, 1, 1); }));
    CHECK(throws_bad([&]{ restir::SceneView s; s.lights.push_back({{0, 1, 0}, {std::nanf(""), 0, 0}});
                          restir::ReservoirRenderer r(s, restir::Camera{}, 4, 4, 1, 1, 1); }));
    CHECK(throws_bad([&]{ restir::SceneView s; s.lights.push_back({{0, 1, 0}, {-1, 0, 0}});
                          restir::ReservoirRenderer r(s, restir::Camera{}, 4, 4, 1, 1, 1); }));
    CHECK(throws_bad([&]{ restir::SceneView s; s.lights.push_back({{0, std::numeric_limits<float>::infinity(), 0}, {1, 1, 1}});
                          restir::ReservoirRenderer r(s, restir::Camera{}, 4, 4, 1, 1, 1); }));
    { // an empty scene with valid parameters is fine (the checkerboard is still there)
        restir::ReservoirRenderer r(empty, restir::Camera{}, 2, 2, 1, 1, 1);
        r.render_frame();
        CHECK(r.framebuffer().size() == 4);
        CHECK(r.direct_shadow_rays() == 0); // no lights, no visibility queries
    }

    // 1x1 render: with no neighbours and no history the final reservoir is the
    // pure stage-1 reservoir, checked against an independent weighted discrete
    // enumeration over the lights.
    Material matte{1.0f, {0.9f, 0.5f, 0.0f, 0.0f}, {0.4f, 0.4f, 0.3f}, 50.f};
    restir::SceneView sc;
    sc.spheres.push_back(Sphere{{0, 0, -10}, 3, matte});
    sc.lights = {
        {{ 0, 10, 0}, {1.0f, 1.0f, 1.0f}},
        {{ 8,  2, 5}, {2.0f, 0.5f, 0.5f}},
        {{-6,  1, 4}, {0.5f, 0.5f, 2.0f}},
    };
    const int K = 4;
    const Surf surf = first_surface(sc, restir::Camera{}, 0, 0, 1, 1);
    CHECK(surf.hit);
    std::array<vec3, 3> C; std::array<double, 3> h;
    double hsum = 0; vec3 Csum{0, 0, 0};
    for (int j = 0; j < 3; ++j) {
        C[j] = indie_C(surf, sc.lights[size_t(j)]);
        h[j] = indie_h(surf, sc.lights[size_t(j)]);
        hsum += h[j];
        addto(Csum, C[j]);
    }

    const int R = 3000;
    std::array<int, 3> counts{0, 0, 0};
    vec3 acc{0, 0, 0}, acc2{0, 0, 0};
    for (int s = 0; s < R; ++s) {
        restir::ReservoirRenderer r(sc, restir::Camera{}, 1, 1, K, 8, std::uint64_t(s));
        const std::vector<vec3>& fb = r.render_frame();
        const restir::Reservoir snap = r.reservoir(0, 0);
        CHECK(snap.M == K); // own stage-1 reservoir only: no neighbours, first frame
        CHECK(snap.selected >= 0 && snap.selected < 3);
        CHECK(approx(snap.h, h[size_t(snap.selected)]));
        CHECK(approx(snap.W, snap.S / (snap.M * snap.h)));
        CHECK(r.direct_shadow_rays() == 1); // one visibility query for the one first hit
        CHECK(r.last_frame_hits() == 1);
        counts[size_t(snap.selected)]++;
        addto(acc, fb[0]);   // matte has no reflection/refraction: fb[0] is the direct term
        addsq(acc2, fb[0]);
    }
    for (int j = 0; j < 3; ++j) { // selection frequency matches the weighted enumeration
        const double p = h[j] / hsum;
        const double freq = counts[size_t(j)] / double(R);
        CHECK(std::fabs(freq - p) < 5.0 * std::sqrt(p * (1 - p) / R) + 0.004);
    }
    for (int ch = 0; ch < 3; ++ch) { // normalized estimator: mean contribution sums all lights
        const double mean = acc[ch] / R;
        const double var = std::max(0.0, acc2[ch] / R - mean * mean);
        const double se = std::sqrt(var / R);
        CHECK(std::fabs(mean - Csum[ch]) < 6.0 * se + 1e-6);
    }
    { // the reference entry sums the same lights through the same material logic
        restir::ReservoirRenderer rr(sc, restir::Camera{}, 1, 1, 1, 1, 123);
        const std::vector<vec3>& ref = rr.render_reference();
        CHECK(vec_approx(ref[0], Csum, 1e-6, 1e-6));
        CHECK(rr.reference_shadow_rays() >= 3); // 3 first-hit queries plus recursive ones
    }

    // All-zero intensities: direct light is exactly zero, reservoirs still work.
    {
        restir::SceneView z = sc;
        for (auto& l : z.lights) l.intensity = {0, 0, 0};
        restir::ReservoirRenderer r(z, restir::Camera{}, 1, 1, K, 8, 42);
        const std::vector<vec3>& fb = r.render_frame();
        const std::vector<vec3>& ref = r.render_reference();
        CHECK(vec_exact(fb[0], vec3{0, 0, 0}));
        CHECK(vec_exact(fb[0], ref[0]));
        const restir::Reservoir snap = r.reservoir(0, 0);
        CHECK(snap.selected >= 0 && snap.M == K && snap.S > 0); // density floor keeps support
    }

    // No lights at all: direct light is zero, no invalid uniform distribution.
    {
        restir::SceneView nl;
        nl.spheres = sc.spheres;
        restir::ReservoirRenderer r(nl, restir::Camera{}, 1, 1, K, 8, 7);
        const std::vector<vec3>& fb = r.render_frame();
        const std::vector<vec3>& ref = r.render_reference();
        CHECK(vec_exact(fb[0], vec3{0, 0, 0}));
        CHECK(vec_exact(fb[0], ref[0]));
        const restir::Reservoir snap = r.reservoir(0, 0);
        CHECK(snap.selected == -1 && snap.M == 0 && snap.S == 0 && snap.W == 0);
        CHECK(r.direct_shadow_rays() == 0);
        CHECK(r.last_frame_hits() == 1);
    }

    // Instance isolation: identical configs reproduce, references do not
    // disturb the stream, different seeds differ.
    {
        const restir::SceneView lv = restir::SceneView::legacy();
        restir::Camera moved; moved.position = {0.1f, 0, 0};
        restir::ReservoirRenderer a(lv, restir::Camera{}, 8, 6, 2, 4, 7);
        restir::ReservoirRenderer b(lv, restir::Camera{}, 8, 6, 2, 4, 7);
        a.render_frame(); a.set_camera(moved); a.render_frame();
        b.render_frame(); b.set_camera(moved); b.render_frame();
        bool same = true;
        for (size_t i = 0; i < a.framebuffer().size(); ++i)
            same = same && vec_exact(a.framebuffer()[i], b.framebuffer()[i]);
        CHECK(same);
        restir::ReservoirRenderer c(lv, restir::Camera{}, 8, 6, 2, 4, 7);
        c.render_frame(); c.render_reference(); c.set_camera(moved); c.render_frame();
        same = true;
        for (size_t i = 0; i < a.framebuffer().size(); ++i)
            same = same && vec_exact(a.framebuffer()[i], c.framebuffer()[i]);
        CHECK(same);
        restir::ReservoirRenderer d(lv, restir::Camera{}, 8, 6, 2, 4, 8); // other seed
        d.render_frame(); d.set_camera(moved); d.render_frame();
        bool differs = false;
        for (size_t i = 0; i < a.framebuffer().size(); ++i)
            differs = differs || !vec_exact(a.framebuffer()[i], d.framebuffer()[i]);
        CHECK(differs);
    }

    // The legacy entry point is untouched.
    {
        vec3 c0 = ::cast_ray(vec3{0, 0, 0}, vec3{-3, 0, -16}.normalized());
        CHECK(std::isfinite(c0.x) && std::isfinite(c0.y) && std::isfinite(c0.z) && c0.x > 0);
    }
}

CASE(reuse) {
    // Two surfaces with different target densities, one pixel each.
    restir::SceneView sc;
    sc.spheres.push_back(Sphere{{-3, 0, -6}, 1.5f, ivory});
    sc.spheres.push_back(Sphere{{ 3, 0, -6}, 1.5f, red_rubber});
    sc.lights = {
        {{  0, 10, 0}, {1.0f, 1.0f, 1.0f}},
        {{-10,  5, 5}, {1.0f, 0.5f, 0.2f}},
        {{ 10,  5, 5}, {0.2f, 0.5f, 1.0f}},
    };
    const int K = 3, HL = 3;
    restir::Camera cam;
    restir::ReservoirRenderer r(sc, cam, 2, 1, K, HL, 99);

    // Frame 1: own reservoir + the frozen stage-1 reservoir of the neighbour.
    r.render_frame();
    CHECK(r.last_frame_hits() == 2);
    const restir::Reservoir p1A = r.phase1_reservoir(0, 0), p1B = r.phase1_reservoir(1, 0);
    const restir::Reservoir fA1 = r.reservoir(0, 0), fB1 = r.reservoir(1, 0);
    CHECK(p1A.M == K && p1B.M == K);
    CHECK(fA1.M == 2 * K && fB1.M == 2 * K); // frozen neighbour: 2K, not 3K
    const Surf sA = first_surface(sc, cam, 0, 0, 2, 1);
    const Surf sB = first_surface(sc, cam, 1, 0, 2, 1);
    CHECK(sA.hit && sB.hit);
    { // super-candidate reweighting: S = sum over sources of h_current(sel)*W_source*m
        const double expSA = indie_h(sA, sc.lights[size_t(p1A.selected)]) * p1A.W * K
                           + indie_h(sA, sc.lights[size_t(p1B.selected)]) * p1B.W * K;
        CHECK(approx(fA1.S, expSA));
        CHECK(fA1.selected == p1A.selected || fA1.selected == p1B.selected);
        CHECK(approx(fA1.h, indie_h(sA, sc.lights[size_t(fA1.selected)])));
        CHECK(approx(fA1.W, fA1.S / (fA1.M * fA1.h)));
        const double expSB = indie_h(sB, sc.lights[size_t(p1B.selected)]) * p1B.W * K
                           + indie_h(sB, sc.lights[size_t(p1A.selected)]) * p1A.W * K;
        CHECK(approx(fB1.S, expSB));
        CHECK(approx(fB1.h, indie_h(sB, sc.lights[size_t(fB1.selected)])));
    }

    // Frame 2: the previous final reservoir joins with m = min(M, historyLimit),
    // W_source preserved after truncation.
    r.render_frame();
    const restir::Reservoir p1A2 = r.phase1_reservoir(0, 0), p1B2 = r.phase1_reservoir(1, 0);
    const restir::Reservoir fA2 = r.reservoir(0, 0);
    CHECK(fA2.M == 3 * K); // K own + K neighbour + min(2K, HL) = K history
    CHECK(r.reservoir(1, 0).M == 3 * K);
    {
        const double expSA2 = indie_h(sA, sc.lights[size_t(p1A2.selected)]) * p1A2.W * K
                            + indie_h(sA, sc.lights[size_t(fA1.selected)]) * fA1.W * std::min(fA1.M, double(HL))
                            + indie_h(sA, sc.lights[size_t(p1B2.selected)]) * p1B2.W * K;
        CHECK(approx(fA2.S, expSA2));
        CHECK(approx(fA2.W, fA2.S / (fA2.M * fA2.h)));
    }

    // Frame 3: the camera moves, the history stays.
    restir::Camera cam2 = cam;
    cam2.position = {0.3f, 0, 0};
    r.set_camera(cam2);
    r.render_frame();
    CHECK(r.last_frame_hits() == 2);
    CHECK(r.reservoir(0, 0).M == 3 * K);
    CHECK(r.reservoir(1, 0).M == 3 * K);

    // Frame 4: a changed light definition clears the history.
    restir::SceneView sc2 = sc;
    sc2.lights[0].intensity = {0.9f, 0.9f, 0.9f};
    r.set_scene(sc2);
    r.render_frame();
    CHECK(r.reservoir(0, 0).M == 2 * K);

    // Frame 5: an identical light set keeps the history.
    r.set_scene(sc2);
    r.render_frame();
    CHECK(r.reservoir(0, 0).M == 3 * K);
}

CASE(lighting) {
    // The reference entry on the legacy scene reproduces the old recursive results.
    {
        restir::ReservoirRenderer rr(restir::SceneView::legacy(), restir::Camera{}, 8, 6, 1, 1, 5);
        const std::vector<vec3>& ref = rr.render_reference();
        for (int y = 0; y < 6; ++y)
        for (int x = 0; x < 8; ++x) {
            const vec3 old = ::cast_ray(vec3{0, 0, 0}, camera_dir(x, y, 8, 6, restir::Camera{}.fov));
            CHECK(vec_approx(old, ref[size_t(y) * 8 + size_t(x)], 1e-4, 1e-4));
        }
    }

    // Colored lights, an occluder sphere and a moving camera.
    restir::SceneView sc = restir::SceneView::legacy();
    sc.spheres.push_back(Sphere{{-11, 10, 2}, 3.5f, red_rubber}); // occluder
    sc.lights = {
        {{-20, 20,  20}, {1.00f, 0.45f, 0.35f}},
        {{ 30, 50, -25}, {0.35f, 1.00f, 0.45f}},
        {{ 30, 20,  30}, {0.40f, 0.50f, 1.00f}},
        {{-10, 35,  10}, {0.90f, 0.90f, 0.80f}},
        {{  0,  8,  30}, {0.70f, 0.30f, 0.30f}},
    };
    const int W = 48, H = 36, K = 6, HL = 16, N = 256;
    restir::Camera cam0, cam1;
    cam1.position = {0.4f, 0.25f, 0};

    restir::ReservoirRenderer refR(sc, cam0, W, H, 1, 1, 0);
    const std::vector<vec3> ref0 = refR.render_reference();
    refR.set_camera(cam1);
    const std::vector<vec3> ref1 = refR.render_reference();
    CHECK(refR.reference_shadow_rays() > 0);

    std::vector<vec3> sum0(size_t(W) * H, vec3{0, 0, 0}), sum1 = sum0;
    std::vector<vec3> sq0 = sum0, sq1 = sum0;
    std::uint64_t directTot = 0, recTot = 0;
    for (int s = 0; s < N; ++s) {
        restir::ReservoirRenderer r(sc, cam0, W, H, K, HL, std::uint64_t(1000 + s));
        const std::vector<vec3>& f0 = r.render_frame();
        CHECK(r.direct_shadow_rays() == r.last_frame_hits()); // at most one direct shadow ray per valid first hit
        CHECK(r.direct_shadow_rays() <= std::uint64_t(W) * H);
        directTot += r.direct_shadow_rays();
        recTot += r.recursive_shadow_rays();
        std::vector<char> prevHit(size_t(W) * H);
        std::vector<double> prevM(size_t(W) * H);
        for (int i = 0; i < W * H; ++i) {
            prevHit[size_t(i)] = r.first_hit(i % W, i / W) ? 1 : 0;
            prevM[size_t(i)] = r.reservoir(i % W, i / W).M;
            addto(sum0[size_t(i)], f0[size_t(i)]);
            addsq(sq0[size_t(i)], f0[size_t(i)]);
        }
        r.set_camera(cam1); // camera moves keep the history
        const std::vector<vec3>& f1 = r.render_frame();
        CHECK(r.direct_shadow_rays() == r.last_frame_hits());
        directTot += r.direct_shadow_rays();
        recTot += r.recursive_shadow_rays();
        if (s < 3) { // full candidate accounting on a moving camera
            for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const int i = y * W + x;
                if (!r.first_hit(x, y)) {
                    CHECK(r.reservoir(x, y).selected == -1 && r.reservoir(x, y).M == 0);
                    continue;
                }
                double m = K; // own stage-1 reservoir
                if (x > 0     && r.first_hit(x - 1, y)) m += K; // frozen neighbours
                if (x < W - 1 && r.first_hit(x + 1, y)) m += K;
                if (y > 0     && r.first_hit(x, y - 1)) m += K;
                if (y < H - 1 && r.first_hit(x, y + 1)) m += K;
                if (prevHit[size_t(i)]) m += std::min(prevM[size_t(i)], double(HL)); // truncated history
                CHECK(r.reservoir(x, y).M == m);
            }
        }
        for (int i = 0; i < W * H; ++i) {
            addto(sum1[size_t(i)], f1[size_t(i)]);
            addsq(sq1[size_t(i)], f1[size_t(i)]);
        }
    }
    CHECK(recTot > 0); // recursive legacy-path queries are counted separately

    double sumDiff = 0, sumVar = 0, sumAbsDiff = 0, sumAbsRef = 0, maxZ = 0;
    size_t zCount = 0, zTotal = 0;
    vec3 meanRIS{0, 0, 0}, meanRef{0, 0, 0};
    for (int frame = 0; frame < 2; ++frame) {
        const std::vector<vec3>& ref = frame ? ref1 : ref0;
        const std::vector<vec3>& sum = frame ? sum1 : sum0;
        const std::vector<vec3>& sq = frame ? sq1 : sq0;
        for (size_t i = 0; i < ref.size(); ++i)
        for (int ch = 0; ch < 3; ++ch) {
            const double mean = sum[i][ch] / N;
            const double var = std::max(0.0, sq[i][ch] / N - mean * mean);
            const double se = std::sqrt(var / N);
            const double diff = mean - ref[i][ch];
            // floor covers float representation noise on deterministic pixels
            const double z = std::fabs(diff) / (se + 1e-6 * (1.0 + std::fabs(ref[i][ch])));
            maxZ = std::max(maxZ, z);
            zCount += z > 8 ? 1 : 0;
            ++zTotal;
            sumDiff += diff;
            sumVar += var / N;
            sumAbsDiff += std::fabs(diff);
            sumAbsRef += std::fabs(ref[i][ch]);
            meanRIS[ch] += mean;
            meanRef[ch] += ref[i][ch];
        }
    }
    const double npix = 2.0 * W * H;
    std::cout << "ris_mean " << meanRIS.x / npix << " " << meanRIS.y / npix << " " << meanRIS.z / npix
              << " ref_mean " << meanRef.x / npix << " " << meanRef.y / npix << " " << meanRef.z / npix
              << " mean_abs_err " << sumAbsDiff / (npix * 3)
              << " rel_err " << sumAbsDiff / sumAbsRef
              << " max_z " << maxZ
              << " z8_fraction " << double(zCount) / zTotal
              << " pooled_z " << std::fabs(sumDiff) / std::sqrt(sumVar)
              << " direct_shadow_per_frame " << double(directTot) / (2 * N)
              << " recursive_shadow_per_frame " << double(recTot) / (2 * N)
              << std::endl;
    // The 1e-6 density floor keeps cross-surface support, which makes a few
    // pixels heavy-tailed; the pooled mean over all pixels and seeds samples
    // that tail and must match the reference tightly.
    CHECK(maxZ < 200);
    CHECK(zCount * 100 <= zTotal); // at most 1% of pixel channels beyond 8 sigma
    CHECK(std::fabs(sumDiff) < 6.0 * std::sqrt(sumVar) + 1e-6);
    CHECK(sumAbsDiff < 0.02 * sumAbsRef + 1e-6);
}
