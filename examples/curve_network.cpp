// Bidirectional curve network surface example.
//
// Builds a 4x3 non-planar network of clamped non-rational B-spline curves,
// constructs the Gordon surface with nanospline::CurveNetwork and writes a
// triangulated/quad OBJ (v / vn / f records) sampled from the native
// BSplinePatch.
//
// Data layout used here (see README "Curve network surfaces"):
//   * transverse curves C_j carry the u direction and are associated with the
//     common v stations V_j; a[j][i] is the parameter on C_j of its
//     intersection with longitudinal curve D_i;
//   * longitudinal curves D_i carry the v direction and are associated with
//     the common u stations U_i; b[i][j] is the matching parameter on D_i;
//   * each source curve is reparameterized onto the common stations by a
//     fixed-rule C1 monotone piecewise cubic map, and the composed curves are
//     native dynamic-degree B-splines (no evaluation closures);
//   * the returned BSplinePatch owns a tensor-product control grid in
//     v-major order: control point (i, j) is stored at linear index
//     i * num_control_points_v + j.

#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <Eigen/Core>

#include <nanospline/CurveNetwork.h>

using nanospline::BSpline;
using nanospline::CurveNetwork;
using nanospline::PiecewiseCubicReparam;

using Curve = BSpline<double, 3, -1>;
using Vec = Eigen::Matrix<double, Eigen::Dynamic, 1>;
using Points = Eigen::Matrix<double, Eigen::Dynamic, 3>;
using Row3 = Eigen::RowVector3d;

namespace {

constexpr int NUM_TRANSVERSE = 4; // C_j, one per v station
constexpr int NUM_LONGITUDINAL = 3; // D_i, one per u station
constexpr int SAMPLES_U = 48;
constexpr int SAMPLES_V = 36;

Curve make_clamped_curve(
    const Points& cp, double a, double b, const std::vector<double>& internal, int degree)
{
    Vec knots(degree + 1 + static_cast<int>(internal.size()) + 1 + degree);
    for (int i = 0; i <= degree; ++i) knots[i] = a;
    for (size_t k = 0; k < internal.size(); ++k) knots[degree + 1 + k] = internal[k];
    for (int i = 0; i <= degree; ++i) knots[knots.size() - 1 - i] = b;
    Curve c;
    c.set_control_points(cp);
    c.set_knots(knots);
    return c;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " output.obj\n";
        return 1;
    }
    const std::string obj_path = argv[1];

    // Common stations, strictly increasing from 0 to 1 with spacing >= 0.05.
    Vec V(NUM_TRANSVERSE), U(NUM_LONGITUDINAL);
    V << 0.0, 0.31, 0.64, 1.0;
    U << 0.0, 0.47, 1.0;

    // ---------------------------------------------------------------
    // Transverse family C_j: non-planar cubics, each with one internal
    // knot, on its own non-unit parameter domain.
    // ---------------------------------------------------------------
    std::vector<Curve> transverse(NUM_TRANSVERSE);
    std::vector<std::vector<double>> a(
        NUM_TRANSVERSE, std::vector<double>(NUM_LONGITUDINAL));
    for (int j = 0; j < NUM_TRANSVERSE; ++j) {
        Points cp(5, 3);
        for (int q = 0; q < 5; ++q) {
            cp(q, 0) = 1.6 * q;
            cp(q, 1) = 2.4 * V[j] + 0.25 * std::sin(1.7 * q + j);
            cp(q, 2) = 0.9 * std::cos(0.9 * q + 0.6 * j) + 0.6 * V[j] * V[j];
        }
        const double dom_a = -1.0 + 0.3 * j;
        const double dom_b = 2.5 + 0.25 * j;
        const double knot = dom_a + 0.4 * (dom_b - dom_a);
        transverse[j] = make_clamped_curve(cp, dom_a, dom_b, {knot}, 3);

        // Nonlinear intersection parameters; endpoints are the domain ends.
        for (int i = 0; i < NUM_LONGITUDINAL; ++i) {
            const double s = U[i];
            a[j][i] = dom_a + (dom_b - dom_a) * (0.25 * s + 0.75 * s * s);
        }
        a[j].front() = dom_a;
        a[j].back() = dom_b;
    }

