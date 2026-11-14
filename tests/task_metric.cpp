// Task tests for tensor-driven anisotropic remeshing.
// Registered through task_support/check.h in three groups:
//   metric_direction, metric_transport, metric_compat.

#include "task_support/check.h"

#include <cmath>
#include <vector>

#include "pmp/algorithms/barycentric_coordinates.h"
#include "pmp/algorithms/distance_point_triangle.h"
#include "pmp/algorithms/remeshing.h"

using namespace pmp;

namespace {

using Metric = Matrix<Scalar, 3, 3>;

constexpr Scalar PI = Scalar(3.14159265358979323846);

// planar (n x n)-cell quadrangulated grid in the xy plane, z = 0,
// consistently oriented with normal +z
SurfaceMesh make_grid(int n, Scalar h)
{
    SurfaceMesh m;
    std::vector<std::vector<Vertex>> v(n + 1);
    for (int j = 0; j <= n; ++j)
    {
        v[j].resize(n + 1);
        for (int i = 0; i <= n; ++i)
            v[j][i] = m.add_vertex(Point(i * h, j * h, Scalar(0.0)));
    }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            m.add_triangle(v[j][i], v[j][i + 1], v[j + 1][i + 1]);
            m.add_triangle(v[j][i], v[j + 1][i + 1], v[j + 1][i]);
        }
    return m;
}

void set_uniform_field(SurfaceMesh& m, const Metric& metric)
{
    auto mp = m.vertex_property<Metric>("v:metric");
    for (auto v : m.vertices())
        mp[v] = metric;
}

Metric diag_metric(Scalar a, Scalar b, Scalar c)
{
    Metric m(Scalar(0.0));
    m(0, 0) = a;
    m(1, 1) = b;
    m(2, 2) = c;
    return m;
}

// rotation about the z axis
Metric rotation_z(Scalar angle_deg)
{
    const Scalar a = angle_deg * PI / Scalar(180.0);
    const Scalar c = std::cos(a), s = std::sin(a);
    Metric r(Scalar(0.0));
    r(0, 0) = c;
    r(0, 1) = -s;
    r(1, 0) = s;
    r(1, 1) = c;
    r(2, 2) = Scalar(1.0);
    return r;
}

Metric rotate_tensor(const Metric& r, const Metric& m)
{
    return r * m * transpose(r);
}

void rotate_geometry(SurfaceMesh& m, const Metric& r)
{
    auto p = m.vertex_property<Point>("v:point");
    for (auto v : m.vertices())
        p[v] = r * p[v];
}

// RMS edge projection length along axis u
Scalar rms_edge_component(const SurfaceMesh& m, const Point& u)
{
    Scalar s(0.0);
    size_t count = 0;
    auto p = m.get_vertex_property<Point>("v:point");
    for (auto e : m.edges())
    {
        const Point d = p[m.vertex(e, 1)] - p[m.vertex(e, 0)];
        s += dot(d, u) * dot(d, u);
        ++count;
    }
    return std::sqrt(s / Scalar(count));
}

bool is_finite(const Metric& m)
{
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            if (!std::isfinite(m(i, j)))
                return false;
    return true;
}

bool is_symmetric(const Metric& m, Scalar tol = Scalar(1e-9))
{
    for (int i = 0; i < 3; ++i)
        for (int j = i + 1; j < 3; ++j)
            if (std::fabs(m(i, j) - m(j, i)) >
                tol * (Scalar(1.0) + std::fabs(m(i, j)) +
                       std::fabs(m(j, i))))
                return false;
    return true;
}

bool is_spd(const Metric& m)
{
    Scalar e1, e2, e3;
    Point q1, q2, q3;
    if (!symmetric_eigendecomposition(m, e1, e2, e3, q1, q2, q3))
        return false;
    const Scalar scale =
        std::max(Scalar(1.0), std::max(std::fabs(e1),
                                       std::max(std::fabs(e2),
                                                std::fabs(e3))));
    return e1 > Scalar(1e-10) * scale && e2 > Scalar(1e-10) * scale &&
           e3 > Scalar(1e-10) * scale;
}

