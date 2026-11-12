// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "gtest/gtest.h"

#include "pmp/algorithms/arap.h"

#include <Eigen/Dense>
#include <Eigen/SVD>

#include <cmath>
#include <functional>
#include <limits>
#include <map>

using namespace pmp;

namespace {

using EVec3 = Eigen::Vector3d;
using EMat3 = Eigen::Matrix3d;

EVec3 as_vec(const Point& p)
{
    return EVec3(p[0], p[1], p[2]);
}

bool finite_point(const Point& p)
{
    return std::isfinite(p[0]) && std::isfinite(p[1]) &&
           std::isfinite(p[2]);
}

// Build a triangulated nx * ny grid with an asymmetric height field.
// The alternating diagonal and the non-planar height make it an
// asymmetric, non-coplanar, open triangle surface.
SurfaceMesh make_grid(int nx, int ny, std::vector<Vertex>& vertices)
{
    SurfaceMesh mesh;
    vertices.clear();
    vertices.reserve(nx * ny);

    auto height = [](int i, int j) {
        return 0.4 * std::sin(0.8 * i + 0.3) * std::cos(0.6 * j) +
               0.07 * i - 0.05 * j;
    };

    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
            vertices.push_back(mesh.add_vertex(
                Point(static_cast<double>(i), static_cast<double>(j),
                      height(i, j))));

    auto id = [&](int i, int j) { return vertices[j * nx + i]; };

    for (int j = 0; j + 1 < ny; ++j)
    {
        for (int i = 0; i + 1 < nx; ++i)
        {
            const Vertex a = id(i, j);
            const Vertex b = id(i + 1, j);
            const Vertex c = id(i + 1, j + 1);
            const Vertex d = id(i, j + 1);
            if ((i + j) % 2 == 0)
            {
                mesh.add_triangle(a, b, c);
                mesh.add_triangle(a, c, d);
            }
            else
            {
                mesh.add_triangle(a, b, d);
                mesh.add_triangle(b, c, d);
            }
        }
    }
    return mesh;
}

// An open strip: a grid with only two rows of vertices.
SurfaceMesh make_strip(std::vector<Vertex>& vertices)
{
    return make_grid(7, 2, vertices);
}

// Independently rebuild the clamped cotangent edge weights from the
// rest positions and the triangle connectivity. Dense n x n matrix,
// n is vertices_size() so deleted slots simply keep a zero row.
Eigen::MatrixXd reference_weights(const SurfaceMesh& mesh,
                                  const std::vector<Point>& rest)
{
    const auto n = mesh.vertices_size();
    Eigen::MatrixXd W = Eigen::MatrixXd::Zero(n, n);

    for (auto f : mesh.faces())
    {
        std::array<Vertex, 3> v{};
        unsigned int k = 0;
        for (auto vv : mesh.vertices(f))
            v[k++] = vv;

        const EVec3 p0 = as_vec(rest[v[0].idx()]);
        const EVec3 p1 = as_vec(rest[v[1].idx()]);
        const EVec3 p2 = as_vec(rest[v[2].idx()]);
        const double area2 = (p1 - p0).cross(p2 - p0).norm();
        EXPECT_GT(area2, 0.0);

        // cotangent at vertex k belongs to the opposite edge
        const double cot0 = (p1 - p0).dot(p2 - p0) / area2;
        const double cot1 = (p0 - p1).dot(p2 - p1) / area2;
        const double cot2 = (p0 - p2).dot(p1 - p2) / area2;

        W(v[1].idx(), v[2].idx()) += 0.5 * cot0;
        W(v[2].idx(), v[1].idx()) += 0.5 * cot0;
        W(v[0].idx(), v[2].idx()) += 0.5 * cot1;
        W(v[2].idx(), v[0].idx()) += 0.5 * cot1;
        W(v[0].idx(), v[1].idx()) += 0.5 * cot2;
        W(v[1].idx(), v[0].idx()) += 0.5 * cot2;
    }

    // clamp the per-edge weights to non-negative values
    for (Eigen::Index i = 0; i < n; ++i)
        for (Eigen::Index j = 0; j < n; ++j)
            W(i, j) = std::max(0.0, W(i, j));

    return W;
}

// Independent weighted Procrustes rotation fit. With reflect == false the
// result is restricted to proper rotations (det +1); with reflect == true
// the unrestricted nearest orthogonal matrix is returned.
EMat3 fit_rotation(const Eigen::MatrixXd& W,
                  const std::vector<EVec3>& rest,
                  const std::vector<EVec3>& positions, unsigned int i,
                  bool reflect)
{
    EMat3 S = EMat3::Zero();
    for (Eigen::Index j = 0; j < W.cols(); ++j)
    {
        const double w = W(i, j);
        if (w <= 0.0)
            continue;
        S += w * (rest[i] - rest[j]) *
             (positions[i] - positions[j]).transpose();
    }

    Eigen::JacobiSVD<EMat3> svd(S, Eigen::ComputeFullU |
                                      Eigen::ComputeFullV);
    EMat3 D = EMat3::Identity();
    if (!reflect &&
        svd.matrixV().determinant() * svd.matrixU().determinant() < 0.0)
        D(2, 2) = -1.0;
    return svd.matrixV() * D * svd.matrixU().transpose();
}

std::vector<EMat3> fit_all_rotations(const Eigen::MatrixXd& W,
                                    const std::vector<EVec3>& rest,
                                    const std::vector<EVec3>& positions,
                                    bool reflect)
{
    std::vector<EMat3> R(rest.size(), EMat3::Identity());
    for (unsigned int i = 0; i < rest.size(); ++i)
    {
        if (W.row(i).sum() <= 0.0)
            continue;
        R[i] = fit_rotation(W, rest, positions, i, reflect);
    }
    return R;
}

// Full directed ARAP energy evaluated from independently fitted rotations.
double reference_energy(const Eigen::MatrixXd& W,
                        const std::vector<EVec3>& rest,
                        const std::vector<EVec3>& positions,
                        const std::vector<EMat3>& R)
{
    double energy = 0.0;
    const auto n = W.rows();
    for (Eigen::Index i = 0; i < n; ++i)
    {
        for (Eigen::Index j = i + 1; j < n; ++j)
        {
            const double w = W(i, j);
            if (w <= 0.0)
                continue;
            const EVec3 ri =
                (positions[i] - positions[j]) - R[i] * (rest[i] - rest[j]);
            const EVec3 rj =
                (positions[j] - positions[i]) - R[j] * (rest[j] - rest[i]);
            energy += w * (ri.squaredNorm() + rj.squaredNorm());
        }
    }
    return energy;
}

std::vector<EVec3> snapshot(const SurfaceMesh& mesh)
{
    std::vector<EVec3> p(mesh.vertices_size());
    for (auto v : mesh.vertices())
        p[v.idx()] = as_vec(mesh.position(v));
    return p;
}

std::vector<EVec3> rest_snapshot(const std::vector<Point>& rest)
{
    std::vector<EVec3> p(rest.size());
    for (size_t i = 0; i < rest.size(); ++i)
        p[i] = as_vec(rest[i]);
    return p;
}

// Independently assemble the dense constrained global system for one
// local/global step and return its solution for every (active) vertex.
std::vector<EVec3> reference_global_step(
    const SurfaceMesh& mesh, const Eigen::MatrixXd& W,
    const std::vector<EVec3>& rest, const std::vector<EVec3>& start,
    const std::map<unsigned int, EVec3>& targets)
{
    const auto n = static_cast<unsigned int>(W.rows());
    std::vector<unsigned char> constrained(n, 0);
    for (auto& [idx, target] : targets)
        constrained[idx] = 1;

    std::vector<EMat3> R(n, EMat3::Identity());
    for (unsigned int i = 0; i < n; ++i)
        if (W.row(i).sum() > 0.0)
            R[i] = fit_rotation(W, rest, start, i, false);

    std::vector<int> free_idx(n, -1);
    int n_free = 0;
    for (unsigned int i = 0; i < n; ++i)
        if (W.row(i).sum() > 0.0 && !constrained[i])
            free_idx[i] = n_free++;

    Eigen::MatrixXd Lff = Eigen::MatrixXd::Zero(n_free, n_free);
    Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(n_free, 3);

    for (unsigned int i = 0; i < n; ++i)
    {
        if (free_idx[i] == -1)
            continue;
        const int fi = free_idx[i];
        for (unsigned int j = 0; j < n; ++j)
        {
            const double w = W(i, j);
            if (w <= 0.0)
                continue;
            const EVec3 c =
                0.5 * w * (R[i] + R[j]) * (rest[i] - rest[j]);
            rhs.row(fi) += c.transpose();
            if (free_idx[j] != -1)
            {
                Lff(fi, fi) += w;
                Lff(fi, free_idx[j]) -= w;
            }
            else
            {
                Lff(fi, fi) += w;
                rhs.row(fi) += w * start[j].transpose();
            }
        }
    }

    Eigen::LDLT<Eigen::MatrixXd> dense_ldlt(Lff);
    EXPECT_EQ(dense_ldlt.info(), Eigen::Success);
    Eigen::MatrixXd sol = dense_ldlt.solve(rhs);

    std::vector<EVec3> result = start;
    for (unsigned int i = 0; i < n; ++i)
    {
        if (free_idx[i] != -1)
            result[i] = sol.row(free_idx[i]).transpose();
        else if (constrained[i])
            result[i] = targets.at(i);
    }
    return result;
}

EMat3 rotation_about(const EVec3& axis, double angle)
{
    const EVec3 a = axis.normalized();
    EMat3 K;
    K << 0.0, -a[2], a[1], a[2], 0.0, -a[0], -a[1], a[0], 0.0;
    return EMat3::Identity() + std::sin(angle) * K +
           (1.0 - std::cos(angle)) * (K * K);
}

void expect_energies_sane(const std::vector<double>& energies,
                          unsigned int iterations)
{
    ASSERT_EQ(energies.size(), iterations + 1);
    for (double e : energies)
    {
        EXPECT_TRUE(std::isfinite(e));
        EXPECT_GE(e, 0.0);
    }
    for (size_t k = 1; k < energies.size(); ++k)
    {
        const double allowed =
            1e-8 * std::max(1.0, energies[k - 1]);
        EXPECT_LE(energies[k], energies[k - 1] + allowed);
    }
}

} // namespace

