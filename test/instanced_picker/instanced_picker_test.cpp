// Test program for examples/instanced_picker.h.
//
// This is a plain, windowless executable: it builds shared meshes and
// instances, runs world-space picking queries, and prints expected/actual
// values for every assertion. It is compiled by check.sh with the system
// C++20 compiler, without downloading anything.

#include "instanced_picker.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace instanced_picker;

namespace {

int g_failures = 0;
int g_checks = 0;

constexpr Scalar kEps = 1e-10;

void report(bool ok, const std::string& name, const std::string& detail) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::cout << "FAIL " << name << " - " << detail << "\n";
    } else {
        std::cout << "PASS " << name << " " << detail << "\n";
    }
}

bool nearly_equal(Scalar a, Scalar b) {
    return std::abs(a - b) <= kEps * std::max<Scalar>(1, std::abs(a));
}

void check_scalar(const std::string& name, Scalar expected, Scalar actual) {
    report(nearly_equal(expected, actual), name,
           "expected=" + std::to_string(expected) +
           " actual=" + std::to_string(actual));
}

void check_u32(const std::string& name, uint32_t expected, uint32_t actual) {
    report(expected == actual, name,
           "expected=" + std::to_string(expected) +
           " actual=" + std::to_string(actual));
}

void check_vec3(const std::string& name, const Vec3& expected, const Vec3& actual) {
    bool ok = nearly_equal(expected[0], actual[0])
           && nearly_equal(expected[1], actual[1])
           && nearly_equal(expected[2], actual[2]);
    report(ok, name,
           "expected=(" + std::to_string(expected[0]) + ", "
                         + std::to_string(expected[1]) + ", "
                         + std::to_string(expected[2]) + ") actual=("
                         + std::to_string(actual[0]) + ", "
                         + std::to_string(actual[1]) + ", "
                         + std::to_string(actual[2]) + ")");
}

void check_no_hit(const std::string& name, const std::optional<Hit>& hit) {
    report(!hit.has_value(), name,
           hit ? ("unexpected hit at t=" + std::to_string(hit->t)
                  + " instance=" + std::to_string(hit->instance_id)
                  + " prim=" + std::to_string(hit->primitive_id))
               : "no hit as expected");
}

template <typename F>
void check_throws(const std::string& name, F&& f) {
    bool threw = false;
    try {
        f();
    } catch (const std::invalid_argument& e) {
        threw = true;
        std::cout << "  (caught invalid_argument: " << e.what() << ")\n";
    }
    report(threw, name, threw ? "invalid_argument thrown as expected"
                              : "invalid_argument was NOT thrown");
}

// ----- Matrix construction helpers (row-major 3x4) -------------------------

Mat3x4 make_translate(Scalar tx, Scalar ty, Scalar tz) {
    return {1, 0, 0, tx,
            0, 1, 0, ty,
            0, 0, 1, tz};
}

Mat3x4 make_scale(Scalar sx, Scalar sy, Scalar sz) {
    return {sx, 0,  0,  0,
            0,  sy, 0,  0,
            0,  0,  sz, 0};
}

// Shear mapping x into z: z' = s * x + z.
Mat3x4 make_z_shear(Scalar s) {
    return {1, 0, 0, 0,
            0, 1, 0, 0,
            s, 0, 1, 0};
}

// Row-major affine composition, returns B * A.
Mat3x4 multiply(const Mat3x4& B, const Mat3x4& A) {
    Mat3x4 C{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            Scalar v = (c < 3) ? 0 : B[r * 4 + 3];
            for (int k = 0; k < 3; ++k)
                v += B[r * 4 + k] * A[k * 4 + c];
            C[r * 4 + c] = v;
        }
    }
    return C;
}

// The shared local triangle used by the reference queries.
std::shared_ptr<Mesh> make_reference_mesh() {
    return std::make_shared<Mesh>(std::vector<Triangle>{
        Triangle{Vec3(0, 0, 0), Vec3(1, 0, 0), Vec3(0, 1, 0)}
    });
}

