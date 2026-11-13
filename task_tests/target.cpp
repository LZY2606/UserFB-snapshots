#include "checks.hpp"
#include "polarization.h"
#include "polarized_renderer.h"
#include <cmath>
#include <complex>

namespace {

using C = std::complex<double>;
using pol::Stokes;
using pol::Mueller;
using pol::PolarState;
using pol_render::PolarScene;
using pol_render::PolarSphere;
using pol_render::CameraRay;

constexpr double PI = 3.14159265358979323846;
constexpr double TOL = 1e-9;
constexpr double LOOSE = 1e-7; // sphere intersection runs in float precision

// ---- reference machinery built from complex E fields ----------------------
// None of these helpers call the project's Mueller functions; they are the
// independent expectation generator required by the task.

Stokes ref_stokes(C e1, C e2) {
    double I = std::norm(e1) + std::norm(e2);
    double Q = std::norm(e1) - std::norm(e2);
    C z = e1 * std::conj(e2);
    return {I, Q, 2.0 * z.real(), 2.0 * z.imag()};
}

// Independent Fresnel amplitudes, same physical formulas.
std::pair<C, C> ref_fresnel(double n1, double n2, double ci) {
    double ct_sq = 1.0 - (n1 / n2) * (n1 / n2) * (1.0 - ci * ci);
    C ct = std::sqrt(C(ct_sq, 0.0));
    C rs = (n1 * ci - n2 * ct) / (n1 * ci + n2 * ct);
    C rp = (n2 * ci - n1 * ct) / (n2 * ci + n1 * ct);
    return {rs, rp};
}

double ref_frame_psi(const dvec3& k, const dvec3& e1, const dvec3& e1_new) {
    dvec3 e2 = cross(k, e1);
    return std::atan2(e2 * e1_new, e1 * e1_new);
}

// Passive rebasing of field components onto a frame rotated by psi.
void ref_rotate_field(C& e1, C& e2, double psi) {
    double c = std::cos(psi), s = std::sin(psi);
    C ne1 = c * e1 + s * e2;
    C ne2 = -s * e1 + c * e2;
    e1 = ne1;
    e2 = ne2;
}

// Local deterministic fallback mirroring the documented frame rule, so the
// reference transport never reaches into the optics module even at normal
// incidence.
dvec3 ref_perpendicular(const dvec3& k) {
    dvec3 a = (std::abs(k.x) < 0.9) ? dvec3{1, 0, 0} : dvec3{0, 1, 0};
    return (a - k * (a * k)).normalized();
}

dvec3 interface_s(const dvec3& N, const dvec3& k_out) {
    dvec3 sc = cross(N, k_out);
    if (sc.norm() < 1e-12) return ref_perpendicular(k_out);
    return sc.normalized();
}

bool physically_valid(const Stokes& s) {
    if (s[0] < -1e-12) return false;
    double pol2 = s[1] * s[1] + s[2] * s[2] + s[3] * s[3];
    return pol2 <= s[0] * s[0] + 1e-9;
}

void near(Check& chk, double got, double expected, double tol, const char* msg) {
    chk(std::abs(got - expected) <= tol * std::max(1.0, std::abs(expected)), msg);
}

void near_stokes(Check& chk, const Stokes& got, const Stokes& ref, double tol, const char* msg) {
    for (int i = 0; i < 4; ++i)
        near(chk, got[i], ref[i], tol, msg);
    chk(physically_valid(got), "output Stokes must stay physically valid");
}

// Field-level recursive transport, mirroring the renderer geometry but never
// using its Mueller/Stokes routines.  Background fields are supplied per
// channel in the deterministic world-projected frame.
struct RefTransport {
    const PolarScene* scene;
    std::array<std::pair<C, C>, 3> bg_field; // field components in reference_axis(k) frame
    int channel = 0;
    int bounces = 0;