TEST(ArapTest, rigid_motion)
{
    for (int grid_case = 0; grid_case < 2; ++grid_case)
    {
        std::vector<Vertex> verts;
        SurfaceMesh mesh =
            grid_case == 0 ? make_grid(5, 4, verts) : make_strip(verts);
        const std::vector<Point> rest = mesh.positions();

        ArapDeformation arap(mesh);

        const EMat3 R = rotation_about(EVec3(0.3, 1.0, 0.6), 0.7);
        const EVec3 t(0.5, -0.3, 0.9);

        // constrain a subset of the vertices to their rigid targets
        std::vector<ArapHandle> handles;
        for (unsigned int i = 0; i < verts.size(); i += 3)
        {
            const EVec3 target = R * as_vec(rest[i]) + t;
            handles.push_back(
                {verts[i], Point(target[0], target[1], target[2])});
        }
        arap.set_handles(handles);

        // the exact rigid motion is the global minimum; the local/global
        // iteration converges geometrically to it (the open strip is the
        // slowest case), so iterate to practical convergence
        const auto energies = arap.deform(1000);
        expect_energies_sane(energies, 1000);
        EXPECT_EQ(arap.factorization_count(), 1u);

        double max_rigid_error = 0.0;
        for (auto v : mesh.vertices())
        {
            const EVec3 expected = R * as_vec(rest[v.idx()]) + t;
            const EVec3 got = as_vec(mesh.position(v));
            max_rigid_error =
                std::max(max_rigid_error, (got - expected).norm());
        }
        EXPECT_LT(max_rigid_error, 1e-6);

        // independent energy from rebuilt weights and fitted rotations
        const Eigen::MatrixXd W = reference_weights(mesh, rest);
        const auto rest_e = rest_snapshot(rest);
        const auto pos = snapshot(mesh);
        const auto rot = fit_all_rotations(W, rest_e, pos, false);
        const double independent = reference_energy(W, rest_e, pos, rot);
        EXPECT_LT(independent, 1e-9);
        EXPECT_LT(std::abs(independent - energies.back()) /
                      std::max(1.0, independent),
                  1e-10);

        // with every vertex constrained to its rigid target the answer
        // is exact after a single local/global update
        std::vector<ArapHandle> all_handles;
        for (auto v : mesh.vertices())
        {
            const EVec3 target = R * as_vec(rest[v.idx()]) + t;
            all_handles.push_back(
                {v, Point(target[0], target[1], target[2])});
        }
        arap.set_handles(all_handles);
        const auto exact = arap.deform(2);
        expect_energies_sane(exact, 2);
        for (double e : exact)
            EXPECT_LT(e, 1e-13);
        for (auto v : mesh.vertices())
        {
            const EVec3 expected = R * as_vec(rest[v.idx()]) + t;
            EXPECT_LT((as_vec(mesh.position(v)) - expected).norm(), 1e-12);
        }
    }
}