// every face is a non-degenerate triangle oriented towards +z,
// every vertex stays manifold, and every metric stays finite SPD
bool mesh_health_ok(const SurfaceMesh& m)
{
    if (!m.is_triangle_mesh())
        return false;

    auto p = m.get_vertex_property<Point>("v:point");
    for (auto f : m.faces())
    {
        auto vv = m.vertices(f);
        const Point a = p[*vv];
        ++vv;
        const Point b = p[*vv];
        ++vv;
        const Point c = p[*vv];
        const Point n = cross(b - a, c - a);
        if (norm(n) == Scalar(0.0) || n[2] <= Scalar(0.0))
            return false;
    }
    for (auto v : m.vertices())
        if (!m.is_manifold(v))
            return false;

    auto mp = m.get_vertex_property<Metric>("v:metric");
    if (!mp)
        return false;
    for (auto v : m.vertices())
        if (!is_finite(mp[v]) || !is_symmetric(mp[v]) || !is_spd(mp[v]))
            return false;

    return true;
}

// independently re-implement transport from a frozen reference:
// nearest reference triangle (smallest index breaks equidistant ties),
// barycentric interpolation of the frozen tensor field
Metric reference_metric_at(const SurfaceMesh& ref, const Point& x,
                           Point& nearest_point)
{
    auto rp = ref.get_vertex_property<Point>("v:point");
    auto rm = ref.get_vertex_property<Metric>("v:metric");

    Scalar best = std::numeric_limits<Scalar>::infinity();
    Face best_face;
    Point best_point;
    for (auto f : ref.faces())
    {
        auto vv = ref.vertices(f);
        const Point& p0 = rp[*vv];
        ++vv;
        const Point& p1 = rp[*vv];
        ++vv;
        const Point& p2 = rp[*vv];
        Point q;
        const Scalar d = dist_point_triangle(x, p0, p1, p2, q);
        const Scalar tol =
            Scalar(1e-12) * (Scalar(1.0) + std::fabs(best));
        if (d < best - tol ||
            (std::fabs(d - best) <= tol &&
             (!best_face.is_valid() || f.idx() < best_face.idx())))
        {
            best = d;
            best_face = f;
            best_point = q;
        }
    }

    auto vv = ref.vertices(best_face);
    const Point& p0 = rp[*vv];
    const Metric& m0 = rm[*vv];
    ++vv;
    const Point& p1 = rp[*vv];
    const Metric& m1 = rm[*vv];
    ++vv;
    const Point& p2 = rp[*vv];
    const Metric& m2 = rm[*vv];

    Point b = barycentric_coordinates(best_point, p0, p1, p2);
    for (int k = 0; k < 3; ++k)
        b[k] = std::max(Scalar(0.0), b[k]);
    const Scalar s = b[0] + b[1] + b[2];
    if (s > Scalar(0.0))
        b *= Scalar(1.0) / s;

    nearest_point = best_point;
    return m0 * b[0] + m1 * b[1] + m2 * b[2];
}

Scalar frobenius_distance(const Metric& a, const Metric& b)
{
    return norm(a - b);
}

// ---------------------------------------------------------------------------
// metric_direction
// ---------------------------------------------------------------------------

// dense along x, sparse along y must produce an anisotropic edge spacing
int direction_axes()
{
    const int n = 4;
    const Scalar h = Scalar(0.5);
    auto m = make_grid(n, h);
    // eigenvalue 100 along x (dense, metric length scale 0.1) and 4 along
    // y (sparse, length scale 0.5); the loose axis starts at target spacing
    set_uniform_field(m, diag_metric(100.0, 4.0, 1.0));

    anisotropic_remeshing(m, 3);

    TASK_CHECK(mesh_health_ok(m));

    const Point ex(1, 0, 0), ey(0, 1, 0);
    const Scalar sx = rms_edge_component(m, ex);
    const Scalar sy = rms_edge_component(m, ey);

    // edges must be clearly shorter along the dense (x) direction
    TASK_CHECK(sx > Scalar(0.0));
    TASK_CHECK(sy / sx > Scalar(1.5));
    TASK_CHECK(m.n_vertices() > size_t(10));

    return 0;
}