    // Fills out1/out2 with the field in the caller frame (e1_out).
    // Returns true only when a surface was hit with the reflection budget
    // exhausted, in which case the field is zero.
    bool run(const dvec3& orig, const dvec3& D, const dvec3& e1_out,
             int depth, C& out1, C& out2) {
        dvec3 k_out = D * -1.0;

        const PolarSphere* hit = nullptr;
        float nearest = 1e30f;
        vec3 forig = to_vec3(orig), fdir = to_vec3(D);
        for (const PolarSphere& ps : scene->spheres) {
            auto [found, t] = ray_sphere_intersect(forig, fdir, ps.sphere);
            if (found && t < nearest) { nearest = t; hit = &ps; }
        }
        if (!hit) {
            // Background field, defined in the deterministic world frame,
            // rotated into this caller's frame.
            dvec3 e1_bg = PolarScene::reference_axis(k_out);
            out1 = bg_field[channel].first;
            out2 = bg_field[channel].second;
            ref_rotate_field(out1, out2, ref_frame_psi(k_out, e1_bg, e1_out));
            return false;
        }
        if (depth >= scene->max_reflections) {
            out1 = out2 = C(0, 0);
            return true;
        }
        ++bounces;

        dvec3 P = orig + D * static_cast<double>(nearest);
        dvec3 N = (P - to_dvec3(hit->sphere.center)).normalized();
        bool inside = (D * N) > 0.0;
        double n1 = inside ? hit->n_inside  : hit->n_outside;
        double n2 = inside ? hit->n_outside : hit->n_inside;

        dvec3 s = interface_s(N, k_out);
        dvec3 Dr = reflect(D, N).normalized();
        dvec3 k_in = Dr * -1.0;

        dvec3 e1_child = PolarScene::reference_axis(k_in);
        C c1, c2;
        bool blocked = run(P, Dr, e1_child, depth + 1, c1, c2);
        if (blocked) { out1 = out2 = C(0, 0); return true; }

        // child field is currently in the child frame; move it into the
        // incoming (s, p_in) interface frame.
        ref_rotate_field(c1, c2, ref_frame_psi(k_in, e1_child, s));

        double ci = std::min(1.0, std::abs(D * N));
        auto [rs, rp] = ref_fresnel(n1, n2, ci);
        C r1 = rs * c1; // s component
        C r2 = rp * c2; // p component, now in the outgoing (s, p_out) frame

        // rotate from the (s, p_out) frame into the caller output frame.
        ref_rotate_field(r1, r2, ref_frame_psi(k_out, s, e1_out));
        out1 = r1;
        out2 = r2;
        return false;
    }
};

Stokes reference_trace(const PolarScene& scene, const CameraRay& ray,
                       const std::array<std::pair<C, C>, 3>& bg_field,
                       int channel, int& bounces) {
    dvec3 D = ray.dir.normalized();
    dvec3 k_out = D * -1.0;
    dvec3 e1_out = PolarScene::reference_axis(k_out);
    RefTransport rt{&scene, bg_field, channel, 0};
    C a, b;
    rt.run(ray.origin, D, e1_out, 0, a, b);
    bounces = rt.bounces;
    return ref_stokes(a, b);
}

PolarSphere make_sphere(dvec3 c, double r, double n_out, double n_in) {
    PolarSphere ps;
    ps.sphere.center = to_vec3(c);
    ps.sphere.radius = float(r);
    ps.n_outside = n_out;
    ps.n_inside = n_in;
    return ps;
}

} // namespace