TEST(ArapTest, bending_energy)
{
    for (int grid_case = 0; grid_case < 2; ++grid_case)
    {
        std::vector<Vertex> verts;
        const int nx = grid_case == 0 ? 6 : 7;
        SurfaceMesh mesh =
            grid_case == 0 ? make_grid(nx, 4, verts) : make_strip(verts);
        const std::vector<Point> rest = mesh.positions();
        const Eigen::MatrixXd W = reference_weights(mesh, rest);

        // pin the left column, lift the right column
        std::vector<ArapHandle> handles;
        std::map<unsigned int, EVec3> targets;
        for (int j = 0; j < (grid_case == 0 ? 4 : 2); ++j)
        {
            const Vertex left = verts[j * nx];
            handles.push_back({left, mesh.position(left)});
            targets[left.idx()] = as_vec(mesh.position(left));

            const Vertex right = verts[j * nx + nx - 1];
            const EVec3 lifted = as_vec(rest[right.idx()]) + EVec3(0, 0, 1.7);
            handles.push_back(
                {right, Point(lifted[0], lifted[1], lifted[2])});
            targets[right.idx()] = lifted;
        }

        ArapDeformation arap(mesh);
        arap.set_handles(handles);

        // ---- one local/global step against a dense reference solve ----
        std::vector<EVec3> start = rest_snapshot(rest);
        for (const auto& [idx, target] : targets)
            start[idx] = target;
        const auto dense =
            reference_global_step(mesh, W, rest_snapshot(rest), start,
                                  targets);

        const auto one_step = arap.deform(1);
        ASSERT_EQ(one_step.size(), 2u);
        for (auto v : mesh.vertices())
        {
            const EVec3 got = as_vec(mesh.position(v));
            EXPECT_LT((got - dense[v.idx()]).norm(), 1e-7);
        }

        // ---- continue bending and check the energy bookkeeping ----
        const auto energies = arap.deform(6);
        expect_energies_sane(energies, 6);

        bool free_vertex_moved = false;
        for (auto v : mesh.vertices())
        {
            if (!targets.count(v.idx()) && W.row(v.idx()).sum() > 0.0)
            {
                if ((as_vec(mesh.position(v)) - as_vec(rest[v.idx()]))
                        .norm() > 1e-6)
                    free_vertex_moved = true;
            }
        }
        EXPECT_TRUE(free_vertex_moved);

        // handles are exact hard constraints
        for (const auto& h : handles)
        {
            const EVec3 got = as_vec(mesh.position(h.vertex));
            const EVec3 want = as_vec(h.target);
            EXPECT_LT((got - want).norm(), 1e-15);
        }

        // independent energy from rebuilt weights and fitted rotations
        const auto rest_e = rest_snapshot(rest);
        const auto pos = snapshot(mesh);
        const auto rot = fit_all_rotations(W, rest_e, pos, false);
        const double independent = reference_energy(W, rest_e, pos, rot);
        EXPECT_LT(std::abs(independent - energies.back()) /
                      std::max(1.0, independent),
                  1e-10);

        // ---- non-coplanar reflection target ----
        std::vector<Vertex> reflection_verts;
        SurfaceMesh reflected_mesh =
            make_grid(5, 4, reflection_verts);
        const std::vector<Point> reflected_rest = reflected_mesh.positions();
        const Eigen::MatrixXd Wr =
            reference_weights(reflected_mesh, reflected_rest);
        ArapDeformation reflected(reflected_mesh);
        std::vector<ArapHandle> all_handles;
        for (auto v : reflected_mesh.vertices())
        {
            const EVec3 p = as_vec(reflected_rest[v.idx()]);
            const EVec3 q(p[0], p[1], -p[2]); // mirror through the xy plane
            all_handles.push_back(
                {v, Point(q[0], q[1], q[2])});
        }
        reflected.set_handles(all_handles);
        const auto reflection_energies = reflected.deform(1);

        const auto r_rest = rest_snapshot(reflected_rest);
        std::vector<EVec3> r_pos(r_rest.size());
        for (auto v : reflected_mesh.vertices())
            r_pos[v.idx()] = EVec3(r_rest[v.idx()][0],
                                  r_rest[v.idx()][1],
                                  -r_rest[v.idx()][2]);

        const auto proper =
            fit_all_rotations(Wr, r_rest, r_pos, false);
        const auto mirrored =
            fit_all_rotations(Wr, r_rest, r_pos, true);
        const double e_proper =
            reference_energy(Wr, r_rest, r_pos, proper);
        const double e_mirrored =
            reference_energy(Wr, r_rest, r_pos, mirrored);

        // allowing det = -1 would reach a strictly lower (near zero) value
        EXPECT_GT(e_proper, 1e-6);
        EXPECT_LT(e_mirrored, 1e-12);
        // the class must report the proper-rotation minimum
        EXPECT_LT(std::abs(reflection_energies[0] - e_proper) /
                      std::max(1.0, e_proper),
                  1e-10);
        // and every fitted rotation must have determinant +1
        for (const auto& Rm : proper)
            EXPECT_NEAR(Rm.determinant(), 1.0, 1e-12);
    }
}

