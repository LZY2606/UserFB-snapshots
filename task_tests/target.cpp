#include "checks.hpp"

#include "polarization.h"
#include "polarized_renderer.h"

#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace {

using C = std::complex<double>;
using pol::Mueller;
using pol::Stokes;
using pol::StokesRGB;
using pol::Vec3;

constexpr double PI = 3.14159265358979323846;

bool near(double a, double b, double eps = 1e-8) {
    double scale = std::max({1.0, std::abs(a), std::abs(b)});
    return std::abs(a - b) <= eps * scale;
}

void expect_near(Check& check, double a, double b, double eps,
                  const char* message) {
    check(near(a, b, eps), message);
}

Stokes stokes_from_jones(C e1, C e2) {
    Stokes s;
    s.I = std::norm(e1) + std::norm(e2);
    s.Q = std::norm(e1) - std::norm(e2);
    C z = e1 * std::conj(e2);
    s.U = 2.0 * z.real();
    s.V = 2.0 * z.imag();
    return s;
}

struct CFresnel {
    C rs, rp, ct;
};

// Deliberately private reference implementation: tests do not derive their
// expected Stokes values through pol::Mueller application.
CFresnel reference_fresnel(double n1, double n2, double ci) {
    ci = std::max(0.0, std::min(1.0, ci));
    double eta = n1 / n2;
    double ct2 = 1.0 - eta * eta * (1.0 - ci * ci);
    C ct = ct2 >= 0.0 ? C(std::sqrt(ct2), 0.0)
                      : C(0.0, std::sqrt(-ct2));
    C rs = (n1 * ci - n2 * ct) / (n1 * ci + n2 * ct);
    C rp = (n2 * ci - n1 * ct) / (n2 * ci + n1 * ct);
    return {rs, rp, ct};
}

Stokes reflect_jones_reference(C e1, C e2, C rs, C rp) {
    return stokes_from_jones(rs * e1, rp * e2);
}

struct CVec3 {
    C x, y, z;
};

CVec3 operator*(const Vec3& a, C b) {
    return {C(a.x) * b, C(a.y) * b, C(a.z) * b};
}
CVec3 operator*(C b, const Vec3& a) { return a * b; }
CVec3 operator+(const CVec3& a, const CVec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
C component(const CVec3& e, const Vec3& axis) {
    return e.x * axis.x + e.y * axis.y + e.z * axis.z;
}

void check_stokes(Check& check, const Stokes& got, const Stokes& expected,
                   double eps, const char* prefix) {
    char buffer[256];
    for (int k = 0; k < 4; ++k) {
        bool ok = near(got[k], expected[k], eps);
        if (!ok) {
            std::snprintf(buffer, sizeof(buffer),
                          "%s component %d mismatch: got %.12g expected %.12g",
                          prefix, k, got[k], expected[k]);
            check(false, buffer);
        } else {
            check(true, prefix);
        }
    }
}

PolarizedScene make_two_bounce_scene(StokesRGB background) {
    const double a = 1.0 / std::sqrt(2.0);

    // Camera O=(0,0,0), D0=(0,0,-1).
    // P1=(0,0,-10), N1=( a,0, a) => reflect(D0,N1)=(1,0,0).
    // P2=(5,0,-10), N2=(-a,a, 0) => reflect((1,0,0),N2)=(0,1,0).
    PolarizedSphere first;
    first.radius = 1.0f;
    first.center = vec3{float(-a), 0.0f, float(-10.0 - a)};
    first.n_out = 1.0;
    first.n_in = 1.5;

    PolarizedSphere second;
    second.radius = 1.0f;
    second.center = vec3{float(5.0 + a), float(-a), -10.0f};
    second.n_out = 1.0;
    second.n_in = 1.5;

    PolarizedScene scene;
    scene.spheres = {first, second};
    scene.background = background;
    scene.max_reflections = 2;
    return scene;
}

} // namespace