// rotating geometry together with an off-diagonal rotated tensor must
// rotate the sampling direction accordingly
int direction_rotated()
{
    const int n = 4;
    const Scalar h = Scalar(0.5);
    const Metric r = rotation_z(45.0);

    auto m = make_grid(n, h);
    rotate_geometry(m, r);
    const Metric field =
        rotate_tensor(r, diag_metric(100.0, 4.0, 1.0));

    // the rotated tensor is genuinely off-diagonal and symmetric
    TASK_CHECK(std::fabs(field(0, 1)) > Scalar(1.0));
    TASK_CHECK(is_symmetric(field));
    TASK_CHECK(is_spd(field));

    set_uniform_field(m, field);
    anisotropic_remeshing(m, 3);

    TASK_CHECK(mesh_health_ok(m));

    // dense/sparse axes in world coordinates
    const Point dense_axis = r * Point(1, 0, 0);
    const Point sparse_axis = r * Point(0, 1, 0);
    const Scalar sd = rms_edge_component(m, dense_axis);
    const Scalar ss = rms_edge_component(m, sparse_axis);

    TASK_CHECK(sd > Scalar(0.0));
    TASK_CHECK(ss / sd > Scalar(2.0));

    // the elongation direction really is the rotated y axis, not world y
    TASK_CHECK(std::fabs(sparse_axis[0]) > Scalar(0.5));

    return 0;
}

// isotropically dense fields refine, isotropically loose fields coarsen
int direction_density_levels()
{
    auto dense = make_grid(4, Scalar(0.5));
    set_uniform_field(dense, diag_metric(36.0, 36.0, 1.0));
    const size_t dense_before = dense.n_vertices();
    anisotropic_remeshing(dense, 3);
    TASK_CHECK(dense.n_vertices() > dense_before);
    TASK_CHECK(mesh_health_ok(dense));

    auto loose = make_grid(4, Scalar(0.5));
    set_uniform_field(loose, diag_metric(1.5, 1.5, 1.0));
    const size_t loose_before = loose.n_vertices();
    anisotropic_remeshing(loose, 3);
    TASK_CHECK(loose.n_vertices() < loose_before);
    TASK_CHECK(mesh_health_ok(loose));

    return 0;
}

// ---------------------------------------------------------------------------
// metric_transport
// ---------------------------------------------------------------------------

// a varying field must be transported from the frozen reference by
// barycentric interpolation, including for newly created vertices
int transport_varying_field()
{
    const int n = 4;
    const Scalar h = Scalar(0.5);
    auto m = make_grid(n, h);
    auto mp = m.vertex_property<Metric>("v:metric");
    auto pp = m.get_vertex_property<Point>("v:point");

    const Metric left = diag_metric(100.0, 4.0, 1.0);
    const Metric right = diag_metric(4.0, 100.0, 1.0);
    for (auto v : m.vertices())
        mp[v] = (pp[v][0] < Scalar(1.0001)) ? left : right;

    // freeze the input surface and field, and mark the original vertices.
    // default false so that vertices created by remeshing stay unmarked.
    SurfaceMesh ref(m);
    auto original = m.add_vertex_property<bool>("v:test-original", false);
    for (auto v : m.vertices())
        original[v] = true;

    anisotropic_remeshing(m, 3);
    TASK_CHECK(mesh_health_ok(m));

    auto p = m.get_vertex_property<Point>("v:point");
    auto metric = m.get_vertex_property<Metric>("v:metric");
    TASK_CHECK((bool)metric);

    size_t checked = 0, new_checked = 0;
    bool saw_left = false, saw_right = false;
    for (auto v : m.vertices())
    {
        const bool is_new = !original[v];

        // boundary and feature vertices are not re-projected; everything
        // else must match the frozen reference field exactly
        if (m.is_boundary(v))
            continue;

        Point q;
        const Metric expected = reference_metric_at(ref, p[v], q);
        TASK_CHECK(frobenius_distance(metric[v], expected) < Scalar(1e-8));
        TASK_CHECK(distance(p[v], q) < Scalar(1e-8));
        ++checked;
        if (is_new)
            ++new_checked;

        if (metric[v](0, 0) > metric[v](1, 1))
            saw_left = true;
        if (metric[v](1, 1) > metric[v](0, 0))
            saw_right = true;
    }

    TASK_CHECK(checked > 0);
    TASK_CHECK(new_checked > 0); // splits actually happened
    TASK_CHECK(saw_left && saw_right); // the field variation survived

    return 0;
}