TEST(ArapTest, handle_reuse)
{
    std::vector<Vertex> verts;
    SurfaceMesh mesh = make_grid(6, 5, verts);
    const std::vector<Point> rest = mesh.positions();

    // second object on an identical mesh, replaying the same drags
    std::vector<Vertex> replay_verts;
    SurfaceMesh replay_mesh = make_grid(6, 5, replay_verts);
    const std::vector<Point> replay_rest = replay_mesh.positions();
    ArapDeformation replay(replay_mesh);

    auto make_handles = [&](double lift, bool extra_handle) {
        std::vector<ArapHandle> handles;
        const Vertex anchor = verts[0];
        handles.push_back({anchor, rest[anchor.idx()]});
        const Vertex tip = verts[6 - 1];
        const EVec3 q = as_vec(rest[tip.idx()]) + EVec3(0, 0, lift);
        handles.push_back({tip, Point(q[0], q[1], q[2])});
        if (extra_handle)
        {
            const Vertex mid = verts[2 * 6 + 2];
            const EVec3 m = as_vec(rest[mid.idx()]) + EVec3(0, 0, 0.5 * lift);
            handles.push_back({mid, Point(m[0], m[1], m[2])});
        }
        return handles;
    };

    ArapDeformation arap(mesh);

    arap.set_handles(make_handles(0.8, false));
    arap.deform(4);
    replay.set_handles(make_handles(0.8, false));
    replay.deform(4);
    EXPECT_EQ(arap.factorization_count(), 1u);
    EXPECT_EQ(replay.factorization_count(), 1u);

    // dragging the same handles to new targets reuses the factorization
    arap.set_handles(make_handles(1.4, false));
    arap.deform(4);
    replay.set_handles(make_handles(1.4, false));
    replay.deform(4);
    EXPECT_EQ(arap.factorization_count(), 1u);

    // changing the iteration count does not factorize again either
    arap.deform(20);
    replay.deform(20);
    EXPECT_EQ(arap.factorization_count(), 1u);

    // adding a handle changes the free set: one new factorization
    arap.set_handles(make_handles(1.4, true));
    arap.deform(3);
    replay.set_handles(make_handles(1.4, true));
    replay.deform(3);
    EXPECT_EQ(arap.factorization_count(), 2u);
    EXPECT_EQ(replay.factorization_count(), 2u);

    // removing it changes the free set again: another factorization
    arap.set_handles(make_handles(1.4, false));
    arap.deform(3);
    replay.set_handles(make_handles(1.4, false));
    replay.deform(3);
    EXPECT_EQ(arap.factorization_count(), 3u);
    EXPECT_EQ(replay.factorization_count(), 3u);

    // the two objects, sharing the same rest pose and drag sequence,
    // must agree vertex by vertex (no stale cache / rest pose drift)
    for (auto v : mesh.vertices())
    {
        const EVec3 a = as_vec(mesh.position(v));
        const EVec3 b = as_vec(replay_mesh.position(v));
        EXPECT_LT((a - b).norm(), 1e-12);
    }

    // the rest pose is still the original one: pinning every vertex back
    // to its rest coordinate restores the mesh exactly
    std::vector<ArapHandle> rest_handles;
    for (auto v : mesh.vertices())
        rest_handles.push_back({v, rest[v.idx()]});
    arap.set_handles(rest_handles);
    const auto pinned = arap.deform(2);
    for (auto v : mesh.vertices())
        EXPECT_LT((as_vec(mesh.position(v)) - as_vec(rest[v.idx()]))
                      .norm(),
                  1e-12);
    for (double e : pinned)
        EXPECT_LT(e, 1e-14);

    // nothing ever touched the recorded rest coordinates
    ASSERT_EQ(rest.size(), replay_rest.size());
    for (size_t i = 0; i < rest.size(); ++i)
        for (int c = 0; c < 3; ++c)
            EXPECT_DOUBLE_EQ(rest[i][c], replay_rest[i][c]);
}