void test_optics(Check& check) {
    // Normal-incidence Fresnel coefficients.
    {
        auto f = pol::fresnel(1.0, 1.5, 1.0);
        expect_near(check, f.rs.real(), -0.2, 1e-12, "normal rs air->glass");
        expect_near(check, f.rs.imag(), 0.0, 1e-12, "normal rs real");
        expect_near(check, f.rp.real(), 0.2, 1e-12, "normal rp air->glass");
        expect_near(check, f.rp.imag(), 0.0, 1e-12, "normal rp real");

        auto g = pol::fresnel(1.5, 1.0, 1.0);
        expect_near(check, g.rs.real(), 0.2, 1e-12, "normal rs glass->air");
        expect_near(check, g.rp.real(), -0.2, 1e-12, "normal rp glass->air");
    }

    // Compare the Mueller implementation against independent complex fields.
    {
        const double n1 = 1.0, n2 = 1.5, ci = 1.0;
        CFresnel rf = reference_fresnel(n1, n2, ci);
        Mueller M = pol::reflection_mueller(n1, n2, ci);

        std::array<std::pair<C, C>, 6> fields = {{
            {1.0, 0.0},
            {0.0, 1.0},
            {C(1.0 / std::sqrt(2.0)), C(1.0 / std::sqrt(2.0))},
            {C(1.0 / std::sqrt(2.0)), C(0.0, 1.0 / std::sqrt(2.0))},
            {C(0.8, -0.1), C(0.25, 0.5)},
            {C(-0.4, 0.7), C(0.6, -0.2)}
        }};

        for (std::size_t i = 0; i < fields.size(); ++i) {
            C e1 = fields[i].first;
            C e2 = fields[i].second;
            Stokes input = stokes_from_jones(e1, e2);
            Stokes expected = reflect_jones_reference(e1, e2, rf.rs, rf.rp);
            Stokes got = M * input;
            check(pol::physically_valid(got, 1e-9),
                  "normal reflected Stokes remains physical");
            check_stokes(check, got, expected, 1e-11, "normal Jones reference");
        }
    }

    // Brewster angle: rp vanishes and reflection of unpolarized light is
    // purely s-polarized.
    {
        const double n1 = 1.0, n2 = 1.5;
        const double tan_theta = n2 / n1;
        const double ci = 1.0 / std::sqrt(1.0 + tan_theta * tan_theta);
        expect_near(check, ci, n1 / std::sqrt(n1 * n1 + n2 * n2), 1e-12,
                    "Brewster cosine");

        auto f = pol::fresnel(n1, n2, ci);
        check(std::abs(f.rp) < 1e-12, "rp vanishes at Brewster");
        check(std::abs(f.rs) > 0.1, "rs remains nonzero at Brewster");

        Mueller M = pol::reflection_mueller(n1, n2, ci);
        Stokes unpolarized{1.0, 0.0, 0.0, 0.0};
        Stokes reflected = M * unpolarized;
        expect_near(check, reflected.Q, reflected.I, 1e-10,
                    "Brewster reflection is pure s in Q sign");
        expect_near(check, reflected.U, 0.0, 1e-10, "Brewster U zero");
        expect_near(check, reflected.V, 0.0, 1e-10, "Brewster V zero");

        CFresnel rf = reference_fresnel(n1, n2, ci);
        C brew_e1(0.6, 0.1), brew_e2(0.3, -0.55);
        Stokes input = stokes_from_jones(brew_e1, brew_e2);
        Stokes expected = reflect_jones_reference(
            brew_e1, brew_e2, rf.rs, rf.rp);
        check_stokes(check, M * input, expected, 1e-10,
                     "Brewster complex-field reference");
    }

    // Total internal reflection must retain a nontrivial rs/rp phase
    // difference and therefore couple U and V.
    {
        const double n1 = 1.5, n2 = 1.0;
        const double ci = 0.5; // 60 degrees from normal, above critical angle
        auto f = pol::fresnel(n1, n2, ci);
        expect_near(check, std::abs(f.rs), 1.0, 1e-12, "TIR |rs|=1");
        expect_near(check, std::abs(f.rp), 1.0, 1e-12, "TIR |rp|=1");
        check(std::abs(f.ct.imag()) > 0.8, "TIR ct has positive imaginary part");

        double phase = std::arg(f.rs) - std::arg(f.rp);
        check(std::abs(std::sin(phase)) > 0.2,
              "TIR produces measurable s/p phase difference");

        CFresnel rf = reference_fresnel(n1, n2, ci);
        Mueller M = pol::reflection_mueller(n1, n2, ci);

        Stokes linear45 = stokes_from_jones(
            C(1.0 / std::sqrt(2.0)), C(1.0 / std::sqrt(2.0)));
        check(std::abs(linear45.U - 1.0) < 1e-12, "test fixture U=1");
        Stokes expected = reflect_jones_reference(
            C(1.0 / std::sqrt(2.0)), C(1.0 / std::sqrt(2.0)),
            rf.rs, rf.rp);
        Stokes got = M * linear45;
        check_stokes(check, got, expected, 1e-11, "TIR U/V phase reference");
        check(std::abs(got.V) > 0.2, "TIR rotates linear U into nonzero V");
        expect_near(check, got.I, 1.0, 1e-10, "TIR conserves total intensity");
        check(pol::physically_valid(got, 1e-9), "TIR output physical");

        C circ_e1(1.0 / std::sqrt(2.0), 0.0);
        C circ_e2(0.0, 1.0 / std::sqrt(2.0));
        Stokes circularish = stokes_from_jones(circ_e1, circ_e2);
        Stokes expected2 = reflect_jones_reference(
            circ_e1, circ_e2, rf.rs, rf.rp);
        check_stokes(check, M * circularish, expected2, 1e-11,
                     "TIR V/U phase reference");
    }

    // Analyzer formula and physical cone on random finite valid states.
    {
        for (int i = 0; i < 24; ++i) {
            double I = 1.0 + 0.37 * i;
            double radius = 0.7 * I;
            double a = 0.13 * i;
            Stokes s{I, radius * std::cos(a), radius * 0.4 * std::sin(a),
                     radius * 0.9};
            // Reduce one component so the cone is safely inside I.
            s.U *= 0.5;
            check(pol::physically_valid(s, 1e-12), "valid random fixture");
            for (double theta : {0.0, 0.2, PI / 4.0, 1.1, PI / 2.0}) {
                double intensity = pol::analyzer_intensity(s, theta);
                double expected = 0.5 * (s.I + s.Q * std::cos(2 * theta)
                                           + s.U * std::sin(2 * theta));
                expect_near(check, intensity, expected, 1e-13,
                            "analyzer intensity formula");
                check(intensity >= -1e-10, "analyzer intensity nonnegative");

                Stokes analyzed = pol::linear_analyzer(theta) * s;
                expect_near(check, analyzed.I, intensity, 1e-12,
                            "analyzer Mueller I row");
                expect_near(check, analyzed.Q,
                            intensity * std::cos(2.0 * theta), 1e-10,
                            "analyzer output Q lies on axis");
                expect_near(check, analyzed.U,
                            intensity * std::sin(2.0 * theta), 1e-10,
                            "analyzer output U lies on axis");
                expect_near(check, analyzed.V, 0.0, 1e-10,
                            "ideal analyzer removes V coherence");
                check(pol::physically_valid(analyzed, 1e-9),
                      "analyzed state physical");
            }
        }
    }
}