// several iterations must keep the mesh manifold, oriented and valid
int transport_multiround()
{
    auto m = make_grid(4, Scalar(0.5));
    const Metric r = rotation_z(30.0);
    set_uniform_field(m, rotate_tensor(r, diag_metric(100.0, 4.0, 1.0)));

    const size_t faces_before = m.n_faces();
    anisotropic_remeshing(m, 5);

    TASK_CHECK(mesh_health_ok(m));
    TASK_CHECK(m.n_faces() != faces_before); // topology was edited

    // metric edge lengths remain finite and within a sane envelope
    auto p = m.get_vertex_property<Point>("v:point");
    auto mp = m.get_vertex_property<Metric>("v:metric");
    Scalar max_len(0.0), sum_len(0.0);
    size_t cnt = 0;
    for (auto e : m.edges())
    {
        const Vertex v0 = m.vertex(e, 0), v1 = m.vertex(e, 1);
        const Point d = p[v1] - p[v0];
        const Metric avg = Scalar(0.5) * (mp[v0] + mp[v1]);
        const Scalar len = std::sqrt(std::max(Scalar(0.0), dot(d, avg * d)));
        TASK_CHECK(std::isfinite(len));
        max_len = std::max(max_len, len);
        sum_len += len;
        ++cnt;
    }
    TASK_CHECK(max_len < Scalar(2.5));
    const Scalar mean_len = sum_len / Scalar(cnt);
    TASK_CHECK(mean_len > Scalar(0.3) && mean_len < Scalar(1.8));

    return 0;
}