// Checks the full content of the canonical reference hit.
void check_reference_hit(const std::string& tag, const std::optional<Hit>& hit,
                         uint32_t instance_id, const Vec3& expected_position,
                         const Vec3& expected_normal, Scalar expected_t) {
    report(hit.has_value(), tag + ":hit", hit ? "hit present" : "expected a hit");
    if (!hit)
        return;
    check_u32(tag + ":instance_id", instance_id, hit->instance_id);
    check_u32(tag + ":primitive_id", 0, hit->primitive_id);
    check_scalar(tag + ":t", expected_t, hit->t);
    check_scalar(tag + ":w0", 0.5, hit->w0);
    check_scalar(tag + ":w1", 0.25, hit->w1);
    check_scalar(tag + ":w2", 0.25, hit->w2);
    check_scalar(tag + ":bary_sum", 1.0, hit->w0 + hit->w1 + hit->w2);
    check_vec3(tag + ":position", expected_position, hit->position);
    check_vec3(tag + ":geometric_normal", expected_normal, hit->geometric_normal);
    check_scalar(tag + ":normal_length", 1.0,
                 std::sqrt(hit->geometric_normal[0] * hit->geometric_normal[0]
                         + hit->geometric_normal[1] * hit->geometric_normal[1]
                         + hit->geometric_normal[2] * hit->geometric_normal[2]));
}

} // namespace