void test_frames(Check& check) {
    Vec3 k = pol::normalize(Vec3(0.31, -0.57, 0.76));
    Vec3 e1 = pol::world_reference_axis(k);
    Vec3 e2 = pol::cross(k, e1);

    check(near(pol::dot(k, e1), 0.0, 1e-13), "world e1 perpendicular k");
    check(near(pol::norm(k), 1.0, 1e-13), "k unit");
    check(near(pol::norm(e1), 1.0, 1e-13), "e1 unit");
    check(near(pol::norm(e2), 1.0, 1e-13), "e2 unit");
    check(near(pol::dot(k, e2), 0.0, 1e-13), "e2 perpendicular k");
    Vec3 handed = pol::cross(e1, e2);
    check(near(handed.x, k.x, 1e-13) && near(handed.y, k.y, 1e-13) &&
          near(handed.z, k.z, 1e-13),
          "frame is right handed: cross(e1,e2)=k");

    pol::PolarizationState state;
    state.k = k;
    state.e1 = e1;
    for (int ch = 0; ch < 3; ++ch) {
        state.stokes[ch] = {2.0 + ch * 0.2, 0.4 - 0.1 * ch,
                            -0.3 + 0.2 * ch, 0.25 + 0.05 * ch};
    }
    check(pol::physically_valid(state.stokes, 1e-12), "frame fixture physical");

    for (double psi : {0.0, 0.17, PI / 4.0, -0.63, 1.21}) {
        pol::PolarizationState r = pol::rotate_reference(state, psi);
        double c1 = std::cos(psi), s1 = std::sin(psi);
        Vec3 expected_e1 = pol::normalize(c1 * e1 + s1 * e2);
        expect_near(check, r.k.x, k.x, 1e-13, "rotation keeps k x");
        expect_near(check, r.k.y, k.y, 1e-13, "rotation keeps k y");
        expect_near(check, r.k.z, k.z, 1e-13, "rotation keeps k z");
        expect_near(check, r.e1.x, expected_e1.x, 1e-12, "rotated e1 x");
        expect_near(check, r.e1.y, expected_e1.y, 1e-12, "rotated e1 y");
        expect_near(check, r.e1.z, expected_e1.z, 1e-12, "rotated e1 z");

        double c2 = std::cos(2 * psi), s2 = std::sin(2 * psi);
        for (int ch = 0; ch < 3; ++ch) {
            const Stokes& a = state.stokes[ch];
            const Stokes& b = r.stokes[ch];
            expect_near(check, b.I, a.I, 1e-13, "rotation preserves I");
            expect_near(check, b.V, a.V, 1e-13, "rotation preserves V");
            expect_near(check, b.Q, c2 * a.Q + s2 * a.U, 1e-12,
                        "Q rotation formula");
            expect_near(check, b.U, -s2 * a.Q + c2 * a.U, 1e-12,
                        "U rotation formula");
        }
    }

    // change_basis to an explicit target is the same signed rotation.
    for (double beta : {0.3, -0.8, 1.4}) {
        Vec3 target = pol::normalize(std::cos(beta) * e1 + std::sin(beta) * e2);
        pol::PolarizationState via_basis = pol::change_basis(state, target);
        pol::PolarizationState via_rotate = pol::rotate_reference(state, beta);
        for (int ch = 0; ch < 3; ++ch) {
            check_stokes(check, via_basis.stokes[ch],
                         via_rotate.stokes[ch], 1e-12, "change_basis=rotate");
        }
        expect_near(check, via_basis.e1.x, target.x, 1e-12, "target e1 x");
        expect_near(check, via_basis.e1.y, target.y, 1e-12, "target e1 y");
        expect_near(check, via_basis.e1.z, target.z, 1e-12, "target e1 z");
    }

    // A basis is only a representation.  Rebase, then place the analyzer at
    // the same world axis; measured linear intensity must be identical.
    for (double beta : {0.25, -0.9, 1.17}) {
        Vec3 new_e1 = pol::normalize(std::cos(beta) * e1 + std::sin(beta) * e2);
        pol::PolarizationState rebased = pol::change_basis(state, new_e1);
        Vec3 new_e2 = rebased.e2();

        for (double alpha : {0.1, 0.8, -0.55, 2.0}) {
            Vec3 world_axis = pol::normalize(
                std::cos(alpha) * e1 + std::sin(alpha) * e2);
            double old_theta = std::atan2(pol::dot(world_axis, e2),
                                          pol::dot(world_axis, e1));
            double new_theta = std::atan2(pol::dot(world_axis, new_e2),
                                          pol::dot(world_axis, new_e1));
            for (int ch = 0; ch < 3; ++ch) {
                double before = pol::analyzer_intensity(state.stokes[ch],
                                                        old_theta);
                double after = pol::analyzer_intensity(rebased.stokes[ch],
                                                       new_theta);
                expect_near(check, before, after, 1e-11,
                            "same world analyzer after rebase");
            }

            pol::PolarizationState direct = pol::change_basis(state, world_axis);
            for (int ch = 0; ch < 3; ++ch) {
                double along_axis = pol::analyzer_intensity(direct.stokes[ch], 0.0);
                double before = pol::analyzer_intensity(state.stokes[ch], old_theta);
                expect_near(check, along_axis, before, 1e-11,
                            "analyzer zero after direct rebase");
            }
        }
    }

    // Camera ray/roll helpers are pure and do not retain a previous basis.
    vec3 ra = polarized_camera_ray(13, 29, 64, 48, 1.05f, 0.33);
    vec3 rb = polarized_camera_ray(13, 29, 64, 48, 1.05f, 0.33);
    expect_near(check, ra.x, rb.x, 0.0, "repeat camera ray x exact");
    expect_near(check, ra.y, rb.y, 0.0, "repeat camera ray y exact");
    expect_near(check, ra.z, rb.z, 0.0, "repeat camera ray z exact");
    vec3 rc = polarized_camera_ray(13, 29, 64, 48, 1.05f, -0.41);
    check(std::abs(ra.x - rc.x) + std::abs(ra.y - rc.y) > 1e-6f,
          "camera roll changes sensor direction");
}

