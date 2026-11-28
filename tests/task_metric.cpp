// Task tests for tensor-driven anisotropic remeshing.
// Registered through task_support/check.h.

#include "task_support/check.h"

#include <cmath>
#include <limits>
#include <vector>

#include "pmp/algorithms/remeshing.h"
#include "pmp/bounding_box.h"

using namespace pmp;

namespace {

using Mat3 = Matrix<Scalar, 3, 3>;

constexpr Scalar pi = Scalar(3.14159265358979323846);

bool nearly_equal(Scalar a, Scalar b, Scalar tol = Scalar(1e-9))
{
    return std::fabs(a - b) <= tol * (Scalar(1) + std::fabs(a) + std::fabs(b));
}

Mat3 rotation_z(Scalar angle)
{
    const Scalar c = std::cos(angle);
    const Scalar s = std::sin(angle);
    // columns are the rotated basis vectors
    return Mat3(Point(c, s, 0), Point(-s, c, 0), Point(0, 0, 1));
}

// Rotate a tensor expressed in the local frame into world coordinates.
Mat3 rotated_tensor(Scalar angle, Scalar mx, Scalar my)
{
    const Mat3 r = rotation_z(angle);
    Mat3 local(0);
    local(0, 0) = mx;
    local(1, 1) = my;
    local(2, 2) = Scalar(1);
    return r * local * transpose(r);
}

// Regular subdivided square grid in the xy-plane, consistently oriented.
SurfaceMesh make_grid(size_t nx, size_t ny, Scalar spacing)
{
    SurfaceMesh mesh;
    std::vector<std::vector<Vertex>> v(
        ny + 1, std::vector<Vertex>(nx + 1));
    for (size_t j = 0; j <= ny; ++j)
        for (size_t i = 0; i <= nx; ++i)
            v[j][i] =
                mesh.add_vertex(Point(i * spacing, j * spacing, 0));

    for (size_t j = 0; j < ny; ++j)
        for (size_t i = 0; i < nx; ++i)
        {
            mesh.add_triangle(v[j][i], v[j][i + 1], v[j + 1][i + 1]);
            mesh.add_triangle(v[j][i], v[j + 1][i + 1], v[j + 1][i]);
        }
    return mesh;
}

void attach_uniform_metric(SurfaceMesh& mesh, const Mat3& m)
{
    auto metric = mesh.add_vertex_property<Mat3>("v:metric");
    for (auto v : mesh.vertices())
        metric[v] = m;
}

// Rotate all geometry around the z-axis by angle.
void rotate_geometry(SurfaceMesh& mesh, Scalar angle)
{
    const Mat3 r = rotation_z(angle);
    auto points = mesh.get_vertex_property<Point>("v:point");
    for (auto v : mesh.vertices())
        points[v] = r * points[v];
}

Scalar mean_metric_edge_length(const SurfaceMesh& mesh)
{
    auto metric = mesh.get_vertex_property<Mat3>("v:metric");
    auto points = mesh.get_vertex_property<Point>("v:point");
    Scalar sum(0);
    size_t count = 0;
    for (auto e : mesh.edges())
    {
        const Vertex v0 = mesh.vertex(e, 0);
        const Vertex v1 = mesh.vertex(e, 1);
        const Point d = points[v1] - points[v0];
        const Mat3 m = (metric[v0] + metric[v1]) * Scalar(0.5);
        sum += std::sqrt(std::max<Scalar>(0, dot(d, m * d)));
        ++count;
    }
    return count ? sum / Scalar(count) : Scalar(0);
}

Scalar max_metric_edge_length(const SurfaceMesh& mesh)
{
    auto metric = mesh.get_vertex_property<Mat3>("v:metric");
    auto points = mesh.get_vertex_property<Point>("v:point");
    Scalar mx(0);
    for (auto e : mesh.edges())
    {
        const Vertex v0 = mesh.vertex(e, 0);
        const Vertex v1 = mesh.vertex(e, 1);
        const Point d = points[v1] - points[v0];
        const Mat3 m = (metric[v0] + metric[v1]) * Scalar(0.5);
        mx = std::max(mx, std::sqrt(std::max<Scalar>(0, dot(d, m * d))));
    }
    return mx;
}

// Minimum over edges of the squared projected physical edge length on
// world axis k, normalized by the number of edges (sampling density
// proxy: smaller mean spacing along k means denser sampling).
Scalar mean_spacing_along_axis(const SurfaceMesh& mesh, unsigned int axis)
{
    auto points = mesh.get_vertex_property<Point>("v:point");
    Scalar sum(0);
    size_t count = 0;
    for (auto e : mesh.edges())
    {
        const Point d = points[mesh.vertex(e, 1)] -
                        points[mesh.vertex(e, 0)];
        sum += std::fabs(d[axis]);
        ++count;
    }
    return sum / Scalar(count);
}

bool all_finite_metrics(const SurfaceMesh& mesh)
{
    auto metric = mesh.get_vertex_property<Mat3>("v:metric");
    if (!metric)
        return false;
    for (auto v : mesh.vertices())
        for (int i = 0; i < 9; ++i)
            if (!std::isfinite(metric[v][i]))
                return false;
    return true;
}

// Mean over edges of the absolute projected edge length onto direction
// d. Under a fixed connectivity budget, finer sampling along d shows up
// as a smaller mean projection.
Scalar mean_projection_along(const SurfaceMesh& mesh, const Point& d)
{
    auto points = mesh.get_vertex_property<Point>("v:point");
    Scalar sum(0);
    for (auto e : mesh.edges())
    {
        const Point edge = points[mesh.vertex(e, 1)] -
                           points[mesh.vertex(e, 0)];
        sum += std::fabs(dot(edge, d));
    }
    return sum / Scalar(mesh.n_edges());
}

// ---------------------------------------------------------------------------
// metric_direction: rotated non-diagonal tensors steer sampling direction
// ---------------------------------------------------------------------------

int direction_basic()
{
    // Strong metric weight along x: edges parallel to x must become short,
    // edges parallel to y may remain long.
    SurfaceMesh dense_x = make_grid(6, 6, Scalar(0.4));
    attach_uniform_metric(dense_x, rotated_tensor(0, 100, 1));
    anisotropic_remeshing(dense_x, 6);

    TASK_CHECK(dense_x.is_triangle_mesh());
    TASK_CHECK(all_finite_metrics(dense_x));
    TASK_CHECK(dense_x.n_vertices() > 49);
    TASK_CHECK(max_metric_edge_length(dense_x) <= Scalar(1.5));

    // spacing along x should be much smaller than spacing along y
    const Scalar sx = mean_spacing_along_axis(dense_x, 0);
    const Scalar sy = mean_spacing_along_axis(dense_x, 1);
    TASK_CHECK(sx < Scalar(0.6) * sy);

    // swapped tensor: now sampling must be dense along y
    SurfaceMesh dense_y = make_grid(6, 6, Scalar(0.4));
    attach_uniform_metric(dense_y, rotated_tensor(0, 1, 100));
    anisotropic_remeshing(dense_y, 6);

    const Scalar sx2 = mean_spacing_along_axis(dense_y, 0);
    const Scalar sy2 = mean_spacing_along_axis(dense_y, 1);
    TASK_CHECK(sy2 < Scalar(0.6) * sx2);
    TASK_CHECK(max_metric_edge_length(dense_y) <= Scalar(1.5));
    return 0;
}

int direction_rotated_tensor()
{
    // A non-diagonal tensor whose dense direction is rotated by 30
    // degrees. In world coordinates the preferred short-edge direction
    // is the eigenvector with the large eigenvalue (100).
    const Scalar angle = pi / Scalar(6);
    SurfaceMesh mesh = make_grid(8, 8, Scalar(0.3));
    attach_uniform_metric(mesh, rotated_tensor(angle, 100, 1));
    anisotropic_remeshing(mesh, 8);

    TASK_CHECK(mesh.is_triangle_mesh());
    TASK_CHECK(all_finite_metrics(mesh));
    TASK_CHECK(max_metric_edge_length(mesh) <= Scalar(1.5));

    // sampling must be much finer along the rotated dense direction
    // (eigenvalue 100) than along the perpendicular sparse direction
    const Point dense_dir(std::cos(angle), std::sin(angle), 0);
    const Point sparse_dir(-std::sin(angle), std::cos(angle), 0);
    const Scalar dense_spacing =
        mean_projection_along(mesh, dense_dir);
    const Scalar sparse_spacing =
        mean_projection_along(mesh, sparse_dir);
    TASK_CHECK(dense_spacing < Scalar(0.5) * sparse_spacing);
    return 0;
}

int direction_rotated_model()
{
    // Rotate model and tensor together: results must be the rotated
    // version of the axis-aligned case, i.e. dense sampling follows the
    // model's local x direction rather than world x.
    const Scalar angle = pi / Scalar(4);

    SurfaceMesh world = make_grid(7, 7, Scalar(0.35));
    rotate_geometry(world, angle);
    attach_uniform_metric(world, rotated_tensor(angle, 100, 1));
    anisotropic_remeshing(world, 6);

    TASK_CHECK(world.is_triangle_mesh());
    TASK_CHECK(max_metric_edge_length(world) <= Scalar(1.5));

    const Point local_x(std::cos(angle), std::sin(angle), 0);
    const Point local_y(-std::sin(angle), std::cos(angle), 0);

    Scalar dx(0), dy(0);
    size_t count = 0;
    auto points = world.get_vertex_property<Point>("v:point");
    for (auto e : world.edges())
    {
        const Point d = points[world.vertex(e, 1)] -
                        points[world.vertex(e, 0)];
        dx += std::fabs(dot(d, local_x));
        dy += std::fabs(dot(d, local_y));
        ++count;
    }
    dx /= Scalar(count);
    dy /= Scalar(count);
    TASK_CHECK(dx < Scalar(0.6) * dy);
    return 0;
}

// ---------------------------------------------------------------------------
// metric_transport: varying reference field, projection, interpolation
// ---------------------------------------------------------------------------

int transport_varying_field()
{
    SurfaceMesh mesh = make_grid(6, 6, Scalar(0.35));
    auto metric = mesh.add_vertex_property<Mat3>("v:metric");

    // metric varies smoothly across the grid: dense along x on the left,
    // dense along y on the right
    auto points = mesh.get_vertex_property<Point>("v:point");
    for (auto v : mesh.vertices())
    {
        const Scalar t = points[v][0] / Scalar(2.1);
        Mat3 m(0);
        m(0, 0) = Scalar(100) * (1 - t) + Scalar(1) * t;
        m(1, 1) = Scalar(1) * (1 - t) + Scalar(100) * t;
        m(2, 2) = 1;
        metric[v] = m;
    }

    const size_t vertices_before = mesh.n_vertices();
    anisotropic_remeshing(mesh, 5);

    TASK_CHECK(mesh.n_vertices() > vertices_before);
    TASK_CHECK(all_finite_metrics(mesh));
    TASK_CHECK(mesh.is_triangle_mesh());

    // every resulting tensor must stay symmetric positive definite and
    // remain within the eigenvalue envelope of the frozen field
    for (auto v : mesh.vertices())
    {
        const Mat3& m = metric[v];
        TASK_CHECK(nearly_equal(m(0, 1), m(1, 0), Scalar(1e-8)));
        TASK_CHECK(nearly_equal(m(0, 2), m(2, 0), Scalar(1e-8)));
        TASK_CHECK(nearly_equal(m(1, 2), m(2, 1), Scalar(1e-8)));
        TASK_CHECK(m(0, 0) > Scalar(0.5));
        TASK_CHECK(m(1, 1) > Scalar(0.5));
        TASK_CHECK(m(2, 2) > Scalar(0.5));
    }
    return 0;
}

int transport_reference_freeze()
{
    // repeated iterations must keep interpolating from the original
    // tensor range, so the transported metrics can never leave the
    // convex hull of the input tensors.
    SurfaceMesh mesh = make_grid(5, 5, Scalar(0.4));
    auto metric = mesh.add_vertex_property<Mat3>("v:metric");
    for (auto v : mesh.vertices())
    {
        Mat3 m(0);
        m(0, 0) = Scalar(25);
        m(1, 1) = Scalar(4);
        m(2, 2) = Scalar(1);
        metric[v] = m;
    }

    anisotropic_remeshing(mesh, 8);

    TASK_CHECK(mesh.n_vertices() > 25);
    for (auto v : mesh.vertices())
    {
        const Mat3& m = metric[v];
        // barycentric interpolation of a constant field is exact
        TASK_CHECK(nearly_equal(m(0, 0), Scalar(25), Scalar(1e-8)));
        TASK_CHECK(nearly_equal(m(1, 1), Scalar(4), Scalar(1e-8)));
        TASK_CHECK(nearly_equal(m(2, 2), Scalar(1), Scalar(1e-8)));
        TASK_CHECK(nearly_equal(m(0, 1), 0, Scalar(1e-8)));
    }
    return 0;
}

int transport_multi_round_edits()
{
    // several iterations of the full pipeline on a coarse mesh must
    // produce a refined manifold mesh whose metric edge lengths stay
    // bounded and whose vertices stay on the reference plane.
    SurfaceMesh mesh = make_grid(4, 4, Scalar(0.5));
    attach_uniform_metric(mesh, rotated_tensor(pi / Scalar(5), 64, 4));

    anisotropic_remeshing(mesh, 10);

    TASK_CHECK(mesh.is_triangle_mesh());
    TASK_CHECK(mesh.n_vertices() > 16);
    TASK_CHECK(all_finite_metrics(mesh));
    TASK_CHECK(max_metric_edge_length(mesh) <= Scalar(1.6));
    TASK_CHECK(mean_metric_edge_length(mesh) > Scalar(0));

    auto points = mesh.get_vertex_property<Point>("v:point");
    for (auto v : mesh.vertices())
        TASK_CHECK(std::fabs(points[v][2]) <= Scalar(1e-9));

    // no degenerate or inverted triangles
    for (auto f : mesh.faces())
    {
        Point p[3];
        int k = 0;
        for (auto v : mesh.vertices(f))
            p[k++] = points[v];
        const Point n = cross(p[1] - p[0], p[2] - p[0]);
        TASK_CHECK(n[2] > Scalar(1e-12));
    }
    return 0;
}

int transport_boundary_polyline()
{
    // the boundary loop must stay on its original polyline: boundary
    // vertices remain in the plane's boundary line set, boundary corners
    // never move, and refinement may add vertices along the polylines.
    SurfaceMesh mesh = make_grid(4, 4, Scalar(0.5));
    attach_uniform_metric(mesh, rotated_tensor(0, 36, 4));

    auto points = mesh.get_vertex_property<Point>("v:point");

    // the four grid corners are polyline corners and must not move
    std::vector<Vertex> corner_handles;
    std::vector<Point> corners;
    for (auto v : mesh.vertices())
    {
        const Point& p = points[v];
        const bool on_x = nearly_equal(p[0], 0) || nearly_equal(p[0], 2);
        const bool on_y = nearly_equal(p[1], 0) || nearly_equal(p[1], 2);
        if (on_x && on_y)
        {
            corner_handles.push_back(v);
            corners.push_back(p);
        }
    }
    TASK_CHECK(corners.size() == 4);

    anisotropic_remeshing(mesh, 6);

    TASK_CHECK(mesh.is_triangle_mesh());
    TASK_CHECK(all_finite_metrics(mesh));
    TASK_CHECK(mesh.n_vertices() > 16);

    // the corner vertex handles are unchanged and did not move
    TASK_CHECK(corner_handles.size() == corners.size());
    for (size_t i = 0; i < corner_handles.size(); ++i)
        TASK_CHECK(points[corner_handles[i]] == corners[i]);

    // every boundary vertex still lies on one of the original boundary
    // lines, and the grid corners are unchanged
    size_t boundary_vertices = 0;
    for (auto v : mesh.vertices())
    {
        if (!mesh.is_boundary(v))
            continue;
        ++boundary_vertices;
        const Point& p = points[v];
        const bool on_boundary_line =
            nearly_equal(p[0], 0) || nearly_equal(p[0], 2) ||
            nearly_equal(p[1], 0) || nearly_equal(p[1], 2);
        TASK_CHECK(on_boundary_line);
        TASK_CHECK(p[2] == 0);

        for (const Point& c : corners)
            if (nearly_equal(p[0], c[0]) && nearly_equal(p[1], c[1]))
                TASK_CHECK(p == c);
    }
    TASK_CHECK(boundary_vertices >= 16);
    return 0;
}

int transport_feature_polyline()
{
    // an interior feature edge keeps its polyline shape: its endpoints
    // are feature corners and do not move, while the edge may be split.
    SurfaceMesh mesh = make_grid(4, 4, Scalar(0.5));
    attach_uniform_metric(mesh, rotated_tensor(0, 36, 4));

    auto efeature = mesh.add_edge_property<bool>("e:feature");
    auto vfeature = mesh.add_vertex_property<bool>("v:feature");

    // mark the middle horizontal row (y = 1.0) as a feature polyline
    for (auto e : mesh.edges())
    {
        const Vertex v0 = mesh.vertex(e, 0);
        const Vertex v1 = mesh.vertex(e, 1);
        const Point a = mesh.position(v0);
        const Point b = mesh.position(v1);
        if (nearly_equal(a[1], 1) && nearly_equal(b[1], 1) &&
            nearly_equal(a[2], b[2]))
        {
            efeature[e] = true;
            vfeature[v0] = true;
            vfeature[v1] = true;
        }
    }

    // endpoints of the feature row are corners (only one feature edge)
    Point corner_left, corner_right;
    Vertex corner_left_v = Vertex();
    Vertex corner_right_v = Vertex();
    for (auto v : mesh.vertices())
    {
        if (!vfeature[v])
            continue;
        int c = 0;
        for (auto h : mesh.halfedges(v))
            if (efeature[mesh.edge(h)])
                ++c;
        if (c == 1)
        {
            if (mesh.position(v)[0] < 1)
            {
                corner_left = mesh.position(v);
                corner_left_v = v;
            }
            else
            {
                corner_right = mesh.position(v);
                corner_right_v = v;
            }
        }
    }

    anisotropic_remeshing(mesh, 6);

    TASK_CHECK(mesh.is_triangle_mesh());
    TASK_CHECK(all_finite_metrics(mesh));

    // feature corner handles survive and did not move
    auto points = mesh.get_vertex_property<Point>("v:point");
    TASK_CHECK(points[corner_left_v] == corner_left);
    TASK_CHECK(points[corner_right_v] == corner_right);

    // feature vertices stay on the row
    for (auto v : mesh.vertices())
    {
        if (!vfeature[v])
            continue;
        const Point& p = points[v];
        TASK_CHECK(nearly_equal(p[1], 1));
        if (nearly_equal(p[0], corner_left[0]))
            TASK_CHECK(p == corner_left);
        if (nearly_equal(p[0], corner_right[0]))
            TASK_CHECK(p == corner_right);
    }

    // the feature polyline was refined and remains a chain of feature
    // edges connecting the two fixed corners
    size_t feature_edges = 0;
    for (auto e : mesh.edges())
        if (efeature[e])
            ++feature_edges;
    TASK_CHECK(feature_edges >= 4);
    return 0;
}

// A smooth bump surface: z = h * exp(-(x^2+y^2)/s^2), consistently
// oriented, exercising projection onto a curved frozen reference.
SurfaceMesh make_bump_grid(size_t n, Scalar extent, Scalar height)
{
    SurfaceMesh mesh;
    std::vector<std::vector<Vertex>> v(
        n + 1, std::vector<Vertex>(n + 1));
    const Scalar spacing = Scalar(2) * extent / Scalar(n);
    for (size_t j = 0; j <= n; ++j)
        for (size_t i = 0; i <= n; ++i)
        {
            const Scalar x = -extent + i * spacing;
            const Scalar y = -extent + j * spacing;
            const Scalar z =
                height * std::exp(-(x * x + y * y));
            v[j][i] = mesh.add_vertex(Point(x, y, z));
        }
    for (size_t j = 0; j < n; ++j)
        for (size_t i = 0; i < n; ++i)
        {
            mesh.add_triangle(v[j][i], v[j][i + 1], v[j + 1][i + 1]);
            mesh.add_triangle(v[j][i], v[j + 1][i + 1], v[j + 1][i]);
        }
    return mesh;
}

int transport_curved_surface()
{
    SurfaceMesh mesh = make_bump_grid(8, Scalar(1.5), Scalar(0.8));
    attach_uniform_metric(mesh, rotated_tensor(pi / Scalar(7), 36, 4));

    anisotropic_remeshing(mesh, 8);

    auto points = mesh.get_vertex_property<Point>("v:point");
    TASK_CHECK(mesh.is_triangle_mesh());
    TASK_CHECK(mesh.n_vertices() > 81);
    TASK_CHECK(all_finite_metrics(mesh));

    // all faces stay non-degenerate and keep the upward orientation of
    // the consistently oriented input
    for (auto f : mesh.faces())
    {
        Point p[3];
        int k = 0;
        for (auto v : mesh.vertices(f))
            p[k++] = points[v];
        const Point n = cross(p[1] - p[0], p[2] - p[0]);
        TASK_CHECK(dot(n, n) > Scalar(1e-20));
        TASK_CHECK(n[2] > 0);
    }

    // every metric stays symmetric positive definite after repeated
    // barycentric transport on the curved reference
    auto metric = mesh.get_vertex_property<Mat3>("v:metric");
    for (auto v : mesh.vertices())
    {
        const Mat3& m = metric[v];
        TASK_CHECK(nearly_equal(m(0, 1), m(1, 0), Scalar(1e-8)));
        TASK_CHECK(nearly_equal(m(0, 2), m(2, 0), Scalar(1e-8)));
        TASK_CHECK(nearly_equal(m(1, 2), m(2, 1), Scalar(1e-8)));
        TASK_CHECK(m(0, 0) > 0 && m(1, 1) > 0 && m(2, 2) > 0);
    }
    return 0;
}

int transport_zero_iterations()
{
    SurfaceMesh mesh = make_grid(3, 3, Scalar(0.5));
    attach_uniform_metric(mesh, rotated_tensor(0, 9, 1));
    auto points = mesh.get_vertex_property<Point>("v:point");
    auto metric = mesh.get_vertex_property<Mat3>("v:metric");

    std::vector<Point> ref_points;
    std::vector<Mat3> ref_metrics;
    for (auto v : mesh.vertices())
    {
        ref_points.push_back(points[v]);
        ref_metrics.push_back(metric[v]);
    }
    const size_t nv = mesh.n_vertices();
    const size_t nf = mesh.n_faces();

    anisotropic_remeshing(mesh, 0);

    TASK_CHECK(mesh.n_vertices() == nv);
    TASK_CHECK(mesh.n_faces() == nf);
    size_t i = 0;
    for (auto v : mesh.vertices())
    {
        TASK_CHECK(points[v] == ref_points[i]);
        TASK_CHECK(metric[v] == ref_metrics[i]);
        ++i;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// metric_compat: validation errors and the legacy scalar entry points
// ---------------------------------------------------------------------------

int compat_missing_property()
{
    SurfaceMesh mesh = make_grid(2, 2, Scalar(0.5));
    bool threw = false;
    try
    {
        anisotropic_remeshing(mesh, 3);
    }
    catch (const InvalidInputException&)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    // mesh untouched after the failed call
    TASK_CHECK(mesh.n_vertices() == 9);
    TASK_CHECK(mesh.n_faces() == 8);
    return 0;
}

int compat_non_finite()
{
    SurfaceMesh mesh = make_grid(2, 2, Scalar(0.5));
    auto metric = mesh.add_vertex_property<Mat3>("v:metric");
    for (auto v : mesh.vertices())
    {
        Mat3 m(0);
        m(0, 0) = m(1, 1) = m(2, 2) = 1;
        metric[v] = m;
    }
    metric[Vertex(0)](1, 1) =
        std::numeric_limits<Scalar>::infinity();

    bool threw = false;
    try
    {
        anisotropic_remeshing(mesh, 3);
    }
    catch (const InvalidInputException&)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(mesh.n_vertices() == 9);
    return 0;
}

int compat_non_symmetric()
{
    SurfaceMesh mesh = make_grid(2, 2, Scalar(0.5));
    auto metric = mesh.add_vertex_property<Mat3>("v:metric");
    for (auto v : mesh.vertices())
    {
        Mat3 m(0);
        m(0, 0) = m(1, 1) = m(2, 2) = 1;
        metric[v] = m;
    }
    metric[Vertex(1)](0, 1) = Scalar(0.5);

    bool threw = false;
    try
    {
        anisotropic_remeshing(mesh, 2);
    }
    catch (const InvalidInputException&)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(mesh.n_vertices() == 9);
    return 0;
}

int compat_non_positive_definite()
{
    SurfaceMesh mesh = make_grid(2, 2, Scalar(0.5));
    auto metric = mesh.add_vertex_property<Mat3>("v:metric");
    for (auto v : mesh.vertices())
    {
        Mat3 m(0);
        m(0, 0) = m(1, 1) = m(2, 2) = 1;
        metric[v] = m;
    }
    metric[Vertex(2)](0, 0) = Scalar(-1);

    bool threw = false;
    try
    {
        anisotropic_remeshing(mesh, 2);
    }
    catch (const InvalidInputException&)
    {
        threw = true;
    }
    TASK_CHECK(threw);

    // zero iterations must perform the same validation
    SurfaceMesh mesh2 = make_grid(2, 2, Scalar(0.5));
    auto metric2 = mesh2.add_vertex_property<Mat3>("v:metric");
    for (auto v : mesh2.vertices())
    {
        Mat3 m(0);
        m(0, 0) = m(1, 1) = m(2, 2) = 1;
        metric2[v] = m;
    }
    metric2[Vertex(0)](1, 1) = Scalar(0);
    threw = false;
    try
    {
        anisotropic_remeshing(mesh2, 0);
    }
    catch (const InvalidInputException&)
    {
        threw = true;
    }
    TASK_CHECK(threw);
    TASK_CHECK(mesh2.n_vertices() == 9);
    return 0;
}

int compat_legacy_entry_points()
{
    // uniform and adaptive remeshing must keep their original behavior
    // and must neither require nor consume the metric property
    SurfaceMesh uniform_mesh = make_grid(4, 4, Scalar(0.5));
    uniform_remeshing(uniform_mesh, Scalar(0.3), 2, true);
    TASK_CHECK(uniform_mesh.is_triangle_mesh());
    TASK_CHECK(uniform_mesh.n_vertices() > 16);
    TASK_CHECK(!uniform_mesh.has_vertex_property("v:metric"));

    // even with a metric property present, the scalar entry point
    // ignores it and gives identical results
    SurfaceMesh a = make_grid(4, 4, Scalar(0.5));
    SurfaceMesh b = make_grid(4, 4, Scalar(0.5));
    auto ma = a.add_vertex_property<Mat3>("v:metric");
    auto mb = b.add_vertex_property<Mat3>("v:metric");
    for (auto v : a.vertices())
    {
        Mat3 m(0);
        m(0, 0) = 100;
        m(1, 1) = 1;
        m(2, 2) = 1;
        ma[v] = m;
        Mat3 n(0);
        n(0, 0) = 1;
        n(1, 1) = 100;
        n(2, 2) = 1;
        mb[v] = n;
    }
    uniform_remeshing(a, Scalar(0.3), 2, true);
    uniform_remeshing(b, Scalar(0.3), 2, true);
    TASK_CHECK(a.n_vertices() == b.n_vertices());
    TASK_CHECK(a.n_faces() == b.n_faces());

    Scalar delta(0);
    auto pa = a.get_vertex_property<Point>("v:point");
    auto pb = b.get_vertex_property<Point>("v:point");
    for (auto v : a.vertices())
        delta += norm(pa[v] - pb[v]);
    TASK_CHECK(delta == 0);
    return 0;
}

int compat_adaptive_entry_point()
{
    // adaptive remeshing still runs with its original scalar signature
    SurfaceMesh mesh = make_grid(5, 5, Scalar(0.4));
    const size_t vertices_before = mesh.n_vertices();
    adaptive_remeshing(mesh, Scalar(0.1), Scalar(0.8), Scalar(0.05), 2,
                       true);
    TASK_CHECK(mesh.is_triangle_mesh());
    TASK_CHECK(mesh.n_vertices() > 0);
    TASK_CHECK(mesh.n_vertices() <= vertices_before);
    TASK_CHECK(!mesh.has_vertex_property("v:metric"));
    return 0;
}

} // namespace

const task_case task_cases[] = {
    {"metric_direction", "basic_tensor", direction_basic},
    {"metric_direction", "rotated_tensor", direction_rotated_tensor},
    {"metric_direction", "rotated_model", direction_rotated_model},
    {"metric_transport", "varying_field", transport_varying_field},
    {"metric_transport", "reference_freeze", transport_reference_freeze},
    {"metric_transport", "multi_round_edits",
     transport_multi_round_edits},
    {"metric_transport", "boundary_polyline",
     transport_boundary_polyline},
    {"metric_transport", "feature_polyline", transport_feature_polyline},
    {"metric_transport", "curved_surface", transport_curved_surface},
    {"metric_transport", "zero_iterations", transport_zero_iterations},
    {"metric_compat", "missing_property", compat_missing_property},
    {"metric_compat", "non_finite", compat_non_finite},
    {"metric_compat", "non_symmetric", compat_non_symmetric},
    {"metric_compat", "non_positive_definite",
     compat_non_positive_definite},
    {"metric_compat", "legacy_uniform", compat_legacy_entry_points},
    {"metric_compat", "legacy_adaptive", compat_adaptive_entry_point},
};

const size_t task_case_count =
    sizeof(task_cases) / sizeof(task_cases[0]);