int main() {
    // --------------------------------------------------------------
    // 1. Canonical reference: scale(2,3,4) then translate z=6.
    //    O=(0.5,0.75,0), D=(0,0,2) -> t=3 at (0.5,0.75,6),
    //    barycentric weights (0.5,0.25,0.25), normal +Z.
    // --------------------------------------------------------------
    {
        auto mesh = make_reference_mesh();
        Mat3x4 m = multiply(make_translate(0, 0, 6), make_scale(2, 3, 4));
        Picker picker(std::vector<Instance>{Instance(1, mesh, m, 0x1)});

        auto hit = picker.pick(Vec3(0.5, 0.75, 0), Vec3(0, 0, 2), 0, 100, 0x1);
        check_reference_hit("canonical", hit, 1, Vec3(0.5, 0.75, 6),
                            Vec3(0, 0, 1), 3);

        // A non-unit, shorter direction must simply rescale t: same point at
        // t = 12 with D = (0,0,0.5).
        auto hit_slow = picker.pick(Vec3(0.5, 0.75, 0), Vec3(0, 0, 0.5), 0, 100, 0x1);
        check_reference_hit("canonical_short_dir", hit_slow, 1,
                            Vec3(0.5, 0.75, 6), Vec3(0, 0, 1), 12);
    }

    // --------------------------------------------------------------
    // 2. Negative x scale flips the winding (normal becomes -Z) but
    //    leaves the barycentric weights unchanged.
    // --------------------------------------------------------------
    {
        auto mesh = make_reference_mesh();
        Mat3x4 m = multiply(make_translate(0, 0, 6), make_scale(-2, 3, 4));
        Picker picker(std::vector<Instance>{Instance(2, mesh, m, 0x1)});

        auto hit = picker.pick(Vec3(-0.5, 0.75, 0), Vec3(0, 0, 2), 0, 100, 0x1);
        check_reference_hit("negative_scale", hit, 2, Vec3(-0.5, 0.75, 6),
                            Vec3(0, 0, -1), 3);
    }

    // --------------------------------------------------------------
    // 3. A second object at another depth: comparison is on t, and
    //    insertion order must not change the result.
    // --------------------------------------------------------------
    {
        auto mesh = make_reference_mesh();
        Mat3x4 near_m = multiply(make_translate(0, 0, 6),  make_scale(2, 3, 4)); // t = 3
        Mat3x4 far_m  = multiply(make_translate(0, 0, 10), make_scale(2, 3, 4)); // t = 5

        Picker near_first(std::vector<Instance>{
            Instance(10, mesh, near_m, 0x1),
            Instance(20, mesh, far_m,  0x1)});
        Picker far_first(std::vector<Instance>{
            Instance(20, mesh, far_m,  0x1),
            Instance(10, mesh, near_m, 0x1)});

        for (const auto& [tag, picker] : {
                std::pair<const char*, const Picker*>{"depth_near_first", &near_first},
                std::pair<const char*, const Picker*>{"depth_far_first",  &far_first}}) {
            auto hit = picker->pick(Vec3(0.5, 0.75, 0), Vec3(0, 0, 2), 0, 100, 0x1);
            check_reference_hit(tag, hit, 10, Vec3(0.5, 0.75, 6),
                                Vec3(0, 0, 1), 3);
        }
    }

    // --------------------------------------------------------------
    // 4. Shear: z' = 3x + z, then translate z=1.
    //    Ray O=(0.2,0.3,0), D=(0,0,1) hits at t = 3*0.2 + 1 = 1.6.
    // --------------------------------------------------------------
    {
        auto mesh = make_reference_mesh();
        Mat3x4 m = multiply(make_translate(0, 0, 1), make_z_shear(3));
        Picker picker(std::vector<Instance>{Instance(3, mesh, m, 0x1)});

        auto hit = picker.pick(Vec3(0.2, 0.3, 0), Vec3(0, 0, 1), 0, 100, 0x1);
        report(hit.has_value(), "shear:hit", hit ? "hit present" : "expected a hit");
        if (hit) {
            check_u32("shear:instance_id", 3, hit->instance_id);
            check_scalar("shear:t", 1.6, hit->t);
            check_scalar("shear:w0", 0.5, hit->w0);
            check_scalar("shear:w1", 0.2, hit->w1);
            check_scalar("shear:w2", 0.3, hit->w2);
            check_vec3("shear:position", Vec3(0.2, 0.3, 1.6), hit->position);
            // cross((1,0,3),(0,1,0)) = (-3,0,1), normalized.
            Scalar k = 1 / std::sqrt(10.0);
            check_vec3("shear:normal", Vec3(-3 * k, 0, k), hit->geometric_normal);
        }
    }

    // --------------------------------------------------------------
    // 5. Closed interval [tmin, tmax]: hits exactly at either
    //    endpoint count; anything strictly outside does not.
    // --------------------------------------------------------------
    {
        auto mesh = make_reference_mesh();
        Mat3x4 m = multiply(make_translate(0, 0, 6), make_scale(2, 3, 4));
        Picker picker(std::vector<Instance>{Instance(4, mesh, m, 0x1)});
        Vec3 o(0.5, 0.75, 0), d(0, 0, 2);

        auto at_endpoints = picker.pick(o, d, 3, 3, 0x1);
        check_reference_hit("interval_exact_endpoints", at_endpoints, 4,
                            Vec3(0.5, 0.75, 6), Vec3(0, 0, 1), 3);

        check_no_hit("interval_tmax_too_small", picker.pick(o, d, 0, 2.999999999, 0x1));
        check_no_hit("interval_tmin_too_large", picker.pick(o, d, 3.000000001, 100, 0x1));
        check_no_hit("interval_whole_range_before", picker.pick(o, d, 0, 1, 0x1));
    }

    // --------------------------------------------------------------
    // 6. Visibility masks: a bitwise AND of zero removes an instance.
    // --------------------------------------------------------------
    {
        auto mesh = make_reference_mesh();
        Mat3x4 near_m = multiply(make_translate(0, 0, 6),  make_scale(2, 3, 4)); // t = 3
        Mat3x4 far_m  = multiply(make_translate(0, 0, 10), make_scale(2, 3, 4)); // t = 5
        Picker picker(std::vector<Instance>{
            Instance(30, mesh, far_m,  0x1), // farther, mask bit 0
            Instance(40, mesh, near_m, 0x2)}); // nearer, mask bit 1
        Vec3 o(0.5, 0.75, 0), d(0, 0, 2);

        // Querying bit 0 skips the nearer object, so the farther one wins.
        auto bit0 = picker.pick(o, d, 0, 100, 0x1);
        check_reference_hit("mask_bit0_only", bit0, 30, Vec3(0.5, 0.75, 10),
                            Vec3(0, 0, 1), 5);

        auto bit1 = picker.pick(o, d, 0, 100, 0x2);
        check_reference_hit("mask_bit1_only", bit1, 40, Vec3(0.5, 0.75, 6),
                            Vec3(0, 0, 1), 3);

        auto both = picker.pick(o, d, 0, 100, 0x3);
        check_reference_hit("mask_both_bits", both, 40, Vec3(0.5, 0.75, 6),
                            Vec3(0, 0, 1), 3);

        check_no_hit("mask_zero_matches_nothing", picker.pick(o, d, 0, 100, 0));
    }

    // --------------------------------------------------------------
    // 7. A rejected near face is treated as transparent: the next
    //    acceptable surface behind it is still found. The tmax of the
    //    query is deliberately placed exactly at the farther hit, so
    //    any erroneous tightening from the rejected face would cull it.
    // --------------------------------------------------------------
    {
        // Two triangles in one mesh: primitive 0 at z=1, primitive 1 at z=2.
        std::vector<Triangle> tris;
        for (Scalar z : {1.0, 2.0})
            tris.push_back(Triangle{
                Vec3(-1, -1, z), Vec3(1, -1, z), Vec3(0, 1, z)});
        auto mesh = std::make_shared<Mesh>(tris);
        Mat3x4 identity = make_translate(0, 0, 0);
        Picker picker(std::vector<Instance>{Instance(5, mesh, identity, 0x1)});

        int calls = 0;
        auto reject_near = [&] (const Hit& h) {
            ++calls;
            check_scalar("reject_near:candidate_bary_sum", 1.0,
                         h.w0 + h.w1 + h.w2);
            return h.primitive_id != 0;
        };
        auto hit = picker.pick(Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 2, 0x1, reject_near);
        report(hit.has_value(), "reject_near:hit", hit ? "hit present" : "expected a hit");
        if (hit) {
            check_u32("reject_near:primitive_id", 1, hit->primitive_id);
            check_scalar("reject_near:t", 2, hit->t);
            check_vec3("reject_near:position", Vec3(0, 0, 2), hit->position);
            check_vec3("reject_near:normal", Vec3(0, 0, 1), hit->geometric_normal);
        }
        report(calls >= 1, "reject_near:callback_used",
               "callback invocations=" + std::to_string(calls));

        // Same idea across instances: rejecting the nearer instance reveals
        // the farther one, again with tmax pinned to the farther hit.
        auto ref = make_reference_mesh();
        Mat3x4 near_m = multiply(make_translate(0, 0, 6),  make_scale(2, 3, 4));
        Mat3x4 far_m  = multiply(make_translate(0, 0, 10), make_scale(2, 3, 4));
        Picker two(std::vector<Instance>{
            Instance(50, ref, near_m, 0x1),
            Instance(60, ref, far_m,  0x1)});
        auto cross = two.pick(Vec3(0.5, 0.75, 0), Vec3(0, 0, 2), 0, 5, 0x1,
            [] (const Hit& h) { return h.instance_id != 50; });
        check_reference_hit("reject_near_instance", cross, 60,
                            Vec3(0.5, 0.75, 10), Vec3(0, 0, 1), 5);
    }

    // --------------------------------------------------------------
    // 8. Rejecting every candidate yields no hit.
    // --------------------------------------------------------------
    {
        auto mesh = make_reference_mesh();
        Mat3x4 m = multiply(make_translate(0, 0, 6), make_scale(2, 3, 4));
        Picker picker(std::vector<Instance>{Instance(6, mesh, m, 0x1)});
        auto hit = picker.pick(Vec3(0.5, 0.75, 0), Vec3(0, 0, 2), 0, 100, 0x1,
                               [] (const Hit&) { return false; });
        check_no_hit("reject_all", hit);
    }

    // --------------------------------------------------------------
    // 9. The BVH reorders primitives internally; the user must still
    //    see the original input index.
    // --------------------------------------------------------------
    {
        constexpr size_t N = 32;
        std::vector<Triangle> tris;
        tris.reserve(N);
        // Input order i runs right-to-left in space, so an SAH builder has
        // to permute primitives to obtain a spatially ordered layout.
        for (size_t i = 0; i < N; ++i) {
            Scalar cx = static_cast<Scalar>(N - 1 - i) * 3;
            tris.push_back(Triangle{
                Vec3(cx - 1, -1, 0), Vec3(cx + 1, -1, 0), Vec3(cx, 1, 0)});
        }
        auto mesh = std::make_shared<Mesh>(tris);

        bool permuted = false;
        for (size_t i = 0; i < N; ++i)
            permuted |= mesh->test_prim_ids()[i] != i;
        report(permuted, "reorder:bvh_permuted_primitives",
               permuted ? "prim_ids is not the identity"
                        : "prim_ids unexpectedly stayed identity");

        Mat3x4 identity = make_translate(0, 0, 0);
        Picker picker(std::vector<Instance>{Instance(7, mesh, identity, 0x1)});

        constexpr uint32_t target = 7;
        Scalar cx = static_cast<Scalar>(N - 1 - target) * 3;
        auto hit = picker.pick(Vec3(cx, 0, -1), Vec3(0, 0, 1), 0, 100, 0x1);
        report(hit.has_value(), "reorder:hit", hit ? "hit present" : "expected a hit");
        if (hit) {
            check_u32("reorder:primitive_id_is_original", target, hit->primitive_id);
            check_scalar("reorder:t", 1, hit->t);
            check_vec3("reorder:position", Vec3(cx, 0, 0), hit->position);
            check_scalar("reorder:w0", 0.25, hit->w0);
            check_scalar("reorder:w1", 0.25, hit->w1);
            check_scalar("reorder:w2", 0.5, hit->w2);
        }
    }

    // --------------------------------------------------------------
    // 10. Exactly equal t: lexicographic minimum of
    //     (instance_id, primitive_id), independent of instance order.
    // --------------------------------------------------------------
    {
        auto mesh = make_reference_mesh();
        Mat3x4 m = multiply(make_translate(0, 0, 6), make_scale(2, 3, 4));
        Vec3 o(0.5, 0.75, 0), d(0, 0, 2);

        Picker high_first(std::vector<Instance>{
            Instance(5, mesh, m, 0x1), Instance(3, mesh, m, 0x1)});
        Picker low_first(std::vector<Instance>{
            Instance(3, mesh, m, 0x1), Instance(5, mesh, m, 0x1)});

        auto h1 = high_first.pick(o, d, 0, 100, 0x1);
        auto h2 = low_first.pick(o, d, 0, 100, 0x1);
        check_reference_hit("tie_instance_high_first", h1, 3,
                            Vec3(0.5, 0.75, 6), Vec3(0, 0, 1), 3);
        check_reference_hit("tie_instance_low_first", h2, 3,
                            Vec3(0.5, 0.75, 6), Vec3(0, 0, 1), 3);

        // Two coplanar triangles in one mesh both containing the ray point:
        // the lower primitive_id wins the exact tie.
        auto coplanar = std::make_shared<Mesh>(std::vector<Triangle>{
            Triangle{Vec3(-1, -1, 1), Vec3(1, -1, 1), Vec3(0, 1, 1)},
            Triangle{Vec3(1, -1, 1), Vec3(-1, -1, 1), Vec3(0, 1, 1)}});
        Picker one(std::vector<Instance>{
            Instance(8, coplanar, make_translate(0, 0, 0), 0x1)});
        auto h3 = one.pick(Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 100, 0x1);
        report(h3.has_value(), "tie_primitive:hit",
               h3 ? "hit present" : "expected a hit");
        if (h3) {
            check_scalar("tie_primitive:t", 1, h3->t);
            check_u32("tie_primitive:primitive_id", 0, h3->primitive_id);
        }
    }

    // --------------------------------------------------------------
    // 11. Single-mesh regression (mirrors test/simple_example.cpp).
    // --------------------------------------------------------------
    {
        auto mesh = std::make_shared<Mesh>(std::vector<Triangle>{
            Triangle{Vec3(1, -1, 1), Vec3(1, 1, 1), Vec3(-1, 1, 1)},
            Triangle{Vec3(1, -1, 1), Vec3(-1, -1, 1), Vec3(-1, 1, 1)}});
        Picker picker(std::vector<Instance>{
            Instance(9, mesh, make_translate(0, 0, 0), 0x1)});
        auto hit = picker.pick(Vec3(0.5, 0, 0), Vec3(0, 0, 1), 0, 100, 0x1);
        report(hit.has_value(), "single_mesh:hit",
               hit ? "hit present" : "expected a hit");
        if (hit) {
            check_u32("single_mesh:primitive_id", 0, hit->primitive_id);
            check_scalar("single_mesh:t", 1, hit->t);
            check_scalar("single_mesh:w0", 0.5, hit->w0);
            check_scalar("single_mesh:w1", 0.25, hit->w1);
            check_scalar("single_mesh:w2", 0.25, hit->w2);
            check_vec3("single_mesh:position", Vec3(0.5, 0, 1), hit->position);
            check_vec3("single_mesh:normal", Vec3(0, 0, 1), hit->geometric_normal);
        }
        check_no_hit("single_mesh:miss",
                     picker.pick(Vec3(50, 50, 0), Vec3(0, 0, 1), 0, 100, 0x1));
    }

    // --------------------------------------------------------------
    // 12. Validation errors throw invalid_argument.
    // --------------------------------------------------------------
    {
        auto mesh = make_reference_mesh();
        Mat3x4 good = multiply(make_translate(0, 0, 6), make_scale(2, 3, 4));

        check_throws("invalid_singular_transform", [&] {
            Picker p(std::vector<Instance>{
                Instance(1, mesh, multiply(make_scale(0, 1, 1), good), 0x1)});
        });
        Mat3x4 nan_matrix = good;
        nan_matrix[5] = std::numeric_limits<Scalar>::quiet_NaN();
        check_throws("invalid_non_finite_transform", [&] {
            Picker p(std::vector<Instance>{Instance(1, mesh, nan_matrix, 0x1)});
        });
        check_throws("invalid_duplicate_id", [&] {
            Picker p(std::vector<Instance>{
                Instance(1, mesh, good, 0x1), Instance(1, mesh, good, 0x1)});
        });

        Picker picker(std::vector<Instance>{Instance(1, mesh, good, 0x1)});
        Scalar nan = std::numeric_limits<Scalar>::quiet_NaN();
        check_throws("invalid_non_finite_ray_origin", [&] {
            picker.pick(Vec3(nan, 0, 0), Vec3(0, 0, 1), 0, 100, 0x1);
        });
        check_throws("invalid_non_finite_ray_direction", [&] {
            picker.pick(Vec3(0, 0, 0), Vec3(0, nan, 1), 0, 100, 0x1);
        });
        check_throws("invalid_zero_direction", [&] {
            picker.pick(Vec3(0, 0, 0), Vec3(0, 0, 0), 0, 100, 0x1);
        });
        check_throws("invalid_tmin_greater_than_tmax", [&] {
            picker.pick(Vec3(0.5, 0.75, 0), Vec3(0, 0, 2), 100, 0, 0x1);
        });
        check_throws("invalid_non_finite_interval", [&] {
            picker.pick(Vec3(0.5, 0.75, 0), Vec3(0, 0, 2), 0, nan, 0x1);
        });
    }

    // --------------------------------------------------------------
    // 13. Empty meshes and empty instance tables are legal.
    // --------------------------------------------------------------
    {
        Picker empty_picker(std::vector<Instance>{});
        check_no_hit("empty_instance_table",
                     empty_picker.pick(Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 100, 0x1));

        auto empty_mesh = std::make_shared<Mesh>(std::vector<Triangle>{});
        check_u32("empty_mesh:size", 0, static_cast<uint32_t>(empty_mesh->size()));
        Picker picker(std::vector<Instance>{
            Instance(1, empty_mesh, make_translate(0, 0, 0), 0x1)});
        check_no_hit("empty_mesh:no_hit",
                     picker.pick(Vec3(0, 0, 0), Vec3(0, 0, 1), 0, 100, 0x1));
    }

    // --------------------------------------------------------------
    // 14. Queries are read-only: mesh content is unchanged afterwards.
    // --------------------------------------------------------------
    {
        constexpr size_t N = 12;
        std::vector<Triangle> tris;
        for (size_t i = 0; i < N; ++i) {
            Scalar cx = static_cast<Scalar>(N - 1 - i) * 3;
            tris.push_back(Triangle{
                Vec3(cx - 1, -1, 0), Vec3(cx + 1, -1, 0), Vec3(cx, 1, 0)});
        }
        auto mesh = std::make_shared<Mesh>(tris);
        auto snapshot = mesh->triangles();

        Picker picker(std::vector<Instance>{
            Instance(1, mesh, make_translate(0, 0, 0), 0x1)});
        for (size_t i = 0; i < N; ++i) {
            Scalar cx = static_cast<Scalar>(N - 1 - i) * 3;
            picker.pick(Vec3(cx, 0, -1), Vec3(0, 0, 1), 0, 100, 0x1);
        }
        picker.pick(Vec3(0, 0, -1), Vec3(0, 0, 1), 0, 100, 0x1,
                    [] (const Hit&) { return false; });

        bool unchanged = snapshot.size() == mesh->triangles().size();
        for (size_t i = 0; unchanged && i < snapshot.size(); ++i) {
            const Triangle& a = snapshot[i];
            const Triangle& b = mesh->triangles()[i];
            const Vec3 va[3] = {a.p0, a.p1, a.p2};
            const Vec3 vb[3] = {b.p0, b.p1, b.p2};
            for (int j = 0; unchanged && j < 3; ++j)
                for (int k = 0; k < 3; ++k)
                    unchanged &= va[j][k] == vb[j][k];
        }
        report(unchanged, "immutability:mesh_unchanged",
               unchanged ? "all triangle vertices identical after queries"
                         : "mesh content was modified by a query");
    }

    std::cout << "\n" << (g_failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED")
              << ": " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