void test_transport(Check& check) {
    const double a = 1.0 / std::sqrt(2.0);

    std::array<std::pair<C, C>, 3> jones = {{
        {1.0, 0.0},
        {0.0, 1.0},
        {C(0.8, 0.1), C(0.3, -0.5)}
    }};
    StokesRGB background;
    for (int ch = 0; ch < 3; ++ch)
        background[ch] = stokes_from_jones(jones[ch].first, jones[ch].second);

    PolarizedScene scene = make_two_bounce_scene(background);

    // Verify that the shared sphere test really realizes the intended path.
    {
        auto [hit0, t0] = ray_sphere_intersect(
            {0, 0, 0}, {0, 0, -1}, scene.spheres[0].center,
            scene.spheres[0].radius);
        check(hit0 && std::abs(t0 - 10.0f) < 2e-5f,
              "primary ray hits first designed sphere");

        vec3 p1{0, 0, -10};
        auto [hit1, t1] = ray_sphere_intersect(
            p1, {1, 0, 0}, scene.spheres[1].center,
            scene.spheres[1].radius);
        check(hit1 && std::abs(t1 - 5.0f) < 2e-5f,
              "reflected ray hits second non-coplanar sphere");
    }

    PolarizedRenderer renderer(scene);
    Vec3 top_e1(1, 0, 0);
    PolarizedTraceResult two = renderer.trace_with_count(
        {0, 0, -1}, &top_e1);
    check(two.hit, "two-bounce trace reports hit");
    check(two.reflection_events == 2, "two actual surface reflection events");
    expect_near(check, two.state.k.x, 0.0, 1e-6, "final k +z x");
    expect_near(check, two.state.k.y, 0.0, 1e-6, "final k +z y");
    expect_near(check, two.state.k.z, 1.0, 1e-6, "final k +z z");
    expect_near(check, two.state.e1.x, 1.0, 1e-6, "requested top e1 x");
    expect_near(check, two.state.e1.y, 0.0, 1e-6, "requested top e1 y");
    expect_near(check, two.state.e1.z, 0.0, 1e-6, "requested top e1 z");

    // Independent world-space complex-field transport through both surfaces.
    Vec3 k_bg(0, -1, 0);
    Vec3 e_bg1(1, 0, 0);
    Vec3 e_bg2 = pol::cross(k_bg, e_bg1); // +z
    check(near(e_bg2.z, 1.0), "background reference is +z");

    Vec3 N2(-a, a, 0), k_out2(-1, 0, 0);
    Vec3 s2(0, 0, 1);
    Vec3 p_in2(-1, 0, 0), p_out2(0, 1, 0);
    Vec3 N1(a, 0, a), s1(0, -1, 0);
    Vec3 p_in1(0, 0, 1), p_out1(1, 0, 0);
    (void)N2; (void)N1; (void)k_out2;

    CFresnel f2 = reference_fresnel(1.0, 1.5, a);
    CFresnel f1 = reference_fresnel(1.0, 1.5, a);

    for (int ch = 0; ch < 3; ++ch) {
        CVec3 E = jones[ch].first * e_bg1 + jones[ch].second * e_bg2;

        C es2 = component(E, s2);
        C ep2 = component(E, p_in2);
        CVec3 out2 = (f2.rs * es2) * s2 + (f2.rp * ep2) * p_out2;

        C es1 = component(out2, s1);
        C ep1 = component(out2, p_in1);
        CVec3 final_e = (f1.rs * es1) * s1 + (f1.rp * ep1) * p_out1;

        C top1 = component(final_e, Vec3(1, 0, 0));
        C top2 = component(final_e, Vec3(0, 1, 0));
        Stokes expected = stokes_from_jones(top1, top2);
        check_stokes(check, two.state.stokes[ch], expected, 2e-5,
                     "non-coplanar two-reflection recursion");
        check(pol::physically_valid(two.state.stokes[ch], 1e-7),
              "transport output physical");
    }

    // Normal incidence must use the deterministic perpendicular s fallback
    // rather than dividing by cross(N,k)=0.  Check the complete Jones path.
    {
        std::array<std::pair<C, C>, 3> normal_jones = {{
            {C(0.70, 0.10), C(0.35, -0.45)},
            {C(1.0, 0.0), C(0.0, 0.6)},
            {C(0.2, -0.4), C(0.8, 0.1)}
        }};
        StokesRGB normal_background;
        for (int ch = 0; ch < 3; ++ch)
            normal_background[ch] = stokes_from_jones(
                normal_jones[ch].first, normal_jones[ch].second);

        PolarizedSphere normal_sphere;
        normal_sphere.center = {0.0f, 0.0f, -10.0f};
        normal_sphere.radius = 1.0f;
        normal_sphere.n_out = 1.0;
        normal_sphere.n_in = 1.5;

        PolarizedScene normal_scene;
        normal_scene.spheres = {normal_sphere};
        normal_scene.background = normal_background;
        normal_scene.max_reflections = 1;

        PolarizedRenderer normal_renderer(normal_scene);
        Vec3 requested_y(0, 1, 0);
        PolarizedTraceResult normal_result =
            normal_renderer.trace_with_count({0, 0, -1}, &requested_y);
        check(normal_result.hit, "normal sphere hit");
        check(normal_result.reflection_events == 1,
              "normal sphere one surface event");
        expect_near(check, normal_result.state.e1.x, 0.0, 1e-6,
                    "normal fallback s/world e1 is +y x");
        expect_near(check, normal_result.state.e1.y, 1.0, 1e-6,
                    "normal fallback s/world e1 is +y y");
        expect_near(check, normal_result.state.e1.z, 0.0, 1e-6,
                    "normal fallback s/world e1 is +y z");

        // Background k=-z has e1=+y and e2=cross(-z,+y)=+x.
        // At the hit s=+y, p_in=+x, p_out=-x.  In the requested top basis
        // (e1=+y,e2=-x), both diagonal coefficients are simply rs,rp.
        for (int ch = 0; ch < 3; ++ch) {
            C top_e1 = C(-0.2) * normal_jones[ch].first;
            C top_e2 = C(0.2) * normal_jones[ch].second;
            Stokes expected_normal = stokes_from_jones(top_e1, top_e2);
            check_stokes(check, normal_result.state.stokes[ch],
                         expected_normal, 1e-6,
                         "normal-incidence deterministic frame transport");
            check(pol::physically_valid(normal_result.state.stokes[ch], 1e-7),
                  "normal transport physical");
        }

        PolarizedTraceResult again =
            normal_renderer.trace_with_count({0, 0, -1}, &requested_y);
        for (int ch = 0; ch < 3; ++ch)
            check_stokes(check, again.state.stokes[ch],
                         normal_result.state.stokes[ch], 1e-13,
                         "normal trace is repeatable");
    }

    // Budget semantics: a further hit with no remaining reflection budget is
    // zero, not background.
    PolarizedScene one_scene = scene;
    one_scene.max_reflections = 1;
    PolarizedRenderer one(one_scene);
    PolarizedTraceResult one_result = one.trace_with_count(
        {0, 0, -1}, &top_e1);
    check(one_result.hit, "budget-one primary reports hit");
    check(one_result.reflection_events == 1, "one processed reflection event");
    for (int ch = 0; ch < 3; ++ch) {
        expect_near(check, one_result.state.stokes[ch].I, 0.0, 1e-12,
                    "exhausted second surface returns zero I");
        expect_near(check, one_result.state.stokes[ch].Q, 0.0, 1e-12,
                    "exhausted second surface returns zero Q");
        expect_near(check, one_result.state.stokes[ch].U, 0.0, 1e-12,
                    "exhausted second surface returns zero U");
        expect_near(check, one_result.state.stokes[ch].V, 0.0, 1e-12,
                    "exhausted second surface returns zero V");
    }

    PolarizedScene zero_scene = scene;
    zero_scene.max_reflections = 0;
    PolarizedRenderer zero(zero_scene);
    PolarizedTraceResult zero_result = zero.trace_with_count(
        {0, 0, -1}, &top_e1);
    check(zero_result.hit, "budget-zero hit reports hit");
    for (int ch = 0; ch < 3; ++ch)
        expect_near(check, zero_result.state.stokes[ch].I, 0.0, 1e-12,
                    "zero budget returns black hit");

    // Missing geometry still returns background in its world basis.
    PolarizedScene empty;
    empty.background = background;
    PolarizedRenderer background_renderer(empty);
    vec3 miss_D = vec3{0.2f, 0.4f, -1.0f}.normalized();
    PolarizedTraceResult bg = background_renderer.trace_ray_with_count(
        vec3{0, 0, 0}, miss_D, nullptr);
    check(!bg.hit, "miss reports background");
    check(bg.reflection_events == 0, "miss has no surface events");
    Vec3 expected_k = -pol::normalize(Vec3(0.2, 0.4, -1.0));
    expect_near(check, bg.state.k.x, expected_k.x, 1e-6, "background k x");
    expect_near(check, bg.state.k.y, expected_k.y, 1e-6, "background k y");
    expect_near(check, bg.state.k.z, expected_k.z, 1e-6, "background k z");
    Vec3 expected_e1 = pol::world_reference_axis(expected_k);
    expect_near(check, bg.state.e1.x, expected_e1.x, 1e-6, "background e1 x");
    expect_near(check, bg.state.e1.y, expected_e1.y, 1e-6, "background e1 y");
    expect_near(check, bg.state.e1.z, expected_e1.z, 1e-6, "background e1 z");

    // Interleaved traces with different requested bases must not share state.
    Vec3 axis_a = pol::normalize(Vec3(0.2, 0.9, 0.1));
    Vec3 axis_b = pol::normalize(Vec3(-0.8, 0.1, 0.4));
    vec3 interleaved_D = vec3{0.1f, -0.2f, -1.0f}.normalized();
    auto a1 = renderer.trace({0, 0, -1}, &axis_a);
    (void)renderer.trace(interleaved_D, &axis_b);
    auto a2 = renderer.trace({0, 0, -1}, &axis_a);
    for (int ch = 0; ch < 3; ++ch) {
        check_stokes(check, a1.stokes[ch], a2.stokes[ch], 1e-13,
                     "interleaved trace basis independence");
    }

    // Changing camera roll must not leave a cached frame in the renderer.
    renderer.roll = 0.31;
    PolarizedTraceResult roll_a = renderer.trace_pixel(24, 31, 64, 48);
    renderer.roll = -0.77;
    (void)renderer.trace_pixel(24, 31, 64, 48);
    renderer.roll = 0.31;
    PolarizedTraceResult roll_b = renderer.trace_pixel(24, 31, 64, 48);
    for (int ch = 0; ch < 3; ++ch) {
        check_stokes(check, roll_a.state.stokes[ch], roll_b.state.stokes[ch],
                     1e-13, "camera roll does not leak a previous frame");
    }
}
