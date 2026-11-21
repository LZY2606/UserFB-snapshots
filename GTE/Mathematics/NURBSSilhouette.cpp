// CPU silhouette (contour) extraction for NURBS surfaces under an
// orthographic camera.  See NURBSSilhouette.h for the documentation of the
// algorithm, the connection rules, the tolerances and the unresolved states.

#include <Mathematics/NURBSSilhouette.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>

using namespace gte;

namespace
{
    using V3 = Vector3<double>;

    double Dot3(V3 const& a, V3 const& b)
    {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    V3 Cross3(V3 const& a, V3 const& b)
    {
        return
        {
            a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]
        };
    }

    double Length3(V3 const& a)
    {
        return std::sqrt(Dot3(a, a));
    }

    bool IsFinite3(V3 const& a)
    {
        return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]);
    }

    // Scalar tensor-product Bernstein polynomial of degree (du,dv) on the
    // unit square.  Coefficient (i,j) is stored at i + (du+1)*j.
    struct Poly2
    {
        int32_t du = 0;
        int32_t dv = 0;
        std::vector<double> c;

        void Reset(int32_t inDU, int32_t inDV)
        {
            du = inDU;
            dv = inDV;
            c.assign(static_cast<size_t>(du + 1) * static_cast<size_t>(dv + 1), 0.0);
        }

        inline double& operator()(int32_t i, int32_t j)
        {
            return c[static_cast<size_t>(i) + static_cast<size_t>(du + 1) * static_cast<size_t>(j)];
        }

        inline double operator()(int32_t i, int32_t j) const
        {
            return c[static_cast<size_t>(i) + static_cast<size_t>(du + 1) * static_cast<size_t>(j)];
        }

        void Range(double& lo, double& hi) const
        {
            lo = hi = c[0];
            for (double v : c)
            {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
    };

    double Binomial(int32_t n, int32_t k)
    {
        double r = 1.0;
        for (int32_t i = 0; i < k; ++i)
        {
            r *= static_cast<double>(n - i);
            r /= static_cast<double>(i + 1);
        }
        return r;
    }

    Poly2 Mul(Poly2 const& A, Poly2 const& B)
    {
        Poly2 R;
        R.Reset(A.du + B.du, A.dv + B.dv);
        for (int32_t i = 0; i <= A.du; ++i)
        {
            for (int32_t j = 0; j <= A.dv; ++j)
            {
                double a = A(i, j);
                if (a == 0.0)
                {
                    continue;
                }
                double wa = a * Binomial(A.du, i) * Binomial(A.dv, j);
                for (int32_t k = 0; k <= B.du; ++k)
                {
                    for (int32_t l = 0; l <= B.dv; ++l)
                    {
                        double b = B(k, l);
                        if (b == 0.0)
                        {
                            continue;
                        }
                        double w = wa * b * Binomial(B.du, k) * Binomial(B.dv, l)
                            / (Binomial(R.du, i + k) * Binomial(R.dv, j + l));
                        R(i + k, j + l) += w;
                    }
                }
            }
        }
        return R;
    }

    // R += s * A; the degrees must agree.
    void AddScaled(Poly2& R, Poly2 const& A, double s)
    {
        for (size_t i = 0; i < R.c.size(); ++i)
        {
            R.c[i] += s * A.c[i];
        }
    }

    // Derivative with respect to the local u parameter (degree drops in u).
    Poly2 DerU(Poly2 const& A)
    {
        Poly2 R;
        R.Reset(A.du - 1, A.dv);
        for (int32_t i = 0; i <= R.du; ++i)
        {
            for (int32_t j = 0; j <= R.dv; ++j)
            {
                R(i, j) = static_cast<double>(A.du) * (A(i + 1, j) - A(i, j));
            }
        }
        return R;
    }

    Poly2 DerV(Poly2 const& A)
    {
        Poly2 R;
        R.Reset(A.du, A.dv - 1);
        for (int32_t i = 0; i <= R.du; ++i)
        {
            for (int32_t j = 0; j <= R.dv; ++j)
            {
                R(i, j) = static_cast<double>(A.dv) * (A(i, j + 1) - A(i, j));
            }
        }
        return R;
    }

    // 1D de Casteljau split of coefficients c[0..d] at s in [0,1].
    void Split1D(std::array<double, 12> const& c, int32_t d, double s,
        double* left, double* right)
    {
        std::array<double, 12> t = c;
        left[0] = t[0];
        right[d] = t[d];
        for (int32_t r = 1; r <= d; ++r)
        {
            for (int32_t i = 0; i <= d - r; ++i)
            {
                t[i] = (1.0 - s) * t[i] + s * t[i + 1];
            }
            left[r] = t[0];
            right[d - r] = t[d - r];
        }
    }

    // Restrict to the sub-interval [0,s] (keepFirst) or [s,1] in u.
    Poly2 SplitU(Poly2 const& A, double s, bool keepFirst)
    {
        Poly2 R;
        R.Reset(A.du, A.dv);
        for (int32_t j = 0; j <= A.dv; ++j)
        {
            std::array<double, 12> row{};
            std::array<double, 12> left{};
            std::array<double, 12> right{};
            for (int32_t i = 0; i <= A.du; ++i)
            {
                row[static_cast<size_t>(i)] = A(i, j);
            }
            Split1D(row, A.du, s, left.data(), right.data());
            for (int32_t i = 0; i <= A.du; ++i)
            {
                R(i, j) = (keepFirst ? left[static_cast<size_t>(i)] : right[static_cast<size_t>(i)]);
            }
        }
        return R;
    }

    Poly2 SplitV(Poly2 const& A, double s, bool keepFirst)
    {
        Poly2 R;
        R.Reset(A.du, A.dv);
        for (int32_t i = 0; i <= A.du; ++i)
        {
            std::array<double, 12> col{};
            std::array<double, 12> left{};
            std::array<double, 12> right{};
            for (int32_t j = 0; j <= A.dv; ++j)
            {
                col[static_cast<size_t>(j)] = A(i, j);
            }
            Split1D(col, A.dv, s, left.data(), right.data());
            for (int32_t j = 0; j <= A.dv; ++j)
            {
                R(i, j) = (keepFirst ? left[static_cast<size_t>(j)] : right[static_cast<size_t>(j)]);
            }
        }
        return R;
    }

    // ------------------------------------------------------------------
    // Surface data: Bezier elements extracted from the NURBS control net.
    // ------------------------------------------------------------------
    struct Element
    {
        double u0 = 0.0;
        double u1 = 0.0;
        double v0 = 0.0;
        double v1 = 0.0;
        // Bernstein forms on the element, local coordinates.
        Poly2 W;        // weight function
        Poly2 P[3];     // homogeneous position (w*S)
        Poly2 G;        // w^2 * f, same zero set as f
    };

    struct SurfaceData
    {
        NURBSSurface<3, double> const* surface = nullptr;
        int32_t du = 0;
        int32_t dv = 0;
        double umin = 0.0;
        double umax = 0.0;
        double vmin = 0.0;
        double vmax = 0.0;
        double span = 1.0;
        std::vector<Element> elements;
    };

    using HPoint = std::array<double, 4>;   // (w*x, w*y, w*z, w)

    // One knot insertion into a row-major net of size nu x nv (u index
    // fastest).  Inserts 'u' once; the knot vector T is updated.
    void InsertKnotU(std::vector<HPoint>& net, int32_t nu, int32_t nv,
        std::vector<double>& T, int32_t d, double u)
    {
        int32_t i = nu - 1;
        for (int32_t k = d; k < nu; ++k)
        {
            if (u < T[static_cast<size_t>(k) + 1])
            {
                i = k;
                break;
            }
        }
        std::vector<HPoint> out(static_cast<size_t>(nu + 1) * static_cast<size_t>(nv));
        for (int32_t j = 0; j < nv; ++j)
        {
            for (int32_t k = 0; k <= i - d; ++k)
            {
                out[static_cast<size_t>(k) + static_cast<size_t>(nu + 1) * j] =
                    net[static_cast<size_t>(k) + static_cast<size_t>(nu) * j];
            }
            for (int32_t k = i - d + 1; k <= i; ++k)
            {
                double alpha = (u - T[static_cast<size_t>(k)])
                    / (T[static_cast<size_t>(k) + d] - T[static_cast<size_t>(k)]);
                HPoint const& P0 = net[static_cast<size_t>(k - 1) + static_cast<size_t>(nu) * j];
                HPoint const& P1 = net[static_cast<size_t>(k) + static_cast<size_t>(nu) * j];
                HPoint q;
                for (int32_t c = 0; c < 4; ++c)
                {
                    q[static_cast<size_t>(c)] = alpha * P1[static_cast<size_t>(c)]
                        + (1.0 - alpha) * P0[static_cast<size_t>(c)];
                }
                out[static_cast<size_t>(k) + static_cast<size_t>(nu + 1) * j] = q;
            }
            for (int32_t k = i + 1; k <= nu; ++k)
            {
                out[static_cast<size_t>(k) + static_cast<size_t>(nu + 1) * j] =
                    net[static_cast<size_t>(k - 1) + static_cast<size_t>(nu) * j];
            }
        }
        T.insert(T.begin() + i + 1, u);
        net = std::move(out);
    }

    std::vector<HPoint> Transpose(std::vector<HPoint> const& net, int32_t n0, int32_t n1)
    {
        std::vector<HPoint> out(net.size());
        for (int32_t i = 0; i < n0; ++i)
        {
            for (int32_t j = 0; j < n1; ++j)
            {
                out[static_cast<size_t>(j) + static_cast<size_t>(n1) * i] =
                    net[static_cast<size_t>(i) + static_cast<size_t>(n0) * j];
            }
        }
        return out;
    }

    // Builds G = w*(Pu x Pv).D - wv*(Pu x P).D - wu*(P x Pv).D in Bernstein
    // form.  All derivatives are with respect to the local element
    // coordinates; the missing positive scale 1/((u1-u0)*(v1-v0)) does not
    // change the sign pattern used for exclusion.
    Poly2 BuildG(Poly2 const& W, Poly2 const* P, V3 const& D)
    {
        Poly2 Wu = DerU(W);
        Poly2 Wv = DerV(W);
        Poly2 Pu[3] = { DerU(P[0]), DerU(P[1]), DerU(P[2]) };
        Poly2 Pv[3] = { DerV(P[0]), DerV(P[1]), DerV(P[2]) };

        Poly2 G;
        G.Reset(3 * W.du - 1, 3 * W.dv - 1);
        for (int32_t c = 0; c < 3; ++c)
        {
            int32_t c1 = (c + 1) % 3;
            int32_t c2 = (c + 2) % 3;
            double dc = D[static_cast<size_t>(c)];
            if (dc == 0.0)
            {
                continue;
            }
            Poly2 t1 = Mul(W, Mul(Pu[c1], Pv[c2]));
            Poly2 t2 = Mul(W, Mul(Pu[c2], Pv[c1]));
            AddScaled(G, t1, dc);
            AddScaled(G, t2, -dc);
            Poly2 t3 = Mul(Wv, Mul(Pu[c1], P[c2]));
            Poly2 t4 = Mul(Wv, Mul(Pu[c2], P[c1]));
            AddScaled(G, t3, -dc);
            AddScaled(G, t4, dc);
            Poly2 t5 = Mul(Wu, Mul(P[c1], Pv[c2]));
            Poly2 t6 = Mul(Wu, Mul(P[c2], Pv[c1]));
            AddScaled(G, t5, -dc);
            AddScaled(G, t6, dc);
        }
        return G;
    }

    bool ExtractElements(NURBSSurface<3, double> const& s, V3 const& D, SurfaceData& sd)
    {
        BasisFunction<double> const& b0 = s.GetBasisFunction(0);
        BasisFunction<double> const& b1 = s.GetBasisFunction(1);
        int32_t du = b0.GetDegree();
        int32_t dv = b1.GetDegree();
        int32_t nu = s.GetNumControls(0);
        int32_t nv = s.GetNumControls(1);

        std::vector<HPoint> net(static_cast<size_t>(nu) * static_cast<size_t>(nv));
        Vector<3, double> const* controls = s.GetControls();
        double const* weights = s.GetWeights();
        for (int32_t k = 0; k < nu * nv; ++k)
        {
            double w = weights[k];
            net[static_cast<size_t>(k)] = { w * controls[k][0], w * controls[k][1],
                w * controls[k][2], w };
        }

        std::vector<double> Tu(b0.GetKnots(), b0.GetKnots() + b0.GetNumKnots());
        std::vector<double> Tv(b1.GetKnots(), b1.GetKnots() + b1.GetNumKnots());

        // Raise every interior knot to full multiplicity so the net splits
        // into Bezier elements.
        UniqueKnot<double> const* uk = b0.GetUniqueKnots();
        int32_t numUniqueU = b0.GetNumUniqueKnots();
        for (int32_t i = 1; i < numUniqueU - 1; ++i)
        {
            for (int32_t r = uk[i].multiplicity; r < du; ++r)
            {
                InsertKnotU(net, nu, nv, Tu, du, uk[i].t);
                ++nu;
            }
        }
        // Transpose, insert v knots, transpose back.
        net = Transpose(net, nu, nv);
        UniqueKnot<double> const* vk = b1.GetUniqueKnots();
        int32_t numUniqueV = b1.GetNumUniqueKnots();
        for (int32_t i = 1; i < numUniqueV - 1; ++i)
        {
            for (int32_t r = vk[i].multiplicity; r < dv; ++r)
            {
                InsertKnotU(net, nv, nu, Tv, dv, vk[i].t);
                ++nv;
            }
        }
        net = Transpose(net, nv, nu);

        sd.surface = &s;
        sd.du = du;
        sd.dv = dv;
        sd.umin = s.GetUMin();
        sd.umax = s.GetUMax();
        sd.vmin = s.GetVMin();
        sd.vmax = s.GetVMax();
        sd.span = std::max(sd.umax - sd.umin, sd.vmax - sd.vmin);
        sd.elements.clear();

        int32_t neu = numUniqueU - 1;
        int32_t nev = numUniqueV - 1;
        for (int32_t q = 0; q < nev; ++q)
        {
            for (int32_t p = 0; p < neu; ++p)
            {
                Element e;
                e.u0 = uk[p].t;
                e.u1 = uk[p + 1].t;
                e.v0 = vk[q].t;
                e.v1 = vk[q + 1].t;
                e.W.Reset(du, dv);
                for (int32_t c = 0; c < 3; ++c)
                {
                    e.P[c].Reset(du, dv);
                }
                for (int32_t j = 0; j <= dv; ++j)
                {
                    for (int32_t i = 0; i <= du; ++i)
                    {
                        HPoint const& h = net[static_cast<size_t>(p * du + i)
                            + static_cast<size_t>(nu) * static_cast<size_t>(q * dv + j)];
                        e.W(i, j) = h[3];
                        for (int32_t c = 0; c < 3; ++c)
                        {
                            e.P[c](i, j) = h[static_cast<size_t>(c)];
                        }
                    }
                }
                e.G = BuildG(e.W, e.P, D);
                sd.elements.push_back(std::move(e));
            }
        }
        return true;
    }

    // ------------------------------------------------------------------
    // Evaluation of f = (Su x Sv).D and its gradient on a surface.
    // ------------------------------------------------------------------
    struct FInfo
    {
        V3 X{ 0.0, 0.0, 0.0 };
        V3 N{ 0.0, 0.0, 0.0 };
        double f = 0.0;
        double fu = 0.0;
        double fv = 0.0;
    };

    FInfo EvalF(NURBSSurface<3, double> const& s, double u, double v, V3 const& D)
    {
        std::array<V3, 6> jet{};
        s.Evaluate(u, v, 2, jet.data());
        FInfo r;
        r.X = jet[0];
        r.N = Cross3(jet[1], jet[2]);
        r.f = Dot3(r.N, D);
        r.fu = Dot3(Cross3(jet[3], jet[2]) + Cross3(jet[1], jet[4]), D);
        r.fv = Dot3(Cross3(jet[4], jet[2]) + Cross3(jet[1], jet[5]), D);
        return r;
    }

    // ------------------------------------------------------------------
    // Shared context: options, budget, unresolved accumulation.
    // ------------------------------------------------------------------
    using Result = NURBSSilhouette::Result;
    using UnresolvedRegion = NURBSSilhouette::UnresolvedRegion;
    using UnresolvedReason = NURBSSilhouette::UnresolvedReason;

    struct Context
    {
        V3 D{ 0.0, 0.0, 1.0 };
        V3 A{ 1.0, 0.0, 0.0 };  // A, B: orthonormal, perpendicular to D
        V3 B{ 0.0, 1.0, 0.0 };
        NURBSSilhouette::Options options{};
        long long budget = 0;
        std::vector<UnresolvedRegion> unresolved;

        bool Spend()
        {
            if (budget <= 0)
            {
                return false;
            }
            --budget;
            return true;
        }

        void AddUnresolved(size_t surface, double u0, double u1, double v0, double v1,
            UnresolvedReason reason)
        {
            UnresolvedRegion r;
            r.surface = surface;
            r.umin = u0;
            r.umax = u1;
            r.vmin = v0;
            r.vmax = v1;
            r.reason = reason;
            unresolved.push_back(r);
        }
    };

    // Tolerance for |f| so that |Dot(N,D)|/|N| stays well inside the angle
    // tolerance.
    double FTol(FInfo const& r, double angleTolerance)
    {
        return 0.25 * angleTolerance * Length3(r.N);
    }

    // Newton projection onto f = 0.  Returns 1 on convergence, 0 on failure,
    // -1 when the gradient of f vanishes (singular zero set).
    int NewtonSilhouette(NURBSSurface<3, double> const& s, V3 const& D,
        double& u, double& v, double umin, double umax, double vmin, double vmax,
        double angleTolerance, int32_t maxIter)
    {
        for (int32_t iter = 0; iter < maxIter; ++iter)
        {
            FInfo r = EvalF(s, u, v, D);
            double g2 = r.fu * r.fu + r.fv * r.fv;
            if (std::fabs(r.f) <= FTol(r, angleTolerance))
            {
                return (g2 > 1.0e-22 ? 1 : -1);
            }
            if (g2 <= 1.0e-24)
            {
                return -1;
            }
            double du = r.f * r.fu / g2;
            double dv = r.f * r.fv / g2;
            u -= du;
            v -= dv;
            u = std::min(std::max(u, umin), umax);
            v = std::min(std::max(v, vmin), vmax);
        }
        FInfo r = EvalF(s, u, v, D);
        double g2 = r.fu * r.fu + r.fv * r.fv;
        if (std::fabs(r.f) <= FTol(r, angleTolerance))
        {
            return (g2 > 1.0e-22 ? 1 : -1);
        }
        return 0;
    }

    // ------------------------------------------------------------------
    // Seed extraction: Bernstein exclusion quadtree over the knot elements.
    // ------------------------------------------------------------------
    struct Seed
    {
        double u = 0.0;
        double v = 0.0;
        bool consumed = false;
    };

    struct Region
    {
        Poly2 g;
        double u0 = 0.0;
        double u1 = 0.0;
        double v0 = 0.0;
        double v1 = 0.0;
    };

    double ScaledDist(SurfaceData const& sd, double u0, double v0, double u1, double v1)
    {
        double du = (u0 - u1) / sd.span;
        double dv = (v0 - v1) / sd.span;
        return std::sqrt(du * du + dv * dv);
    }

    void ExtractSeeds(Context& ctx, size_t si, SurfaceData const& sd, std::vector<Seed>& seeds)
    {
        double seedSize = sd.span / 64.0;
        double minSize = std::max(8.0 * ctx.options.parameterTolerance, sd.span / 16384.0);

        std::vector<Region> stack;
        for (Element const& e : sd.elements)
        {
            Region r;
            r.g = e.G;
            r.u0 = e.u0;
            r.u1 = e.u1;
            r.v0 = e.v0;
            r.v1 = e.v1;
            stack.push_back(std::move(r));
        }

        while (!stack.empty())
        {
            if (!ctx.Spend())
            {
                for (Region const& r : stack)
                {
                    ctx.AddUnresolved(si, r.u0, r.u1, r.v0, r.v1, UnresolvedReason::BudgetExhausted);
                }
                return;
            }
            Region r = std::move(stack.back());
            stack.pop_back();

            double lo, hi;
            r.g.Range(lo, hi);
            if (lo > 0.0 || hi < 0.0)
            {
                continue;   // provably no zero in this region
            }

            double size = std::max(r.u1 - r.u0, r.v1 - r.v0);
            double cu = 0.5 * (r.u0 + r.u1);
            double cv = 0.5 * (r.v0 + r.v1);

            // A vanishing zero-set gradient at a zero means the silhouette is
            // not an isolated regular curve here; report instead of guessing.
            FInfo rc = EvalF(*sd.surface, cu, cv, ctx.D);
            double gradLen = std::sqrt(rc.fu * rc.fu + rc.fv * rc.fv);
            double nLen = Length3(rc.N);
            if (nLen <= 1.0e-12 && std::fabs(rc.f) <= 1.0e-9)
            {
                ctx.AddUnresolved(si, r.u0, r.u1, r.v0, r.v1, UnresolvedReason::SurfaceDegenerate);
                continue;
            }
            if (gradLen <= 1.0e-11 && std::fabs(rc.f) <= 1.0e-9 * (1.0 + nLen))
            {
                ctx.AddUnresolved(si, r.u0, r.u1, r.v0, r.v1, UnresolvedReason::NonIsolatedZeroSet);
                continue;
            }

            if (size <= seedSize)
            {
                double su = cu;
                double sv = cv;
                int status = NewtonSilhouette(*sd.surface, ctx.D, su, sv,
                    sd.umin, sd.umax, sd.vmin, sd.vmax, ctx.options.angleTolerance, 40);
                if (status == 1)
                {
                    // Record the seed unless an equivalent one exists.
                    bool duplicate = false;
                    for (Seed const& s : seeds)
                    {
                        if (ScaledDist(sd, s.u, s.v, su, sv) * sd.span <= seedSize)
                        {
                            duplicate = true;
                            break;
                        }
                    }
                    if (!duplicate)
                    {
                        seeds.push_back({ su, sv, false });
                    }
                    continue;
                }
                if (status == -1)
                {
                    ctx.AddUnresolved(si, r.u0, r.u1, r.v0, r.v1,
                        UnresolvedReason::ZeroSetGradientVanishes);
                    continue;
                }
                if (size <= minSize)
                {
                    ctx.AddUnresolved(si, r.u0, r.u1, r.v0, r.v1,
                        UnresolvedReason::ZeroSetGradientVanishes);
                    continue;
                }
                // Newton did not converge yet; keep refining.
            }

            if (size <= minSize)
            {
                ctx.AddUnresolved(si, r.u0, r.u1, r.v0, r.v1,
                    UnresolvedReason::ZeroSetGradientVanishes);
                continue;
            }

            // Subdivide into four children with exact Bernstein coefficients.
            double um = 0.5 * (r.u0 + r.u1);
            double vm = 0.5 * (r.v0 + r.v1);
            Poly2 gLo = SplitU(r.g, 0.5, true);
            Poly2 gHi = SplitU(r.g, 0.5, false);
            Poly2 g00 = SplitV(gLo, 0.5, true);
            Poly2 g01 = SplitV(gLo, 0.5, false);
            Poly2 g10 = SplitV(gHi, 0.5, true);
            Poly2 g11 = SplitV(gHi, 0.5, false);
            stack.push_back({ std::move(g00), r.u0, um, r.v0, vm });
            stack.push_back({ std::move(g01), r.u0, um, vm, r.v1 });
            stack.push_back({ std::move(g10), um, r.u1, r.v0, vm });
            stack.push_back({ std::move(g11), um, r.u1, vm, r.v1 });
        }
    }

    // ------------------------------------------------------------------
    // Branch tracing.
    // ------------------------------------------------------------------
    struct TracePoint
    {
        double u = 0.0;
        double v = 0.0;
        V3 X{ 0.0, 0.0, 0.0 };
        V3 T{ 0.0, 0.0, 0.0 };
    };

    enum class EndReason
    {
        Boundary,
        Closed,
        Singular,
        StepLimit
    };

    struct TraceResult
    {
        std::vector<TracePoint> points; // excludes the seed, includes the end point
        EndReason reason = EndReason::StepLimit;
        double endU = 0.0;
        double endV = 0.0;
    };

    TracePoint MakePoint(NURBSSurface<3, double> const& s, V3 const& D, double u, double v)
    {
        FInfo r = EvalF(s, u, v, D);
        TracePoint p;
        p.u = u;
        p.v = v;
        p.X = r.X;
        // Map the parametric direction (-fv, fu) through the derivative.
        V3 T;
        std::array<V3, 6> jet{};
        s.Evaluate(u, v, 1, jet.data());
        T = (-r.fv) * jet[1] + r.fu * jet[2];
        double len = Length3(T);
        if (len > 0.0)
        {
            T = (1.0 / len) * T;
        }
        p.T = T;
        return p;
    }

    TraceResult TraceDirection(Context& ctx, SurfaceData const& sd,
        double seedU, double seedV, double dirSign)
    {
        NURBSSurface<3, double> const& s = *sd.surface;
        double hmax = sd.span / 128.0;
        double hmin = std::max(4.0 * ctx.options.parameterTolerance, sd.span * 1.0e-9);
        double h = sd.span / 512.0;
        int32_t const maxSteps = 100000;

        TraceResult out;
        double pu = seedU;
        double pv = seedV;
        double arc = 0.0;

        FInfo rp = EvalF(s, pu, pv, ctx.D);

        for (int32_t step = 0; step < maxSteps; ++step)
        {
            double gradLen = std::sqrt(rp.fu * rp.fu + rp.fv * rp.fv);
            if (gradLen <= 1.0e-11)
            {
                out.reason = EndReason::Singular;
                out.endU = pu;
                out.endV = pv;
                return out;
            }
            double tux = -rp.fv / gradLen * dirSign;
            double tvx = rp.fu / gradLen * dirSign;

            // Predictor-corrector with adaptive step.
            double qu = 0.0;
            double qv = 0.0;
            bool accepted = false;
            double hTry = h;
            for (int32_t attempt = 0; attempt < 24; ++attempt)
            {
                qu = pu + hTry * tux;
                qv = pv + hTry * tvx;
                // Correct back onto the zero set.
                int status = NewtonSilhouette(s, ctx.D, qu, qv,
                    sd.umin - sd.span, sd.umax + sd.span, sd.vmin - sd.span, sd.vmax + sd.span,
                    ctx.options.angleTolerance, 8);
                if (status == 1)
                {
                    double du = qu - pu;
                    double dv = qv - pv;
                    double moved = std::sqrt(du * du + dv * dv);
                    if (moved <= 1.75 * hTry)
                    {
                        FInfo rq = EvalF(s, qu, qv, ctx.D);
                        double gl2 = std::sqrt(rq.fu * rq.fu + rq.fv * rq.fv);
                        if (gl2 > 1.0e-11)
                        {
                            double dot = ((-rq.fv) * tux + rq.fu * tvx) / gl2 * dirSign;
                            if (dot >= 0.4)
                            {
                                accepted = true;
                                break;
                            }
                        }
                    }
                }
                else if (status == -1)
                {
                    out.reason = EndReason::Singular;
                    out.endU = qu;
                    out.endV = qv;
                    return out;
                }
                hTry *= 0.5;
                if (hTry < hmin)
                {
                    break;
                }
            }

            if (!accepted)
            {
                out.reason = EndReason::Singular;
                out.endU = pu;
                out.endV = pv;
                return out;
            }

            bool outside = (qu < sd.umin || qu > sd.umax || qv < sd.vmin || qv > sd.vmax);
            if (outside)
            {
                // Bisect between p (inside) and q (outside) for the boundary
                // crossing, projecting the iterates back onto the zero set.
                double lu = pu;
                double lv = pv;
                double hu = qu;
                double hv = qv;
                for (int32_t it = 0; it < 40; ++it)
                {
                    double mu = 0.5 * (lu + hu);
                    double mv = 0.5 * (lv + hv);
                    bool in = (mu >= sd.umin && mu <= sd.umax && mv >= sd.vmin && mv <= sd.vmax);
                    if (in)
                    {
                        double cu = mu;
                        double cv = mv;
                        int status = NewtonSilhouette(s, ctx.D, cu, cv,
                            sd.umin, sd.umax, sd.vmin, sd.vmax, ctx.options.angleTolerance, 8);
                        if (status == 1)
                        {
                            lu = cu;
                            lv = cv;
                        }
                        else
                        {
                            hu = mu;
                            hv = mv;
                        }
                    }
                    else
                    {
                        hu = mu;
                        hv = mv;
                    }
                    double w = std::sqrt((hu - lu) * (hu - lu) + (hv - lv) * (hv - lv));
                    if (w <= 4.0 * ctx.options.parameterTolerance)
                    {
                        break;
                    }
                }
                double bu = std::min(std::max(lu, sd.umin), sd.umax);
                double bv = std::min(std::max(lv, sd.vmin), sd.vmax);
                out.points.push_back(MakePoint(s, ctx.D, bu, bv));
                out.reason = EndReason::Boundary;
                out.endU = bu;
                out.endV = bv;
                return out;
            }

            // Closed-loop detection: returned to the seed.
            double du0 = qu - seedU;
            double dv0 = qv - seedV;
            double distSeed = std::sqrt(du0 * du0 + dv0 * dv0);
            double stepLen = std::sqrt((qu - pu) * (qu - pu) + (qv - pv) * (qv - pv));
            arc += stepLen;
            if (step >= 8 && distSeed <= 0.8 * hTry && arc >= 8.0 * hTry)
            {
                out.reason = EndReason::Closed;
                out.endU = qu;
                out.endV = qv;
                return out;
            }

            out.points.push_back(MakePoint(s, ctx.D, qu, qv));
            pu = qu;
            pv = qv;
            rp = EvalF(s, pu, pv, ctx.D);
            h = std::min(hTry * 1.4, hmax);
        }

        out.reason = EndReason::StepLimit;
        out.endU = pu;
        out.endV = pv;
        return out;
    }

    // ------------------------------------------------------------------
    // Ray casting for occlusion.
    // ------------------------------------------------------------------
    enum class RayState
    {
        Clear,
        Occluded,
        Unknown
    };

    struct RayRegion
    {
        Poly2 a;
        Poly2 b;
        Poly2 tnum;
        Poly2 w;
        double u0 = 0.0;
        double u1 = 0.0;
        double v0 = 0.0;
        double v1 = 0.0;
    };

    // Minimize q(u,v) = (A.(S-X))^2 + (B.(S-X))^2, the squared distance
    // from the ray line to the surface, over a parameter cell.  Uses a 3x3
    // grid start plus Gauss-Newton iterations clamped to the cell.  Returns
    // the minimum found and the minimizing parameters.
    double RayMissDistance(Context& ctx, SurfaceData const& sd, V3 const& X,
        double u0, double u1, double v0, double v1, double& qu, double& qv)
    {
        NURBSSurface<3, double> const& s = *sd.surface;
        double bq = std::numeric_limits<double>::max();
        double bu = 0.5 * (u0 + u1);
        double bv = 0.5 * (v0 + v1);
        for (int32_t i = 0; i <= 2; ++i)
        {
            for (int32_t j = 0; j <= 2; ++j)
            {
                double u = u0 + (u1 - u0) * (0.5 * i);
                double v = v0 + (v1 - v0) * (0.5 * j);
                V3 d = s.GetPosition(u, v) - X;
                double a = Dot3(ctx.A, d);
                double b = Dot3(ctx.B, d);
                double q = a * a + b * b;
                if (q < bq)
                {
                    bq = q;
                    bu = u;
                    bv = v;
                }
            }
        }
        double u = bu;
        double v = bv;
        for (int32_t iter = 0; iter < 16; ++iter)
        {
            std::array<V3, 6> jet{};
            s.Evaluate(u, v, 1, jet.data());
            V3 d = jet[0] - X;
            double a = Dot3(ctx.A, d);
            double b = Dot3(ctx.B, d);
            double q = a * a + b * b;
            if (q < bq)
            {
                bq = q;
                bu = u;
                bv = v;
            }
            double j00 = Dot3(ctx.A, jet[1]);
            double j01 = Dot3(ctx.A, jet[2]);
            double j10 = Dot3(ctx.B, jet[1]);
            double j11 = Dot3(ctx.B, jet[2]);
            double det = j00 * j11 - j01 * j10;
            if (std::fabs(det) <= 1.0e-18)
            {
                break;
            }
            double du = (j11 * a - j01 * b) / det;
            double dv = (-j10 * a + j00 * b) / det;
            u -= du;
            v -= dv;
            u = std::min(std::max(u, u0), u1);
            v = std::min(std::max(v, v0), v1);
        }
        std::array<V3, 6> jet{};
        s.Evaluate(u, v, 1, jet.data());
        V3 d = jet[0] - X;
        double a = Dot3(ctx.A, d);
        double b = Dot3(ctx.B, d);
        double q = a * a + b * b;
        if (q < bq)
        {
            bq = q;
            bu = u;
            bv = v;
        }
        qu = bu;
        qv = bv;
        return bq;
    }

    // Classify one ray against one surface.  Returns Clear when no
    // intersection beyond the spatial tolerance exists, Occluded when one is
    // found, Unknown when a region can neither be excluded nor resolved.
    RayState RayVsSurface(Context& ctx, SurfaceData const& sd, V3 const& X)
    {
        double spatialTol = ctx.options.spatialTolerance;
        double raySeed = sd.span / 32.0;
        double minSize = std::max(8.0 * ctx.options.parameterTolerance, sd.span / 16384.0);
        // Miss-distance threshold for declaring a ray-surface intersection.
        // A true intersection has zero miss distance; near a silhouette fold
        // the surface stays within distance ~k*t of its own tangent ray, so
        // the threshold must be far below k*spatialTolerance to avoid false
        // occlusions from the fold.
        double rootTol = std::min(1.0e-13, spatialTol * 1.0e-3);

        std::vector<RayRegion> stack;
        for (Element const& e : sd.elements)
        {
            RayRegion r;
            r.u0 = e.u0;
            r.u1 = e.u1;
            r.v0 = e.v0;
            r.v1 = e.v1;
            r.a.Reset(sd.du, sd.dv);
            r.b.Reset(sd.du, sd.dv);
            r.tnum.Reset(sd.du, sd.dv);
            r.w = e.W;
            double ax = Dot3(ctx.A, X);
            double bx = Dot3(ctx.B, X);
            double dx = Dot3(ctx.D, X);
            for (int32_t j = 0; j <= sd.dv; ++j)
            {
                for (int32_t i = 0; i <= sd.du; ++i)
                {
                    double px = e.P[0](i, j);
                    double py = e.P[1](i, j);
                    double pz = e.P[2](i, j);
                    double w = e.W(i, j);
                    r.a(i, j) = ctx.A[0] * px + ctx.A[1] * py + ctx.A[2] * pz - ax * w;
                    r.b(i, j) = ctx.B[0] * px + ctx.B[1] * py + ctx.B[2] * pz - bx * w;
                    r.tnum(i, j) = ctx.D[0] * px + ctx.D[1] * py + ctx.D[2] * pz - dx * w;
                }
            }
            stack.push_back(std::move(r));
        }

        bool unknown = false;
        while (!stack.empty())
        {
            if (!ctx.Spend())
            {
                return RayState::Unknown;
            }
            RayRegion r = std::move(stack.back());
            stack.pop_back();

            double lo, hi;
            r.a.Range(lo, hi);
            if (lo > 0.0 || hi < 0.0)
            {
                continue;
            }
            r.b.Range(lo, hi);
            if (lo > 0.0 || hi < 0.0)
            {
                continue;
            }
            // Conservative range of t = tnum/w on the region (w > 0).
            double tlo, thi, wlo, whi;
            r.tnum.Range(tlo, thi);
            r.w.Range(wlo, whi);
            if (wlo <= 0.0)
            {
                wlo = 1.0e-300;
            }
            double tHi = (thi >= 0.0 ? thi / wlo : thi / whi);
            if (tHi <= spatialTol)
            {
                continue;   // any intersection here is at or behind the origin
            }

            double size = std::max(r.u1 - r.u0, r.v1 - r.v0);
            if (size <= raySeed)
            {
                // Degenerate case: a and b vanish over the whole cell, the
                // ray lies inside the surface (non-isolated intersection).
                double alo, ahi, blo, bhi;
                r.a.Range(alo, ahi);
                r.b.Range(blo, bhi);
                double aamp = std::max(std::fabs(alo), std::fabs(ahi));
                double bamp = std::max(std::fabs(blo), std::fabs(bhi));
                if (aamp <= rootTol && bamp <= rootTol)
                {
                    unknown = true;
                    continue;
                }

                // Resolve the cell by its miss distance: a common zero of
                // (a,b) is an intersection, a positive minimum is a proof
                // that the ray misses the surface here.
                double qu = 0.0;
                double qv = 0.0;
                double qmin = RayMissDistance(ctx, sd, X, r.u0, r.u1, r.v0, r.v1, qu, qv);
                if (qmin <= rootTol * rootTol)
                {
                    V3 P = sd.surface->GetPosition(qu, qv);
                    double t = Dot3(ctx.D, P - X);
                    if (t > spatialTol)
                    {
                        return RayState::Occluded;
                    }
                }
                continue;
            }

            if (size <= minSize)
            {
                unknown = true;
                continue;
            }

            double um = 0.5 * (r.u0 + r.u1);
            double vm = 0.5 * (r.v0 + r.v1);
            Poly2 aLo = SplitU(r.a, 0.5, true);
            Poly2 aHi = SplitU(r.a, 0.5, false);
            Poly2 bLo = SplitU(r.b, 0.5, true);
            Poly2 bHi = SplitU(r.b, 0.5, false);
            Poly2 tLo = SplitU(r.tnum, 0.5, true);
            Poly2 tHi2 = SplitU(r.tnum, 0.5, false);
            Poly2 wLo = SplitU(r.w, 0.5, true);
            Poly2 wHi = SplitU(r.w, 0.5, false);

            RayRegion c00{ SplitV(aLo, 0.5, true), SplitV(bLo, 0.5, true),
                SplitV(tLo, 0.5, true), SplitV(wLo, 0.5, true), r.u0, um, r.v0, vm };
            RayRegion c01{ SplitV(aLo, 0.5, false), SplitV(bLo, 0.5, false),
                SplitV(tLo, 0.5, false), SplitV(wLo, 0.5, false), r.u0, um, vm, r.v1 };
            RayRegion c10{ SplitV(aHi, 0.5, true), SplitV(bHi, 0.5, true),
                SplitV(tHi2, 0.5, true), SplitV(wHi, 0.5, true), um, r.u1, r.v0, vm };
            RayRegion c11{ SplitV(aHi, 0.5, false), SplitV(bHi, 0.5, false),
                SplitV(tHi2, 0.5, false), SplitV(wHi, 0.5, false), um, r.u1, vm, r.v1 };
            stack.push_back(std::move(c00));
            stack.push_back(std::move(c01));
            stack.push_back(std::move(c10));
            stack.push_back(std::move(c11));
        }
        return (unknown ? RayState::Unknown : RayState::Clear);
    }

    RayState RayOccluded(Context& ctx, std::vector<SurfaceData> const& sds, V3 const& X)
    {
        bool unknown = false;
        for (SurfaceData const& sd : sds)
        {
            RayState st = RayVsSurface(ctx, sd, X);
            if (st == RayState::Occluded)
            {
                return RayState::Occluded;
            }
            if (st == RayState::Unknown)
            {
                unknown = true;
            }
        }
        return (unknown ? RayState::Unknown : RayState::Clear);
    }

    // ------------------------------------------------------------------
    // Visibility along a traced curve.
    // ------------------------------------------------------------------
    using Curve = NURBSSilhouette::Curve;
    using IntervalState = NURBSSilhouette::IntervalState;
    using VisibilityInterval = NURBSSilhouette::VisibilityInterval;

    IntervalState ToIntervalState(RayState rs)
    {
        if (rs == RayState::Occluded)
        {
            return IntervalState::Hidden;
        }
        if (rs == RayState::Unknown)
        {
            return IntervalState::Unknown;
        }
        return IntervalState::Visible;
    }

    // Map a curve parameter t to a surface point: interpolate UV between the
    // bracketing samples and project back onto the zero set of f.  Returns
    // false when the projection fails.
    bool CurvePoint(Context& ctx, Curve const& curve, SurfaceData const& sd, double t,
        double& u, double& v, V3& X)
    {
        size_t n = curve.samples.size();
        size_t seg = curve.closed ? n : n - 1;
        size_t k = 0;
        while (k + 1 < seg && curve.parameter[k + 1] < t)
        {
            ++k;
        }
        double ta = curve.parameter[k];
        double tb = (k + 1 < n ? curve.parameter[k + 1] : 1.0);
        double lambda = (tb > ta ? (t - ta) / (tb - ta) : 0.0);
        lambda = std::min(std::max(lambda, 0.0), 1.0);
        auto const& s0 = curve.samples[k];
        auto const& s1 = curve.samples[(k + 1) % n];
        u = s0.u + lambda * (s1.u - s0.u);
        v = s0.v + lambda * (s1.v - s0.v);
        int status = NewtonSilhouette(*sd.surface, ctx.D, u, v,
            sd.umin, sd.umax, sd.vmin, sd.vmax, ctx.options.angleTolerance, 12);
        if (status != 1)
        {
            return false;
        }
        X = sd.surface->GetPosition(u, v);
        return true;
    }

    void ComputeVisibility(Context& ctx, std::vector<SurfaceData> const& sds, Curve& curve)
    {
        size_t n = curve.samples.size();
        if (n == 0)
        {
            return;
        }
        SurfaceData const& sd = sds[curve.surface];
        size_t seg = curve.closed ? n : n - 1;

        // Total chord length, used to stop the boundary bisection.
        double total = 0.0;
        for (size_t i = 0; i < seg; ++i)
        {
            total += Length3(curve.samples[(i + 1) % n].position - curve.samples[i].position);
        }
        if (total <= 0.0)
        {
            total = 1.0;
        }
        double boundaryTol = std::max(1.0e-4, 16.0 * ctx.options.spatialTolerance) / total;

        struct Piece
        {
            double t0;
            double t1;
            IntervalState state;
        };
        std::vector<Piece> pieces;

        auto stateAt = [&](double t, IntervalState& st, double& u, double& v)
        {
            V3 X;
            if (!CurvePoint(ctx, curve, sd, t, u, v, X))
            {
                st = IntervalState::Unknown;
                return;
            }
            st = ToIntervalState(RayOccluded(ctx, sds, X));
        };

        auto markUnresolved = [&](double t, UnresolvedReason reason)
        {
            double u = 0.0;
            double v = 0.0;
            V3 X{};
            double h = sd.span / 256.0;
            if (CurvePoint(ctx, curve, sd, t, u, v, X))
            {
                ctx.AddUnresolved(curve.surface, u - h, u + h, v - h, v + h, reason);
            }
            else
            {
                ctx.AddUnresolved(curve.surface, sd.umin, sd.umax, sd.vmin, sd.vmax, reason);
            }
        };

        // Recursive segment processing: never trusts the endpoints alone,
        // the midpoint is examined and transitions are bisected.
        std::function<void(double, IntervalState, double, IntervalState, int32_t)> process;
        process = [&](double t0, IntervalState s0, double t1, IntervalState s1, int32_t depth)
        {
            if (s0 != s1)
            {
                if (s0 == IntervalState::Unknown || s1 == IntervalState::Unknown)
                {
                    double tm = 0.5 * (t0 + t1);
                    pieces.push_back({ t0, tm, s0 });
                    pieces.push_back({ tm, t1, s1 });
                    return;
                }
                // Bisect to locate the occlusion state change.
                double lo = t0;
                double hi = t1;
                bool unknown = false;
                for (int32_t it = 0; it < 40; ++it)
                {
                    double tm = 0.5 * (lo + hi);
                    IntervalState sm = IntervalState::Unknown;
                    double u = 0.0;
                    double v = 0.0;
                    stateAt(tm, sm, u, v);
                    if (sm == IntervalState::Unknown)
                    {
                        unknown = true;
                        break;
                    }
                    if (sm == s0)
                    {
                        lo = tm;
                    }
                    else
                    {
                        hi = tm;
                    }
                    if (hi - lo <= boundaryTol)
                    {
                        break;
                    }
                }
                if (unknown)
                {
                    pieces.push_back({ t0, lo, s0 });
                    pieces.push_back({ lo, hi, IntervalState::Unknown });
                    pieces.push_back({ hi, t1, s1 });
                    markUnresolved(0.5 * (lo + hi), UnresolvedReason::OcclusionBoundaryUnresolved);
                    return;
                }
                double tb = 0.5 * (lo + hi);
                pieces.push_back({ t0, tb, s0 });
                pieces.push_back({ tb, t1, s1 });
                return;
            }

            double tm = 0.5 * (t0 + t1);
            IntervalState sm = IntervalState::Unknown;
            double u = 0.0;
            double v = 0.0;
            stateAt(tm, sm, u, v);
            if (sm == s0)
            {
                pieces.push_back({ t0, t1, s0 });
                return;
            }
            if (depth >= 24)
            {
                pieces.push_back({ t0, t1, IntervalState::Unknown });
                markUnresolved(tm, UnresolvedReason::RayUnresolved);
                return;
            }
            if (sm == IntervalState::Unknown)
            {
                markUnresolved(tm, UnresolvedReason::RayUnresolved);
            }
            process(t0, s0, tm, sm, depth + 1);
            process(tm, sm, t1, s1, depth + 1);
        };

        // Occlusion state at the samples (using the stored positions).
        std::vector<IntervalState> sampleState(n);
        for (size_t i = 0; i < n; ++i)
        {
            sampleState[i] = ToIntervalState(RayOccluded(ctx, sds, curve.samples[i].position));
            if (sampleState[i] == IntervalState::Unknown)
            {
                double h = sd.span / 256.0;
                ctx.AddUnresolved(curve.surface,
                    curve.samples[i].u - h, curve.samples[i].u + h,
                    curve.samples[i].v - h, curve.samples[i].v + h,
                    UnresolvedReason::RayUnresolved);
            }
        }

        for (size_t k = 0; k < seg; ++k)
        {
            double ta = curve.parameter[k];
            double tb = (k + 1 < n ? curve.parameter[k + 1] : 1.0);
            IntervalState sa = sampleState[k];
            IntervalState sb = sampleState[(k + 1) % n];
            process(ta, sa, tb, sb, 0);
        }

        // Merge adjacent pieces with equal state.
        std::vector<VisibilityInterval> merged;
        for (Piece const& p : pieces)
        {
            double t0 = std::min(std::max(p.t0, 0.0), 1.0);
            double t1 = std::min(std::max(p.t1, 0.0), 1.0);
            if (!merged.empty() && merged.back().state == p.state
                && std::fabs(merged.back().t1 - t0) <= 1.0e-9)
            {
                merged.back().t1 = t1;
            }
            else
            {
                merged.push_back({ t0, t1, p.state });
            }
        }

        // On a closed curve, merge across the t=1/t=0 seam; the merged
        // interval wraps (t0 > t1) and is reported first.
        if (curve.closed && merged.size() >= 2 && merged.front().state == merged.back().state)
        {
            VisibilityInterval wrap{ merged.back().t0, merged.front().t1, merged.front().state };
            merged.erase(merged.begin());
            merged.pop_back();
            merged.insert(merged.begin(), wrap);
        }

        curve.intervals = std::move(merged);
    }
}

namespace gte
{
    NURBSSilhouette::NURBSSilhouette(
        std::vector<std::shared_ptr<NURBSSurface<3, double>>> surfaces,
        Vector3<double> const& viewDirection,
        Options const& options)
        :
        mSurfaces(std::move(surfaces)),
        mDirection(viewDirection),
        mOptions(options),
        mValid(false)
    {
        // Validate the camera.
        if (!IsFinite3(mDirection))
        {
            mError = "view direction is not finite";
            return;
        }
        double dLen = Length3(mDirection);
        if (std::fabs(dLen - 1.0) > 1.0e-6)
        {
            mError = "view direction is not a unit vector";
            return;
        }

        // Validate the tolerances.
        if (!std::isfinite(mOptions.spatialTolerance) || mOptions.spatialTolerance <= 0.0 ||
            !std::isfinite(mOptions.parameterTolerance) || mOptions.parameterTolerance <= 0.0 ||
            !std::isfinite(mOptions.angleTolerance) || mOptions.angleTolerance <= 0.0 ||
            mOptions.angleTolerance >= 0.5 ||
            mOptions.regionBudget == 0)
        {
            mError = "invalid tolerance or budget options";
            return;
        }

        // Validate the surfaces.
        for (size_t s = 0; s < mSurfaces.size(); ++s)
        {
            auto const& sp = mSurfaces[s];
            if (!sp || !(*sp))
            {
                mError = "surface is not constructed";
                return;
            }
            for (int32_t dim = 0; dim < 2; ++dim)
            {
                BasisFunction<double> const& b = sp->GetBasisFunction(dim);
                int32_t degree = b.GetDegree();
                if (degree != 2 && degree != 3)
                {
                    mError = "surface degree is not 2 or 3";
                    return;
                }
                if (b.IsPeriodic())
                {
                    mError = "periodic knot vectors are not supported";
                    return;
                }
                if (!b.IsOpen())
                {
                    mError = "knot vector is not clamped";
                    return;
                }
                UniqueKnot<double> const* uk = b.GetUniqueKnots();
                int32_t numUnique = b.GetNumUniqueKnots();
                for (int32_t i = 1; i < numUnique - 1; ++i)
                {
                    if (uk[i].multiplicity < 1 || uk[i].multiplicity > degree - 1)
                    {
                        mError = "interior knot multiplicity violates the C1 requirement";
                        return;
                    }
                }
            }
            double const* weights = sp->GetWeights();
            Vector<3, double> const* controls = sp->GetControls();
            int32_t numControls = sp->GetNumControls(0) * sp->GetNumControls(1);
            for (int32_t k = 0; k < numControls; ++k)
            {
                if (!std::isfinite(weights[k]) || weights[k] <= 0.0)
                {
                    mError = "weights must be positive and finite";
                    return;
                }
                if (!IsFinite3(controls[k]))
                {
                    mError = "control points must be finite";
                    return;
                }
            }
        }

        mValid = true;
        mError.clear();
    }

    bool NURBSSilhouette::Compute(Result& output) const
    {
        if (!mValid)
        {
            return false;
        }

        Context ctx;
        ctx.D = mDirection;
        // Orthonormal frame perpendicular to D for ray queries.
        V3 ref = (std::fabs(mDirection[2]) < 0.9 ? V3{ 0.0, 0.0, 1.0 } : V3{ 1.0, 0.0, 0.0 });
        ctx.A = Cross3(mDirection, ref);
        ctx.A = (1.0 / Length3(ctx.A)) * ctx.A;
        ctx.B = Cross3(mDirection, ctx.A);
        ctx.options = mOptions;
        ctx.budget = static_cast<long long>(mOptions.regionBudget);

        // Per-surface Bezier data.
        std::vector<SurfaceData> sds(mSurfaces.size());
        for (size_t si = 0; si < mSurfaces.size(); ++si)
        {
            ExtractElements(*mSurfaces[si], ctx.D, sds[si]);
        }

        // Seed extraction on the true parametric zero set.
        std::vector<std::vector<Seed>> seeds(mSurfaces.size());
        for (size_t si = 0; si < mSurfaces.size(); ++si)
        {
            ExtractSeeds(ctx, si, sds[si], seeds[si]);
        }

        // Trace branches from the seeds.
        Result result;
        result.complete = true;
        for (size_t si = 0; si < mSurfaces.size(); ++si)
        {
            SurfaceData const& sd = sds[si];
            NURBSSurface<3, double> const& s = *mSurfaces[si];
            double consumeTol = sd.span / 64.0;

            for (Seed& seed : seeds[si])
            {
                if (seed.consumed)
                {
                    continue;
                }
                seed.consumed = true;

                TracePoint seedPoint = MakePoint(s, ctx.D, seed.u, seed.v);
                TraceResult fwd = TraceDirection(ctx, sd, seed.u, seed.v, +1.0);
                Curve curve;
                curve.surface = si;
                if (fwd.reason == EndReason::Closed)
                {
                    curve.closed = true;
                    curve.samples.reserve(fwd.points.size() + 1);
                    curve.samples.push_back({ si, seed.u, seed.v, seedPoint.X, seedPoint.T });
                    for (TracePoint const& p : fwd.points)
                    {
                        curve.samples.push_back({ si, p.u, p.v, p.X, p.T });
                    }
                }
                else
                {
                    TraceResult bwd = TraceDirection(ctx, sd, seed.u, seed.v, -1.0);
                    curve.closed = false;
                    curve.samples.reserve(bwd.points.size() + fwd.points.size() + 1);
                    for (auto it = bwd.points.rbegin(); it != bwd.points.rend(); ++it)
                    {
                        curve.samples.push_back({ si, it->u, it->v, it->X, it->T });
                    }
                    curve.samples.push_back({ si, seed.u, seed.v, seedPoint.X, seedPoint.T });
                    for (TracePoint const& p : fwd.points)
                    {
                        curve.samples.push_back({ si, p.u, p.v, p.X, p.T });
                    }
                    if (bwd.reason == EndReason::Singular || bwd.reason == EndReason::StepLimit)
                    {
                        double h = sd.span / 256.0;
                        ctx.AddUnresolved(si, bwd.endU - h, bwd.endU + h, bwd.endV - h, bwd.endV + h,
                            UnresolvedReason::ZeroSetGradientVanishes);
                    }
                    if (fwd.reason == EndReason::Singular || fwd.reason == EndReason::StepLimit)
                    {
                        double h = sd.span / 256.0;
                        ctx.AddUnresolved(si, fwd.endU - h, fwd.endU + h, fwd.endV - h, fwd.endV + h,
                            UnresolvedReason::ZeroSetGradientVanishes);
                    }
                }

                if (curve.samples.size() < 4)
                {
                    continue;   // degenerate branch (e.g. tangential boundary touch)
                }

                // Consume other seeds lying on this branch.
                for (Seed& other : seeds[si])
                {
                    if (other.consumed)
                    {
                        continue;
                    }
                    for (auto const& smp : curve.samples)
                    {
                        if (ScaledDist(sd, other.u, other.v, smp.u, smp.v) * sd.span <= consumeTol)
                        {
                            other.consumed = true;
                            break;
                        }
                    }
                }

                result.curves.push_back(std::move(curve));
            }
        }

        // Curve parameters: normalized polyline chord length.
        for (Curve& curve : result.curves)
        {
            size_t n = curve.samples.size();
            curve.parameter.assign(n, 0.0);
            size_t seg = (curve.closed ? n : n - 1);
            std::vector<double> L(seg + 1, 0.0);
            for (size_t i = 0; i < seg; ++i)
            {
                V3 d = curve.samples[(i + 1) % n].position - curve.samples[i].position;
                L[i + 1] = L[i] + Length3(d);
            }
            double total = L[seg];
            if (total <= 0.0)
            {
                total = 1.0;
            }
            for (size_t i = 0; i < n; ++i)
            {
                curve.parameter[i] = L[std::min(i, seg)] / total;
            }
        }

        // Visibility along the continuous curve parameter.
        for (Curve& curve : result.curves)
        {
            ComputeVisibility(ctx, sds, curve);
        }

        result.complete = ctx.unresolved.empty();
        result.unresolved = std::move(ctx.unresolved);
        output = std::move(result);
        return true;
    }
}