// ---------------------------------------------------------------------------
// Fresnel amplitudes, Mueller reflection and analyzer against field references
// ---------------------------------------------------------------------------
void test_optics(Check& chk) {
    // Normal incidence: hand-computed coefficients, no division by zero.
    {
        auto [rs, rp] = pol::fresnel_amplitude(1.0, 1.5, 1.0);
        near(chk, rs.real(), -0.2, TOL, "normal rs");
        near(chk, rs.imag(), 0.0, TOL, "normal rs real");
        near(chk, rp.real(),  0.2, TOL, "normal rp");
        near(chk, rp.imag(), 0.0, TOL, "normal rp real");
        near(chk, std::norm(rs), 0.04, TOL, "normal reflectance");
        dvec3 k{0, 0, -1};
        dvec3 e1 = pol::perpendicular_axis(k);
        near(chk, e1.norm(), 1.0, TOL, "fallback axis unit");
        near(chk, e1 * k, 0.0, TOL, "fallback axis perpendicular");
    }

    // Brewster angle: p reflection vanishes, reflected light is pure s.
    {
        double n1 = 1.0, n2 = 1.5;
        double theta_b = std::atan(n2 / n1);
        auto [rs, rp] = pol::fresnel_amplitude(n1, n2, std::cos(theta_b));
        chk(std::abs(rp) < 1e-12, "Brewster rp vanishes");
        chk(std::abs(rs) > 0.1, "Brewster rs stays finite");

        C in1(1.0 / std::sqrt(2.0), 0), in2(1.0 / std::sqrt(2.0), 0);
        Stokes Sin = ref_stokes(in1, in2);
        auto [ers, erp] = ref_fresnel(n1, n2, std::cos(theta_b));
        Stokes expect = ref_stokes(ers * in1, erp * in2);
        Mueller M = pol::reflection_mueller(n1, n2, std::cos(theta_b));
        Stokes got = pol::apply_mueller(M, Sin);
        near_stokes(chk, got, expect, TOL, "Brewster Mueller vs field");
        near(chk, got[pol::Q], got[pol::I], TOL, "Brewster reflected light is s polarized");
        near(chk, got[pol::U], 0.0, TOL, "Brewster U zero");
        near(chk, got[pol::V], 0.0, TOL, "Brewster V zero");
    }

    // Total internal reflection: unit modulus, phase difference couples U/V.
    {
        double n1 = 1.5, n2 = 1.0;
        double ci = std::cos(60.0 * PI / 180.0); // beyond the ~41.8 deg critical angle
        auto [rs, rp] = pol::fresnel_amplitude(n1, n2, ci);
        near(chk, std::abs(rs), 1.0, TOL, "TIR |rs| = 1");
        near(chk, std::abs(rp), 1.0, TOL, "TIR |rp| = 1");
        double delta = std::arg(rs) - std::arg(rp);
        chk(std::abs(delta) > 0.01, "TIR produces a real phase difference");

        C in1(1, 0), in2(1, 0); // 45-degree linear: (I,Q,U,V) = (2,0,2,0)
        Stokes Sin = ref_stokes(in1, in2);
        near(chk, Sin[pol::U], 2.0, TOL, "input U");
        Stokes expect = ref_stokes(rs * in1, rp * in2);
        chk(std::abs(expect[pol::V]) > 0.01, "field reference shows TIR circular component");
        Mueller M = pol::reflection_mueller(n1, n2, ci);
        Stokes got = pol::apply_mueller(M, Sin);
        near_stokes(chk, got, expect, TOL, "TIR Mueller keeps U/V coupled");
        // Explicit phase relations for a diagonal Jones matrix.
        near(chk, got[pol::U],  2.0 * std::cos(delta), TOL, "TIR U = 2 cos delta");
        near(chk, got[pol::V],  2.0 * std::sin(delta), TOL, "TIR V = 2 sin delta");
        near(chk, got[pol::I], 2.0, TOL, "TIR preserves intensity");
    }

    // Generic sweep: Mueller must match field-level Jones on every state.
    {
        const C fields[][2] = {
            {C(1.2, 0.0), C(0.0, 0.0)},
            {C(0.7, 0.3), C(0.5, -0.4)},
            {C(0.2, -0.9), C(0.8, 0.1)},
            {C(1.0, 0.5), C(-0.3, 0.6)},
        };
        for (double deg = 5.0; deg < 85.0; deg += 9.5) {
            for (int swap = 0; swap < 2; ++swap) {
                double n1 = swap ? 1.5 : 1.0;
                double n2 = swap ? 1.0 : 1.5;
                double ci = std::cos(deg * PI / 180.0);
                auto [rs, rp] = ref_fresnel(n1, n2, ci);
                Mueller M = pol::reflection_mueller(n1, n2, ci);
                for (const auto& f : fields) {
                    Stokes Sin = ref_stokes(f[0], f[1]);
                    Stokes expect = ref_stokes(rs * f[0], rp * f[1]);
                    Stokes got = pol::apply_mueller(M, Sin);
                    near_stokes(chk, got, expect, TOL, "Mueller sweep vs field");
                }
            }
        }
    }

    // Ideal linear analyzer, including the full output Stokes state.
    {
        C e1(0.8, 0.2), e2(0.4, -0.7);
        Stokes Sin = ref_stokes(e1, e2);
        for (double theta = 0.0; theta < PI; theta += 0.21) {
            double c = std::cos(theta), s = std::sin(theta);
            C projected = c * e1 + s * e2;
            double expect_i = std::norm(projected);
            Stokes expect = ref_stokes(c * projected, s * projected);
            near(chk, pol::analyzer_intensity(Sin, theta), expect_i, TOL, "analyzer intensity");
            Stokes got = pol::linear_analyzer(Sin, theta);
            near_stokes(chk, got, expect, TOL, "analyzer full state vs field");
        }
        // Crossed analyzers on a linear state block everything.
        Stokes lin{1, 1, 0, 0};
        near(chk, pol::analyzer_intensity(lin, PI / 2), 0.0, TOL, "crossed analyzer");
        near(chk, pol::analyzer_intensity(lin, 0.0), 1.0, TOL, "parallel analyzer");
    }
}

