// Copyright 2026 the Polygon Mesh Processing Library developers.
// SPDX-License-Identifier: MIT

#include "pmp/algorithms/arap.h"

#include "pmp/algorithms/numerics.h"

#include <Eigen/SVD>
#include <Eigen/SparseCholesky>

#include <array>
#include <cmath>

namespace pmp {

namespace {

using EVec3 = Eigen::Vector3d;
using EMat3 = Eigen::Matrix3d;

bool is_finite(const Point& p)
{
    return std::isfinite(p[0]) && std::isfinite(p[1]) &&
           std::isfinite(p[2]);
}

EVec3 to_eigen(const Point& p)
{
    return EVec3(p[0], p[1], p[2]);
}

// Fit the proper rotation (det = +1) minimizing
// sum_k w_k ||x_k - R p_k||^2 for the weighted edge cross-covariance S.
// For rank deficient neighborhoods the SVD still yields a minimizer
// within the proper rotations (reflections are excluded).
EMat3 fit_proper_rotation(const EMat3& S)
{
    Eigen::JacobiSVD<EMat3> svd(
        S, Eigen::ComputeFullU | Eigen::ComputeFullV);
    const EMat3 U = svd.matrixU();
    const EMat3 V = svd.matrixV();

    EMat3 D = EMat3::Identity();
    if (V.determinant() * U.determinant() < 0.0)
        D(2, 2) = -1.0;

    return V * D * U.transpose();
}

} // namespace

// Cached factorization of the free-vertex system. The signature records
// the free vertex indexing of the last factorization; it is invalidated
// whenever the effective free vertex set changes.
struct ArapSolverCache
{
    Eigen::SimplicialLDLT<SparseMatrix> solver;
    std::vector<int> signature;
    bool factorized{false};
};

ArapDeformation::ArapDeformation(SurfaceMesh& mesh) : mesh_(mesh)
{
    const auto n = mesh.vertices_size();

    rest_.resize(n);
    constrained_.assign(n, 0);
    active_.assign(n, 0);
    edge_weight_.assign(mesh.edges_size(), 0.0);
    cache_ = std::make_unique<ArapSolverCache>();

    for (auto v : mesh.vertices())
    {
        if (!is_finite(mesh.position(v)))
        {
            auto what =
                std::string{__func__} + ": Non-finite vertex coordinates.";
            throw InvalidInputException(what);
        }
        if (!mesh.is_manifold(v))
        {
            auto what =
                std::string{__func__} + ": Mesh is not 2-manifold.";
            throw InvalidInputException(what);
        }
        rest_[v.idx()] = mesh.position(v);
    }

    // validate faces and accumulate cotangent edge contributions
    for (auto f : mesh.faces())
    {
        if (mesh.valence(f) != 3)
        {
            auto what =
                std::string{__func__} + ": Mesh contains non-triangle faces.";
            throw InvalidInputException(what);
        }

        std::array<Vertex, 3> verts{};
        unsigned int k = 0;
        for (auto v : mesh.vertices(f))
            verts[k++] = v;

        const EVec3 p0 = to_eigen(rest_[verts[0].idx()]);
        const EVec3 p1 = to_eigen(rest_[verts[1].idx()]);
        const EVec3 p2 = to_eigen(rest_[verts[2].idx()]);
        const double double_area = (p1 - p0).cross(p2 - p0).norm();

        if (!(double_area > 0.0) || !std::isfinite(double_area))
        {
            auto what =
                std::string{__func__} + ": Triangle has zero area.";
            throw InvalidInputException(what);
        }

        // cotangent of the angle at vertex k belongs to the edge
        // opposite to that vertex, i.e. the edge of the other two corners
        const std::array<double, 3> cot = {
            (p1 - p0).dot(p2 - p0) / double_area,
            (p0 - p1).dot(p2 - p1) / double_area,
            (p0 - p2).dot(p1 - p2) / double_area};

        for (unsigned int i = 0; i < 3; ++i)
        {
            const Edge opposite = mesh.find_edge(
                verts[(i + 1) % 3], verts[(i + 2) % 3]);
            edge_weight_[opposite.idx()] += 0.5 * cot[i];
        }
    }

    // clamp the accumulated edge weights to non-negative values
    for (auto e : mesh.edges())
    {
        double& w = edge_weight_[e.idx()];
        if (w < 0.0)
            w = 0.0;
        if (w > 0.0)
        {
            active_[mesh.vertex(e, 0).idx()] = 1;
            active_[mesh.vertex(e, 1).idx()] = 1;
        }
    }
}

ArapDeformation::~ArapDeformation() = default;

void ArapDeformation::set_handles(
    const std::vector<ArapHandle>& handles)
{
    const auto n = mesh_.vertices_size();
    std::vector<unsigned char> constrained(n, 0);
    std::vector<ArapHandle> validated;
    validated.reserve(handles.size());

    for (const auto& h : handles)
    {
        const Vertex v = h.vertex;
        if (!v.is_valid() || !mesh_.is_valid(v) || mesh_.is_deleted(v))
        {
            auto what =
                std::string{__func__} + ": Handle references invalid vertex.";
            throw InvalidInputException(what);
        }
        if (!is_finite(h.target))
        {
            auto what =
                std::string{__func__} + ": Handle target is not finite.";
            throw InvalidInputException(what);
        }
        if (constrained[v.idx()])
        {
            auto what =
                std::string{__func__} + ": Duplicate handle vertex.";
            throw InvalidInputException(what);
        }
        constrained[v.idx()] = 1;
        validated.push_back(h);
    }

    handles_.swap(validated);
    constrained_.swap(constrained);
}

std::vector<double> ArapDeformation::deform(unsigned int iterations)
{
    if (iterations == 0)
    {
        auto what =
            std::string{__func__} + ": iterations must be at least one.";
        throw InvalidInputException(what);
    }

    std::vector<double> energies(iterations + 1, 0.0);

    const auto n = mesh_.vertices_size();
    if (n == 0)
        return energies;

    // work on a local copy taken from the last committed positions:
    // mesh coordinates are touched again only on success
    std::vector<Point> x(n);
    for (auto v : mesh_.vertices())
        x[v.idx()] = mesh_.position(v);

    // hard constraints: move handle vertices to their targets
    for (const auto& h : handles_)
        x[h.vertex.idx()] = h.target;

    // the solve requires finite working coordinates; targets are already
    // validated as finite, so only caller-corrupted free vertices remain
    for (const auto& p : x)
    {
        if (!is_finite(p))
        {
            auto what = std::string{__func__} +
                        ": Non-finite working coordinates; aborting solve.";
            throw SolverException(what);
        }
    }

    // label components connected by positive-weight edges
    std::vector<int> component(n, -1);
    std::vector<unsigned char> component_has_handle;
    for (auto seed : mesh_.vertices())
    {
        const unsigned int s = seed.idx();
        if (!active_[s] || component[s] != -1)
            continue;

        const int cid = static_cast<int>(component_has_handle.size());
        component_has_handle.push_back(0);
        std::vector<Vertex> stack{seed};
        component[s] = cid;
        while (!stack.empty())
        {
            const Vertex v = stack.back();
            stack.pop_back();
            if (constrained_[v.idx()])
                component_has_handle[cid] = 1;
            for (auto e : mesh_.edges(v))
            {
                if (edge_weight_[e.idx()] <= 0.0)
                    continue;
                Vertex u = mesh_.vertex(e, 0);
                if (u == v)
                    u = mesh_.vertex(e, 1);
                if (component[u.idx()] == -1)
                {
                    component[u.idx()] = cid;
                    stack.push_back(u);
                }
            }
        }
    }

    // free vertices: active, unconstrained, and in a component with a handle
    std::vector<int> free_idx(n, -1);
    int n_free = 0;
    for (auto v : mesh_.vertices())
    {
        const unsigned int i = v.idx();
        if (active_[i] && !constrained_[i])
        {
            const int cid = component[i];
            if (cid != -1 && component_has_handle[cid])
                free_idx[i] = n_free++;
        }
    }

    const bool same_free_set =
        cache_->factorized && cache_->signature.size() == free_idx.size() &&
        cache_->signature == free_idx;

    if (n_free > 0 && !same_free_set)
    {
        // assemble the reduced Laplacian of the free vertices
        std::vector<Triplet> triplets;
        triplets.reserve(mesh_.n_edges() * 2);
        for (auto e : mesh_.edges())
        {
            const double w = edge_weight_[e.idx()];
            if (w <= 0.0)
                continue;
            const int i = mesh_.vertex(e, 0).idx();
            const int j = mesh_.vertex(e, 1).idx();
            const int fi = free_idx[i];
            const int fj = free_idx[j];
            if (fi != -1 && fj != -1)
            {
                triplets.emplace_back(fi, fi, w);
                triplets.emplace_back(fj, fj, w);
                triplets.emplace_back(fi, fj, -w);
                triplets.emplace_back(fj, fi, -w);
            }
            else if (fi != -1)
            {
                triplets.emplace_back(fi, fi, w);
            }
            else if (fj != -1)
            {
                triplets.emplace_back(fj, fj, w);
            }
        }

        SparseMatrix Lff(n_free, n_free);
        Lff.setFromTriplets(triplets.begin(), triplets.end());

        cache_->solver.compute(Lff);
        if (cache_->solver.info() != Eigen::Success)
        {
            cache_->factorized = false;
            cache_->signature.assign(n, -1);
            auto what = std::string{__func__} +
                        ": Failed to factorize linear system.";
            throw SolverException(what);
        }
        cache_->factorized = true;
        cache_->signature = free_idx;
        ++factorization_count_;
    }
    else if (n_free == 0)
    {
        // an empty free system must not be mistaken for a cached one
        cache_->factorized = false;
        cache_->signature = free_idx;
    }

    // fit the optimal proper rotation of every active vertex at state
    auto fit_rotations = [&](const std::vector<Point>& state) {
        std::vector<EMat3> rotations(n, EMat3::Identity());
        for (auto v : mesh_.vertices())
        {
            const unsigned int i = v.idx();
            if (!active_[i])
                continue;

            EMat3 S = EMat3::Zero();
            const EVec3 pi = to_eigen(rest_[i]);
            const EVec3 xi = to_eigen(state[i]);
            for (auto e : mesh_.edges(v))
            {
                const double w = edge_weight_[e.idx()];
                if (w <= 0.0)
                    continue;
                Vertex u = mesh_.vertex(e, 0);
                if (u == v)
                    u = mesh_.vertex(e, 1);
                const EVec3 pj = to_eigen(rest_[u.idx()]);
                const EVec3 xj = to_eigen(state[u.idx()]);
                S += w * (pi - pj) * (xi - xj).transpose();
            }
            rotations[i] = fit_proper_rotation(S);
        }
        return rotations;
    };

    // full directed ARAP energy with the supplied optimal rotations
    auto evaluate = [&](const std::vector<Point>& state,
                        const std::vector<EMat3>& rotations) {
        double energy = 0.0;
        for (auto e : mesh_.edges())
        {
            const double w = edge_weight_[e.idx()];
            if (w <= 0.0)
                continue;
            const unsigned int i = mesh_.vertex(e, 0).idx();
            const unsigned int j = mesh_.vertex(e, 1).idx();
            const EVec3 pi = to_eigen(rest_[i]);
            const EVec3 pj = to_eigen(rest_[j]);
            const EVec3 xi = to_eigen(state[i]);
            const EVec3 xj = to_eigen(state[j]);
            const EVec3 ri = (xi - xj) - rotations[i] * (pi - pj);
            const EVec3 rj = (xj - xi) - rotations[j] * (pj - pi);
            energy += w * (ri.squaredNorm() + rj.squaredNorm());
        }
        return energy;
    };

    // energy at the starting state (handle targets already applied)
    {
        auto rotations = fit_rotations(x);
        energies[0] = evaluate(x, rotations);
    }

    DenseMatrix rhs = DenseMatrix::Zero(n_free, 3);

    for (unsigned int iter = 0; iter < iterations; ++iter)
    {
        // local step: optimal rotations at the current positions
        auto rotations = fit_rotations(x);

        // global step: right-hand side of the reduced position system
        rhs.setZero();
        for (auto e : mesh_.edges())
        {
            const double w = edge_weight_[e.idx()];
            if (w <= 0.0)
                continue;
            const unsigned int i = mesh_.vertex(e, 0).idx();
            const unsigned int j = mesh_.vertex(e, 1).idx();
            const EVec3 d = to_eigen(rest_[i]) - to_eigen(rest_[j]);
            const EVec3 c = 0.5 * w * (rotations[i] + rotations[j]) * d;

            const int fi = free_idx[i];
            const int fj = free_idx[j];
            if (fi != -1)
            {
                rhs.row(fi) += c.transpose();
                if (fj == -1) // constrained endpoint contributes its target
                    rhs.row(fi) += w * to_eigen(x[j]).transpose();
            }
            if (fj != -1)
            {
                rhs.row(fj) -= c.transpose();
                if (fi == -1)
                    rhs.row(fj) += w * to_eigen(x[i]).transpose();
            }
        }

        if (n_free > 0)
        {
            const DenseMatrix solution = cache_->solver.solve(rhs);
            if (cache_->solver.info() != Eigen::Success ||
                !solution.allFinite())
            {
                // keep the last committed positions and the rest pose
                auto what = std::string{__func__} +
                            ": Failed to solve linear system.";
                throw SolverException(what);
            }

            for (auto v : mesh_.vertices())
            {
                const int fi = free_idx[v.idx()];
                if (fi != -1)
                    x[v.idx()] = Point(solution(fi, 0), solution(fi, 1),
                                       solution(fi, 2));
            }
        }

        // energy at the new state, again with optimal rotations
        auto rotated = fit_rotations(x);
        energies[iter + 1] = evaluate(x, rotated);
    }

    // commit only after every solve has succeeded
    for (auto v : mesh_.vertices())
        mesh_.position(v) = x[v.idx()];

    return energies;
}

} // namespace pmp