// zero iterations validate but must not touch the mesh at all
int transport_zero_iterations()
{
    auto m = make_grid(4, Scalar(0.2));
    set_uniform_field(m, diag_metric(4.0, 9.0, 1.0));

    auto p_before = m.get_vertex_property<Point>("v:point");
    auto m_before = m.get_vertex_property<Metric>("v:metric");
    std::vector<Point> positions;
    std::vector<Metric> metrics;
    for (auto v : m.vertices())
    {
        positions.push_back(p_before[v]);
        metrics.push_back(m_before[v]);
    }
    const size_t nv = m.n_vertices(), nf = m.n_faces(), ne = m.n_edges();

    anisotropic_remeshing(m, 0);

    TASK_CHECK(m.n_vertices() == nv);
    TASK_CHECK(m.n_faces() == nf);
    TASK_CHECK(m.n_edges() == ne);
    auto p_after = m.get_vertex_property<Point>("v:point");
    auto m_after = m.get_vertex_property<Metric>("v:metric");
    size_t i = 0;
    for (auto v : m.vertices())
    {
        TASK_CHECK(p_after[v] == positions[i]);
        TASK_CHECK(frobenius_distance(m_after[v], metrics[i]) == Scalar(0.0));
        ++i;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// metric_compat
// ---------------------------------------------------------------------------

bool expect_invalid(SurfaceMesh& m)
{
    try
    {
        anisotropic_remeshing(m, 2);
    }
    catch (const InvalidInputException&)
    {
        return true;
    }
    catch (...)
    {
        return false;
    }
    return false;
}

int compat_invalid_inputs()
{
    // missing property
    {
        auto m = make_grid(3, Scalar(0.2));
        TASK_CHECK(expect_invalid(m));
        TASK_CHECK(m.n_vertices() == size_t(16));
        TASK_CHECK(m.n_faces() == size_t(18));
    }

    auto make_field_mesh = [&]() {
        auto m = make_grid(3, Scalar(0.2));
        set_uniform_field(m, diag_metric(2.0, 3.0, 1.0));
        return m;
    };

    // non-finite entry
    {
        auto m = make_field_mesh();
        auto mp = m.get_vertex_property<Metric>("v:metric");
        mp[Vertex(0)](0, 0) =
            std::numeric_limits<Scalar>::quiet_NaN();
        const size_t nv = m.n_vertices();
        TASK_CHECK(expect_invalid(m));
        TASK_CHECK(m.n_vertices() == nv);
    }

    // asymmetric matrix
    {
        auto m = make_field_mesh();
        auto mp = m.get_vertex_property<Metric>("v:metric");
        mp[Vertex(1)](0, 1) = Scalar(1.0);
        mp[Vertex(1)](1, 0) = Scalar(2.0);
        TASK_CHECK(expect_invalid(m));
        TASK_CHECK(m.n_vertices() == size_t(16));
    }

    // indefinite and semidefinite matrices
    {
        auto m = make_field_mesh();
        auto mp = m.get_vertex_property<Metric>("v:metric");
        mp[Vertex(2)] = diag_metric(1.0, -1.0, 1.0);
        TASK_CHECK(expect_invalid(m));
    }
    {
        auto m = make_field_mesh();
        auto mp = m.get_vertex_property<Metric>("v:metric");
        mp[Vertex(2)] = diag_metric(0.0, 1.0, 1.0);
        TASK_CHECK(expect_invalid(m));
    }

    // non-finite coordinate
    {
        auto m = make_field_mesh();
        auto p = m.get_vertex_property<Point>("v:point");
        p[Vertex(3)][1] = std::numeric_limits<Scalar>::infinity();
        TASK_CHECK(expect_invalid(m));
        TASK_CHECK(m.n_faces() == size_t(18));
    }

    // degenerate face
    {
        auto m = make_field_mesh();
        auto p = m.get_vertex_property<Point>("v:point");
        p[Vertex(1)] = p[Vertex(0)];
        TASK_CHECK(expect_invalid(m));
        TASK_CHECK(m.n_vertices() == size_t(16));
    }

    return 0;
}

// the legacy scalar entry points keep their behavior and ignore v:metric
int compat_legacy_entries()
{
    auto with_a = make_grid(5, Scalar(0.2));
    set_uniform_field(with_a, diag_metric(100.0, 1.0, 1.0));
    auto with_b = make_grid(5, Scalar(0.2));
    set_uniform_field(with_b, diag_metric(1.0, 100.0, 1.0));
    auto without = make_grid(5, Scalar(0.2));

    uniform_remeshing(with_a, Scalar(0.15), 3, true);
    uniform_remeshing(with_b, Scalar(0.15), 3, true);
    uniform_remeshing(without, Scalar(0.15), 3, true);

    TASK_CHECK(with_a.n_vertices() == without.n_vertices());
    TASK_CHECK(with_a.n_faces() == without.n_faces());
    TASK_CHECK(with_b.n_vertices() == without.n_vertices());

    // tensor swap must have zero effect on the scalar path
    Scalar delta(0.0);
    auto pa = with_a.get_vertex_property<Point>("v:point");
    auto pb = with_b.get_vertex_property<Point>("v:point");
    for (auto v : with_a.vertices())
        delta += norm(pa[v] - pb[v]);
    TASK_CHECK(delta == Scalar(0.0));

    // the scalar path leaves the custom property in place
    TASK_CHECK(with_a.has_vertex_property("v:metric"));

    // adaptive entry point likewise ignores the tensor
    auto ad_a = make_grid(5, Scalar(0.2));
    set_uniform_field(ad_a, diag_metric(100.0, 1.0, 1.0));
    auto ad_none = make_grid(5, Scalar(0.2));
    adaptive_remeshing(ad_a, Scalar(0.05), Scalar(0.5), Scalar(0.01), 2);
    adaptive_remeshing(ad_none, Scalar(0.05), Scalar(0.5), Scalar(0.01), 2);
    TASK_CHECK(ad_a.n_vertices() == ad_none.n_vertices());
    TASK_CHECK(ad_a.n_faces() == ad_none.n_faces());

    return 0;
}

// selection locks, feature corners and the boundary polyline are respected
int compat_constraints()
{
    const int n = 5;
    const Scalar h = Scalar(0.2);

    // selection: vertices with x >= 0.5 are locked
    {
        auto m = make_grid(n, h);
        set_uniform_field(m, diag_metric(36.0, 36.0, 1.0));
        auto sel = m.add_vertex_property<bool>("v:selected");
        auto p = m.get_vertex_property<Point>("v:point");
        std::vector<std::pair<Vertex, Point>> locked;
        for (auto v : m.vertices())
        {
            if (p[v][0] >= Scalar(0.5) - Scalar(1e-12))
            {
                locked.emplace_back(v, p[v]);
            }
            else
            {
                sel[v] = true;
            }
        }
        TASK_CHECK(!locked.empty());
        anisotropic_remeshing(m, 3);
        TASK_CHECK(mesh_health_ok(m));
        auto p2 = m.get_vertex_property<Point>("v:point");
        for (const auto& [v, pos] : locked)
            TASK_CHECK(p2[v] == pos);
    }

    // feature corner immobility, feature edge inheritance, boundary shape
    {
        auto m = make_grid(n, h);
        set_uniform_field(m, diag_metric(36.0, 36.0, 1.0));
        auto vfeature = m.vertex_property<bool>("v:feature", false);
        auto efeature = m.edge_property<bool>("e:feature", false);

        auto p = m.get_vertex_property<Point>("v:point");
        Vertex va, vb;
        Edge feature_edge;
        bool found = false;
        for (auto e : m.edges())
        {
            const Vertex u = m.vertex(e, 0), w = m.vertex(e, 1);
            if (std::fabs(p[u][1] - Scalar(0.4)) < Scalar(1e-12) &&
                std::fabs(p[w][1] - p[u][1]) < Scalar(1e-12) &&
                std::fabs(p[u][0] - Scalar(0.4)) < Scalar(1e-12))
            {
                va = u;
                vb = w;
                feature_edge = e;
                found = true;
                break;
            }
        }
        TASK_CHECK(found);
        efeature[feature_edge] = true;
        vfeature[va] = true; // one incident feature edge -> corner -> locked
        vfeature[vb] = true;
        const Point pa = p[va], pb = p[vb];

        anisotropic_remeshing(m, 3);
        TASK_CHECK(mesh_health_ok(m));

        auto p2 = m.get_vertex_property<Point>("v:point");
        TASK_CHECK(p2[va] == pa);
        TASK_CHECK(p2[vb] == pb);

        // splitting along the feature polyline preserves/inherits it
        size_t feature_edges = 0;
        auto ef2 = m.get_edge_property<bool>("e:feature");
        TASK_CHECK((bool)ef2);
        for (auto e : m.edges())
            if (ef2[e])
                ++feature_edges;
        TASK_CHECK(feature_edges >= 1);

        // every vertex stays on the plane, every boundary vertex on the
        // original rectangle border
        for (auto v : m.vertices())
        {
            TASK_CHECK(std::fabs(p2[v][2]) < Scalar(1e-12));
            if (m.is_boundary(v))
            {
                const bool on_border =
                    std::fabs(p2[v][0]) < Scalar(1e-10) ||
                    std::fabs(p2[v][0] - Scalar(n * h)) < Scalar(1e-10) ||
                    std::fabs(p2[v][1]) < Scalar(1e-10) ||
                    std::fabs(p2[v][1] - Scalar(n * h)) < Scalar(1e-10);
                TASK_CHECK(on_border);
            }
        }
    }

    return 0;
}

} // namespace

// external linkage so task_support/driver.c can discover the test cases
const task_case task_cases[] = {
    {"metric_direction", "axes", direction_axes},
    {"metric_direction", "rotated_offdiagonal", direction_rotated},
    {"metric_direction", "density_levels", direction_density_levels},
    {"metric_transport", "varying_reference_field",
     transport_varying_field},
    {"metric_transport", "multiround_editing", transport_multiround},
    {"metric_transport", "zero_iteration", transport_zero_iterations},
    {"metric_compat", "invalid_inputs", compat_invalid_inputs},
    {"metric_compat", "legacy_entries", compat_legacy_entries},
    {"metric_compat", "constraints", compat_constraints},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