TEST(ArapTest, disconnected)
{
    // two unconnected shells plus one isolated vertex
    std::vector<Vertex> va;
    SurfaceMesh shell_a = make_grid(4, 3, va);
    const std::vector<Point> rest_a = shell_a.positions();

    std::vector<Vertex> vb;
    SurfaceMesh shell_b = make_grid(3, 3, vb);
    const EVec3 shift(10.0, 0.0, 0.0);
    for (auto v : shell_b.vertices())
        shell_b.position(v) = Point(shell_b.position(v)[0] + shift[0],
                                    shell_b.position(v)[1],
                                    shell_b.position(v)[2]);

    SurfaceMesh mesh;
    std::map<unsigned int, unsigned int> local_a;
    std::map<unsigned int, unsigned int> local_b;
    for (auto v : shell_a.vertices())
    {
        const Vertex nv = mesh.add_vertex(shell_a.position(v));
        local_a[v.idx()] = nv.idx();
    }
    for (auto v : shell_b.vertices())
    {
        const Vertex nv = mesh.add_vertex(shell_b.position(v));
        local_b[v.idx()] = nv.idx();
    }
    for (auto f : shell_a.faces())
    {
        std::vector<Vertex> face;
        for (auto v : shell_a.vertices(f))
            face.push_back(Vertex(local_a[v.idx()]));
        mesh.add_triangle(face[0], face[1], face[2]);
    }
    for (auto f : shell_b.faces())
    {
        std::vector<Vertex> face;
        for (auto v : shell_b.vertices(f))
            face.push_back(Vertex(local_b[v.idx()]));
        mesh.add_triangle(face[0], face[1], face[2]);
    }
    const Vertex isolated = mesh.add_vertex(Point(20, 20, 20));

    const std::vector<Point> rest = mesh.positions();

    // handles only on shell A
    std::vector<ArapHandle> handles;
    const Vertex a_anchor = Vertex(local_a[0]);
    handles.push_back({a_anchor, mesh.position(a_anchor)});
    const Vertex a_tip_local = va.back();
    const Vertex a_tip = Vertex(local_a[a_tip_local.idx()]);
    const EVec3 lifted = as_vec(rest[a_tip.idx()]) + EVec3(0, 0, 1.3);
    handles.push_back(
        {a_tip, Point(lifted[0], lifted[1], lifted[2])});

    ArapDeformation arap(mesh);
    arap.set_handles(handles);
    const size_t count_before = arap.factorization_count();
    const auto energies = arap.deform(5);
    expect_energies_sane(energies, 5);
    EXPECT_EQ(arap.factorization_count(), count_before + 1);

    // shell B and the isolated vertex do not participate
    for (auto v : shell_b.vertices())
    {
        const unsigned int global = local_b[v.idx()];
        EXPECT_LT((as_vec(mesh.position(Vertex(global))) -
                   as_vec(rest[global]))
                      .norm(),
                  1e-15);
    }
    EXPECT_LT((as_vec(mesh.position(isolated)) - EVec3(20, 20, 20)).norm(),
              1e-15);

    // standalone reference solve of shell A
    ArapDeformation reference(shell_a);
    std::vector<ArapHandle> local_handles;
    local_handles.push_back({va[0], rest_a[va[0].idx()]});
    const EVec3 local_lift =
        as_vec(rest_a[a_tip_local.idx()]) + EVec3(0, 0, 1.3);
    local_handles.push_back(
        {a_tip_local,
         Point(local_lift[0], local_lift[1], local_lift[2])});
    reference.set_handles(local_handles);
    reference.deform(5);
    for (auto v : shell_a.vertices())
    {
        const EVec3 expected = as_vec(shell_a.position(v));
        const EVec3 got =
            as_vec(mesh.position(Vertex(local_a[v.idx()])));
        EXPECT_LT((got - expected).norm(), 1e-10);
    }

    // an isolated vertex with a handle goes straight to its target
    const Point iso_target(21.5, 19.5, 22.0);
    handles.push_back({isolated, iso_target});
    arap.set_handles(handles);
    arap.deform(2);
    EXPECT_LT((as_vec(mesh.position(isolated)) - as_vec(iso_target))
                  .norm(),
              1e-15);

    // releasing everything leaves every component in place
    const std::vector<EVec3> before_release = snapshot(mesh);
    arap.set_handles({});
    const size_t count_at_release = arap.factorization_count();
    const auto released = arap.deform(3);
    expect_energies_sane(released, 3);
    EXPECT_EQ(arap.factorization_count(), count_at_release);
    for (auto v : mesh.vertices())
        EXPECT_LT((as_vec(mesh.position(v)) - before_release[v.idx()])
                      .norm(),
                  1e-15);
    for (size_t k = 1; k < released.size(); ++k)
        EXPECT_DOUBLE_EQ(released[k], released[0]);

    // all vertices fixed: no free system, no new factorization
    const size_t fixed_count = arap.factorization_count();
    std::vector<ArapHandle> all_fixed;
    for (auto v : mesh.vertices())
        all_fixed.push_back({v, mesh.position(v)});
    arap.set_handles(all_fixed);
    const auto fixed_energies = arap.deform(2);
    expect_energies_sane(fixed_energies, 2);
    EXPECT_EQ(arap.factorization_count(), fixed_count);

    const Eigen::MatrixXd W = reference_weights(mesh, rest);
    const auto rest_e = rest_snapshot(rest);
    const auto pos = snapshot(mesh);
    const auto rot = fit_all_rotations(W, rest_e, pos, false);
    EXPECT_LT(std::abs(fixed_energies.back() -
                       reference_energy(W, rest_e, pos, rot)) /
                  std::max(1.0, fixed_energies.back()),
              1e-10);
}