// ---------------------------------------------------------------------------
// Reference frames, rebasing and analyzer orientation invariance
// ---------------------------------------------------------------------------
void test_frames(Check& chk) {
    // Hand-made frame: k along z, e1 along x, e2 = cross(k,e1) = y.
    dvec3 k{0, 0, 1};
    dvec3 e1{1, 0, 0};
    PolarState st;
    st.k = k;
    st.e1 = e1;
    C fe1(0.9, 0.2), fe2(0.3, -0.5);
    Stokes s0 = ref_stokes(fe1, fe2);
    for (auto& s : st.s) s = s0;
    near(chk, st.e2().x, 0.0, TOL, "e2 x");
    near(chk, st.e2().y, 1.0, TOL, "e2 y");

    for (double psi = -1.4; psi <= 1.4; psi += 0.19) {
        dvec3 e1n{std::cos(psi), std::sin(psi), 0};
        PolarState moved = pol::rebase(st, k, e1n);
        near(chk, moved.e1 * moved.k, 0.0, TOL, "rebased axis perpendicular");
        near(chk, moved.e1.norm(), 1.0, TOL, "rebased axis unit");

        // Field-level expectation in the rotated frame.
        C a = fe1, b = fe2;
        ref_rotate_field(a, b, psi);
        Stokes expect = ref_stokes(a, b);
        near_stokes(chk, moved.s[0], expect, TOL, "rebase vs field rotation");
        near_stokes(chk, moved.s[2], expect, TOL, "rebase per channel");

        // Going back must be the identity.
        PolarState back = pol::rebase(moved, k, e1);
        near_stokes(chk, back.s[0], s0, TOL, "rebase inverse");

        // Analyzer turned to the same world direction measures the same power.
        // A direction at angle theta in the old frame has coordinate theta-psi
        // in the frame rotated by psi.
        for (double theta = 0.0; theta < PI; theta += 0.37) {
            double p_old = pol::analyzer_intensity(st.s[0], theta);
            double p_new = pol::analyzer_intensity(moved.s[0], theta - psi);
            near(chk, p_new, p_old, TOL, "analyzer orientation invariance");
        }
    }

    // The deterministic world-projected frame is unit, orthogonal and stable.
    const dvec3 dirs[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, -1},
                          {0.3, -0.7, 0.65}, {-0.2, 0.9, 0.4}};
    for (dvec3 kk : dirs) {
        kk = kk.normalized();
        dvec3 ax = PolarScene::reference_axis(kk);
        near(chk, ax.norm(), 1.0, TOL, "reference axis unit");
        near(chk, ax * kk, 0.0, TOL, "reference axis perpendicular");
        dvec3 again = PolarScene::reference_axis(kk);
        near(chk, (ax - again).norm(), 0.0, TOL, "reference axis deterministic");
        dvec3 e2 = cross(kk, ax);
        near(chk, e2.norm(), 1.0, TOL, "e2 unit");
    }

    // I/V never change under a pure frame rotation.
    Stokes sv{1.3, 0.4, -0.2, 0.5};
    Stokes rv = pol::rotate_stokes(sv, 0.83);
    near(chk, rv[pol::I], sv[pol::I], TOL, "rotation keeps I");
    near(chk, rv[pol::V], sv[pol::V], TOL, "rotation keeps V");
}