    // ---------------------------------------------------------------
    // Longitudinal family D_i: natural cubic curves through the declared
    // intersection points, each on a nonlinear non-unit parameter axis.
    // ---------------------------------------------------------------
    std::vector<Curve> longitudinal(NUM_LONGITUDINAL);
    std::vector<std::vector<double>> b(
        NUM_LONGITUDINAL, std::vector<double>(NUM_TRANSVERSE));
    for (int i = 0; i < NUM_LONGITUDINAL; ++i) {
        Points grid(NUM_TRANSVERSE, 3);
        Vec stations(NUM_TRANSVERSE);
        for (int j = 0; j < NUM_TRANSVERSE; ++j) {
            grid.row(j) = transverse[j].evaluate(a[j][i]);
            const double w = V[j];
            stations[j] = -0.5 + 2.0 * (0.3 * w + 0.7 * w * w);
        }
        Vec knots;
        int degree;
        const Points ctrl =
            nanospline::internal::natural_cubic_interpolation(stations, grid, knots, degree);
        Curve c;
        c.set_control_points(ctrl);
        c.set_knots(knots);
        longitudinal[i] = c;
        for (int j = 0; j < NUM_TRANSVERSE; ++j) b[i][j] = stations[j];
    }

    // ---------------------------------------------------------------
    // Gordon surface S = LU + LV - LUV as one native tensor product patch.
    // ---------------------------------------------------------------
    const CurveNetwork network(transverse, longitudinal, V, U, a, b);
    const auto& patch = network.get_patch();
    std::cout << "Built patch: degree " << patch.get_degree_u() << " x "
              << patch.get_degree_v() << ", control grid "
              << patch.num_control_points_u() << " x "
              << patch.num_control_points_v() << "\n";

    // ---------------------------------------------------------------
    // Sample the native patch and write v / vn / f records.
    // ---------------------------------------------------------------
    std::ofstream out(obj_path);
    if (!out) {
        std::cerr << "Cannot open " << obj_path << " for writing\n";
        return 1;
    }
    out.precision(10);

    std::vector<Row3> positions;
    std::vector<Row3> normals;
    positions.reserve(SAMPLES_U * SAMPLES_V);
    normals.reserve(SAMPLES_U * SAMPLES_V);

    const double u_min = patch.get_u_lower_bound();
    const double u_max = patch.get_u_upper_bound();
    const double v_min = patch.get_v_lower_bound();
    const double v_max = patch.get_v_upper_bound();

    for (int iu = 0; iu < SAMPLES_U; ++iu) {
        const double u = u_min + (u_max - u_min) * iu / (SAMPLES_U - 1);
        for (int iv = 0; iv < SAMPLES_V; ++iv) {
            const double v = v_min + (v_max - v_min) * iv / (SAMPLES_V - 1);
            const Row3 p = patch.evaluate(u, v);
            Row3 normal =
                patch.evaluate_derivative_u(u, v).cross(patch.evaluate_derivative_v(u, v));
            const double length = normal.norm();
            if (length > 0.0) normal /= length;
            positions.push_back(p);
            normals.push_back(normal);
        }
    }

    for (const Row3& p : positions) {
        out << "v " << p[0] << " " << p[1] << " " << p[2] << "\n";
    }
    for (const Row3& n : normals) {
        out << "vn " << n[0] << " " << n[1] << " " << n[2] << "\n";
    }
    for (int iu = 0; iu + 1 < SAMPLES_U; ++iu) {
        for (int iv = 0; iv + 1 < SAMPLES_V; ++iv) {
            const auto index = [](int i, int j) {
                return i * SAMPLES_V + j + 1; // OBJ is 1-based.
            };
            const int i00 = index(iu, iv);
            const int i10 = index(iu + 1, iv);
            const int i11 = index(iu + 1, iv + 1);
            const int i01 = index(iu, iv + 1);
            out << "f " << i00 << "//" << i00 << " " << i10 << "//" << i10 << " " << i11
                << "//" << i11 << " " << i01 << "//" << i01 << "\n";
        }
    }
    out.close();
    std::cout << "Wrote " << positions.size() << " vertices and "
              << (SAMPLES_U - 1) * (SAMPLES_V - 1) << " faces to " << obj_path << "\n";
    return 0;
}