TEST(ArapTest, sparse_handles)
{
    // empty mesh: all-zero energies, no factorization
    {
        SurfaceMesh empty;
        ArapDeformation arap(empty);
        const auto energies = arap.deform(3);
        ASSERT_EQ(energies.size(), 4u);
        for (double e : energies)
            EXPECT_EQ(e, 0.0);
        EXPECT_EQ(arap.factorization_count(), 0u);
    }

    std::vector<Vertex> verts;
    SurfaceMesh mesh = make_strip(verts);
    const std::vector<Point> rest = mesh.positions();
    const Eigen::MatrixXd W = reference_weights(mesh, rest);

    // a single handle anchors the whole connected strip
    ArapDeformation arap(mesh);
    const Vertex anchor = verts[3];
    const EVec3 anchor_target = as_vec(rest[anchor.idx()]) + EVec3(0, 0, 1.1);
    arap.set_handles(
        {{anchor,
          Point(anchor_target[0], anchor_target[1], anchor_target[2])}});
    const auto energies = arap.deform(6);
    expect_energies_sane(energies, 6);
    EXPECT_EQ(arap.factorization_count(), 1u);
    EXPECT_LT((as_vec(mesh.position(anchor)) - anchor_target).norm(), 1e-15);

    bool moved = false;
    for (auto v : mesh.vertices())
        if (v != anchor &&
            (as_vec(mesh.position(v)) - as_vec(rest[v.idx()])).norm() >
                1e-6)
            moved = true;
    EXPECT_TRUE(moved);

    // dense reference for one step of the single-handle case
    {
        SurfaceMesh ref_mesh = make_strip(verts);
        const std::vector<Point> ref_rest = ref_mesh.positions();
        const Eigen::MatrixXd Wr = reference_weights(ref_mesh, ref_rest);
        ArapDeformation ref_arap(ref_mesh);
        const EVec3 q = as_vec(ref_rest[3]) + EVec3(0, 0, 1.1);
        ref_arap.set_handles(
            {{verts[3], Point(q[0], q[1], q[2])}});
        ref_arap.deform(1);

        std::vector<EVec3> start = rest_snapshot(ref_rest);
        start[3] = q;
        const auto dense = reference_global_step(
            ref_mesh, Wr, rest_snapshot(ref_rest), start,
            {{3u, q}});
        for (auto v : ref_mesh.vertices())
            EXPECT_LT((as_vec(ref_mesh.position(v)) - dense[v.idx()])
                          .norm(),
                      1e-7);
    }

    // mesh with deleted faces and a deleted vertex, no garbage collection:
    // valid vertex indices must still work while deleted ones are rejected
    std::vector<Vertex> hole_verts;
    SurfaceMesh hole_mesh = make_grid(4, 4, hole_verts);
    const std::vector<Point> hole_rest = hole_mesh.positions();
    const Vertex corner = hole_verts[0];
    Face doomed_face;
    Vertex doomed_vertex = corner;
    for (auto f : hole_mesh.faces())
    {
        bool touches_corner = false;
        for (auto vv : hole_mesh.vertices(f))
            if (vv == corner)
                touches_corner = true;
        if (touches_corner)
        {
            doomed_face = f;
            break;
        }
    }
    hole_mesh.delete_face(doomed_face);
    hole_mesh.delete_vertex(doomed_vertex);
    EXPECT_GT(hole_mesh.vertices_size(), hole_mesh.n_vertices());
    EXPECT_TRUE(hole_mesh.is_deleted(doomed_vertex));

    ArapDeformation hole(hole_mesh);
    EXPECT_THROW(hole.set_handles({{doomed_vertex, Point(0, 0, 0)}}),
                 InvalidInputException);

    const Vertex survivor = hole_verts.back();
    ASSERT_TRUE(survivor.is_valid());
    ASSERT_FALSE(hole_mesh.is_deleted(survivor));
    const EVec3 survivor_target =
        as_vec(hole_rest[survivor.idx()]) + EVec3(0, 0, 0.9);
    const Vertex other = hole_verts[3];
    ASSERT_FALSE(hole_mesh.is_deleted(other));
    hole.set_handles(
        {{other, hole_rest[other.idx()]},
         {survivor,
          Point(survivor_target[0], survivor_target[1],
                survivor_target[2])}});
    const auto hole_energies = hole.deform(4);
    expect_energies_sane(hole_energies, 4);
    EXPECT_LT((as_vec(hole_mesh.position(survivor)) - survivor_target)
                  .norm(),
              1e-15);
    EXPECT_LT((as_vec(hole_mesh.position(other)) -
               as_vec(hole_rest[other.idx()]))
                  .norm(),
              1e-15);
}