// ---------------------------------------------------------------------------
// Full recursive transport: real non-coplanar double reflections, depth,
// background, camera-roll frame independence
// ---------------------------------------------------------------------------
namespace {

PolarScene two_sphere_scene() {
    PolarScene scene;
    scene.spheres.push_back(make_sphere({-2.0, 0.0, -9.0}, 2.0, 1.5, 1.0));
    scene.spheres.push_back(make_sphere({ 3.0, 1.0, -8.0}, 2.0, 1.0, 1.5));
    scene.max_reflections = 6;
    return scene;
}

std::array<std::pair<C, C>, 3> make_bg_fields() {
    return {{
        {C(0.80, 0.0), C(0.30, 0.10)},
        {C(0.75, 0.05), C(0.35, 0.0)},
        {C(0.70, -0.08), C(0.40, 0.12)},
    }};
}

void install_background(PolarScene& scene,
                        const std::array<std::pair<C, C>, 3>& f) {
    for (int ch = 0; ch < 3; ++ch)
        scene.background.stokes[ch] = ref_stokes(f[ch].first, f[ch].second);
}

} // namespace

void test_transport(Check& chk) {
    PolarScene scene = two_sphere_scene();
    auto bg = make_bg_fields();
    install_background(scene, bg);

    // Miss: background comes back, expressed in the caller frame.
    {
        CameraRay ray{{0, 0, 0}, dvec3{0, 1, 0}.normalized()};
        PolarState got = scene.trace(ray);
        dvec3 kexpect = ray.dir.normalized() * -1.0;
        near(chk, (got.k - kexpect).norm(), 0.0, TOL, "miss k = -D");
        near(chk, (got.e1 - PolarScene::reference_axis(kexpect)).norm(), 0.0, TOL, "miss frame");
        for (int ch = 0; ch < 3; ++ch)
            near_stokes(chk, got.s[ch], scene.background.stokes[ch], TOL, "miss returns background");
    }

    // Scan camera rays until a path with two non-coplanar reflections is found.
    CameraRay multi{{0, 0, 0}, {0, 0, -1}};
    bool found_multi = false;
    dvec3 s_axes[2];
    for (double ay = -0.35; ay <= 0.35 && !found_multi; ay += 0.05) {
        for (double ax = -0.6; ax <= 0.6 && !found_multi; ax += 0.05) {
            CameraRay cand{{0, 0, 0}, dvec3{ax, ay, -1}.normalized()};
            int bounces = 0;
            reference_trace(scene, cand, bg, 0, bounces);
            if (bounces >= 2) {
                multi = cand;
                found_multi = true;
            }
        }
    }
    chk(found_multi, "scene contains a real two-bounce path");

    if (found_multi) {
        // Full Stokes on every channel must match the field-level recursion.
        PolarState got = scene.trace(multi);
        for (int ch = 0; ch < 3; ++ch) {
            int bounces = 0;
            Stokes expect = reference_trace(scene, multi, bg, ch, bounces);
            chk(bounces >= 2, "reference path really bounces twice");
            near_stokes(chk, got.s[ch], expect, LOOSE, "two-bounce transport vs field");
        }
        // The two reflection planes must differ: no scalar angle factor could
        // reproduce the cross-coupling observed along this path.
        dvec3 D = multi.dir.normalized();
        dvec3 k_out = D * -1.0;
        const PolarSphere* first = nullptr;
        float nearest = 1e30f;
        for (const PolarSphere& ps : scene.spheres) {
            auto [hit2, t] = ray_sphere_intersect(to_vec3(multi.origin), to_vec3(D), ps.sphere);
            if (hit2 && t < nearest) { nearest = t; first = &ps; }
        }
        chk(first != nullptr, "first bounce located");
        dvec3 P1 = multi.origin + D * static_cast<double>(nearest);
        dvec3 N1 = (P1 - to_dvec3(first->sphere.center)).normalized();
        s_axes[0] = interface_s(N1, k_out);
        dvec3 Dr = reflect(D, N1).normalized();
        dvec3 k_in = Dr * -1.0;
        const PolarSphere* second = nullptr;
        nearest = 1e30f;
        for (const PolarSphere& ps : scene.spheres) {
            auto [hit2, t] = ray_sphere_intersect(to_vec3(P1), to_vec3(Dr), ps.sphere);
            if (hit2 && t < nearest) { nearest = t; second = &ps; }
        }
        chk(second != nullptr && second != first, "second bounce is another sphere");
        dvec3 P2 = P1 + Dr * static_cast<double>(nearest);
        dvec3 N2 = (P2 - to_dvec3(second->sphere.center)).normalized();
        s_axes[1] = interface_s(N2, k_in);
        double parallel = std::abs(s_axes[0] * s_axes[1]);
        chk(parallel < 0.99, "the two reflections are non-coplanar");

        // Reflection budget: counted in actual surface events.
        PolarScene limited = scene;
        limited.max_reflections = 1;
        PolarState cut = limited.trace(multi);
        for (int ch = 0; ch < 3; ++ch) {
            near(chk, cut.s[ch][pol::I], 0.0, TOL, "budget exhausted returns zero");
            for (int i = 1; i < 4; ++i)
                near(chk, cut.s[ch][i], 0.0, TOL, "exhausted state is zero");
        }
        limited.max_reflections = 2;
        PolarState kept = limited.trace(multi);
        PolarState full = scene.trace(multi);
        for (int ch = 0; ch < 3; ++ch)
            near_stokes(chk, kept.s[ch], full.s[ch], LOOSE, "two-event budget keeps the path");

        // Energy not reflected leaves the channel: output cannot exceed the
        // background energy it ultimately samples.
        for (int ch = 0; ch < 3; ++ch)
            chk(got.s[ch][pol::I] <= scene.background.stokes[ch][pol::I] + 1e-9,
                "reflection path only loses energy");
    }

    // Single-bounce path compared channel by channel with the field reference.
    {
        CameraRay one{{0, 0, 0}, dvec3{0.1, 0.05, -1}.normalized()};
        int bounces = 0;
        reference_trace(scene, one, bg, 0, bounces);
        if (bounces == 1) {
            PolarState got = scene.trace(one);
            for (int ch = 0; ch < 3; ++ch) {
                int b2 = 0;
                Stokes expect = reference_trace(scene, one, bg, ch, b2);
                near_stokes(chk, got.s[ch], expect, LOOSE, "single-bounce transport vs field");
            }
        }
    }

    // Camera roll / repeated traces never inherit a previous frame.
    {
        CameraRay r1{{0, 0, 0}, dvec3{0.2, 0.1, -1}.normalized()};
        CameraRay r2{{0, 0, 0}, dvec3{-0.3, 0.25, -1}.normalized()};
        PolarState a = scene.trace(r1);
        PolarState b = scene.trace(r2);
        PolarState a2 = scene.trace(r1);
        for (const PolarState* st : {&a, &b, &a2}) {
            dvec3 kk = st->k;
            near(chk, (st->e1 - PolarScene::reference_axis(kk)).norm(), 0.0, TOL,
                 "each trace rebuilds its own frame");
            near(chk, st->e1 * st->k, 0.0, TOL, "frame stays orthogonal");
            near(chk, st->e2().norm(), 1.0, TOL, "frame stays right-handed unit");
        }
        near_stokes(chk, a2.s[1], a.s[1], TOL, "repeated trace is independent of history");

        // Same physical light, different frame, analyzer at the same world
        // orientation must give identical power.
        dvec3 alt = pol::perpendicular_axis(a.k);
        PolarState rebased = pol::rebase(a, a.k, alt);
        double psi = ref_frame_psi(a.k, a.e1, alt);
        for (int ch = 0; ch < 3; ++ch) {
            for (double theta = 0.0; theta < PI; theta += 0.4) {
                double p1 = pol::analyzer_intensity(a.s[ch], theta);
                double p2 = pol::analyzer_intensity(rebased.s[ch], theta - psi);
                near(chk, p2, p1, TOL, "rolled frame analyzer equivalence");
            }
        }
    }

    // Ray starting inside a sphere.  A reflection-only path in a closed convex
    // object can never reach the background: it must bounce until the budget
    // is exhausted and then return zero.  Along the way the first event is an
    // inside-side hit beyond the critical angle, so the n1 = n_inside TIR
    // branch (whose U/V phase is field-checked in test_optics) is exercised.
    {
        PolarScene inner;
        inner.spheres.push_back(make_sphere({0, 0, 0}, 6.0, 1.0, 1.5));
        inner.max_reflections = 4;
        auto bg2 = make_bg_fields();
        install_background(inner, bg2);

        dvec3 o{0, 5.0, 0}, D{1, 0, 0};
        // Independently locate the first wall hit and classify it.
        const PolarSphere& wall = inner.spheres[0];
        auto [found, t] = ray_sphere_intersect(to_vec3(o), to_vec3(D), wall.sphere);
        chk(found, "interior ray meets the wall");
        dvec3 P = o + D * static_cast<double>(t);
        dvec3 N = (P - to_dvec3(wall.sphere.center)).normalized();
        chk(D * N > 0.0, "first hit is approached from the inside");
        double ci_hit = std::abs(D * N);
        double ci_crit = std::sqrt(1.0 - (1.0 / 1.5) * (1.0 / 1.5));
        chk(ci_hit < ci_crit, "inside incidence is beyond the critical angle");
        auto [rs, rp] = ref_fresnel(1.5, 1.0, ci_hit);
        near(chk, std::abs(rs), 1.0, TOL, "inside TIR |rs| = 1");
        near(chk, std::abs(rp), 1.0, TOL, "inside TIR |rp| = 1");

        CameraRay ray{o, D};
        int bounces = 0;
        Stokes expect = reference_trace(inner, ray, bg2, 0, bounces);
        chk(bounces == inner.max_reflections, "interior path uses the full budget");
        PolarState got = inner.trace(ray);
        near_stokes(chk, got.s[0], expect, LOOSE, "trapped interior ray matches field reference");
        for (int ch = 0; ch < 3; ++ch)
            near(chk, got.s[ch][pol::I], 0.0, TOL, "trapped reflection-only ray ends in zero");
    }
}