TEST(ArapTest, rejection)
{
    // non-triangle face
    {
        SurfaceMesh quad;
        const auto v0 = quad.add_vertex(Point(0, 0, 0));
        const auto v1 = quad.add_vertex(Point(1, 0, 0));
        const auto v2 = quad.add_vertex(Point(1, 1, 0));
        const auto v3 = quad.add_vertex(Point(0, 1, 0));
        quad.add_quad(v0, v1, v2, v3);
        EXPECT_THROW(ArapDeformation{quad}, InvalidInputException);
    }

    // non-finite coordinates
    {
        SurfaceMesh bad;
        bad.add_vertex(Point(std::numeric_limits<double>::infinity(), 0, 0));
        bad.add_vertex(Point(1, 0, 0));
        bad.add_vertex(Point(0, 1, 0));
        EXPECT_THROW(ArapDeformation{bad}, InvalidInputException);
    }

    // zero-area triangle (three distinct collinear points)
    {
        SurfaceMesh flat;
        const auto a = flat.add_vertex(Point(0, 0, 0));
        const auto b = flat.add_vertex(Point(1, 0, 0));
        const auto c = flat.add_vertex(Point(2, 0, 0));
        flat.add_triangle(a, b, c);
        EXPECT_THROW(ArapDeformation{flat}, InvalidInputException);
    }

    std::vector<Vertex> verts;
    SurfaceMesh mesh = make_grid(5, 4, verts);
    const std::vector<Point> rest = mesh.positions();
    ArapDeformation arap(mesh);

    const Vertex v0 = verts[0];
    const Vertex v1 = verts.back();
    const std::vector<ArapHandle> valid = {
        {v0, rest[v0.idx()]},
        {v1, Point(rest[v1.idx()][0], rest[v1.idx()][1],
                   rest[v1.idx()][2] + 1.0)}};
    arap.set_handles(valid);
    arap.deform(2);

    // invalid handle inputs leave the previous valid set untouched
    EXPECT_THROW(arap.set_handles({{Vertex(), Point(0, 0, 0)}}),
                 InvalidInputException);
    EXPECT_THROW(
        arap.set_handles(
            {{Vertex(mesh.vertices_size() + 5), Point(0, 0, 0)}}),
        InvalidInputException);
    EXPECT_THROW(arap.set_handles({{v0, rest[v0.idx()]}, {v0, rest[v1.idx()]}}),
                 InvalidInputException);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(arap.set_handles(
                     {{v0, Point(nan, 0.0, 0.0)}}),
                 InvalidInputException);

    // old handles are still active and exact
    const auto after_rejection = arap.deform(2);
    expect_energies_sane(after_rejection, 2);
    for (const auto& h : valid)
        EXPECT_LT((as_vec(mesh.position(h.vertex)) - as_vec(h.target))
                      .norm(),
                  1e-15);

    // zero iterations
    EXPECT_THROW(arap.deform(0), InvalidInputException);

    // simulated numerical breakdown: a solve failure must not leave
    // partial coordinates and must not reset the rest pose
    const std::vector<EVec3> committed = snapshot(mesh);
    const Vertex free_v = verts[2 * 5 + 1];
    mesh.position(free_v) =
        Point(nan, std::numeric_limits<double>::infinity(), 0.0);
    EXPECT_THROW(arap.deform(2), SolverException);

    // nothing was written back during the failed call
    for (auto v : mesh.vertices())
    {
        if (v == free_v)
            continue;
        EXPECT_LT((as_vec(mesh.position(v)) - committed[v.idx()]).norm(),
                  1e-15);
    }

    // restore the coordinate and keep solving with the same object
    mesh.position(free_v) =
        Point(committed[free_v.idx()][0], committed[free_v.idx()][1],
              committed[free_v.idx()][2]);
    const auto recovered = arap.deform(3);
    expect_energies_sane(recovered, 3);
    for (auto v : mesh.vertices())
        EXPECT_TRUE(finite_point(mesh.position(v)));

    // a fresh object with the same rest pose reproduces the result
    SurfaceMesh replay_mesh;
    std::vector<Vertex> replay_verts;
    replay_mesh = make_grid(5, 4, replay_verts);
    ArapDeformation replay(replay_mesh);
    replay.set_handles(valid);
    replay.deform(2);
    replay.deform(2);
    replay.deform(3);
    for (auto v : mesh.vertices())
        EXPECT_LT((as_vec(mesh.position(v)) -
                   as_vec(replay_mesh.position(v)))
                      .norm(),
                  1e-12);
}

