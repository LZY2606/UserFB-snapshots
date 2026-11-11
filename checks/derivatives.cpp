//
//   checks/derivatives.cpp
//
//   Independent, double-precision CPU verification of the derivative chain
//   used by Bfr surface evaluation.  The checks are deliberately built from
//   position-only Evaluate samples (finite differences), from explicit
//   control-point identities, and from a separate face-varying Surface --
//   never from the analytic derivative basis as its own reference.
//
//   The three check groups are:
//     derivative-reference : analytic P/Du/Dv/Duu/Duv/Dvv vs. finite
//                            differences of position-only evaluations, with
//                            explicit truncation/cancellation/scale budgets.
//     stencil-identity     : EvaluateStencil weights composed manually over
//                            GetControlPointIndices vs. Evaluate, plus a
//                            per-control-point perturbation check and
//                            affine/face-cycle variants.
//     fvar-sides           : a separate face-varying Surface with its own
//                            index table for two geometry-adjacent but
//                            UV-discontinuous sides, plus UV-table
//                            permutation and transform variants.
//
//   No ApplyStencil call is used to prove a stencil; all template evidence is
//   composed by this file.  Signs are compared as signs (there is no
//   per-point sign compensation).
//
//   Reproduction scope
//   ------------------
//   Bfr surface evaluation in double precision on the CPU, built with
//   OPENSUBDIV_GREGORY_EVAL_TRUE_DERIVATIVES.  The original defect was a
//   sign reversal of Duv (and of every second derivative) in triangular
//   subpatches whose PatchParam is rotated by 180 degrees: the second-
//   derivative scale incorrectly retained the first-derivative rotation sign.
//   Positions and first derivatives were unaffected.  Here, Loop exact
//   comparisons are restricted to *regular LOOP* subpatches produced by
//   refinement of the irregular tetrahedron face.  Gregory-triangle leaves
//   (param.IsRegular()==false) still use rational derivative approximation
//   and are deliberately excluded from exact pass/fail comparison.
//
//   Sampling
//   --------
//   * Loop tetrahedron (asymmetric): the reported points (0.3,0.35) in a
//     depth-1 rotated regular LOOP subpatch and (0.15,0.175) in a depth-2
//     rotated regular LOOP subpatch, plus a depth-2 non-rotated regular
//     LOOP control.  Patch type, depth, U/V, rotation and parameter scale
//     are read from the actual PatchTree and the sample plus every FD
//     neighbor is re-checked with FindSubPatch.  Local sample coords
//     (0.4,0.3) keep the neighborhood inside the same smooth subpatch and
//     away from the extraordinary vertex at local (0,0).
//   * Catmull-Clark: an asymmetric cube (regular subpatch and a
//     GREGORY_BASIS quad subpatch near its valence-3 vertices) and an
//     asymmetric planar 5x5 grid (regular face, known zero normal curvature).
//
//   Finite differences
//   ------------------
//   All references are position-only Evaluate samples:
//     Du,Dv  = central differences with h and h/2
//     Duu,Dvv= second axial differences
//     Duv    = the four-corner mixed difference
//   Two step sizes are required to converge where the measured error is above
//   the cancellation floor: the finer error must not exceed 0.75 of the
//   coarser error (otherwise the sample is rejected as non-convergent).
//
//   Tolerance basis
//   ---------------
//   L is the diagonal of the original control-net bounding box and q=2^depth.
//   For order k derivatives the adopted bound separates truncation and
//   floating-point cancellation:
//     order 0: 64 eps L
//     order 1: (1/6) L q^3 h^2 + 8 eps L / h
//     order 2: (1/12) L q^4 h^2 + 32 eps L / h^2
//   The second-order bound divided by L q^2 must stay <= 1e-5 for both steps;
//   that ceiling is asserted and may not be raised.  Relative error is never
//   formed with a near-zero denominator, and no per-point sign correction is
//   applied.  The non-rotated path is calibrated first (CALIBRATION line)
//   and the rotated samples reuse exactly the same steps and formula.
//

#include <opensubdiv/far/topologyDescriptor.h>
#include <opensubdiv/far/topologyRefinerFactory.h>
#include <opensubdiv/bfr/refinerSurfaceFactory.h>
#include <opensubdiv/bfr/surface.h>
#include <opensubdiv/bfr/surfaceData.h>
#include <opensubdiv/bfr/patchTree.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace OpenSubdiv;

typedef Bfr::Surface<double> Surface;
typedef Surface::PointDescriptor PD;

const double kEps = std::numeric_limits<double>::epsilon();
const int    kNumComponents3 = 3;

//  Adopted (non-raiseable) normalized second-derivative budget: 1e-5.
const double kSecondBudgetNormalized = 1e-5;
//  The same normalized budget is used for first derivatives as a consistency
//  guard (the task's explicit ceiling is for second derivatives).
const double kFirstBudgetNormalized  = 1e-5;

const double kH1 = 1.0e-3;
const double kH2 = 0.5e-3;

//  Error budget components.  The subpatch parameter scale is q=2^depth;
//  L is the diagonal of the original control-net bounding box.
struct ErrBudget {
    double truncation;
    double cancellation;
    double bound;
    double normalized;
};

ErrBudget makeBudget(int order, double L, double q, double h) {
    ErrBudget b = {0.0, 0.0, 0.0, 0.0};
    if (order == 0) {
        b.cancellation = 64.0 * kEps * L;
        b.bound        = b.cancellation;
        b.normalized   = b.bound / L;
        return b;
    }
    const double q3 = q * q * q;
    const double q4 = q3 * q;
    if (order == 1) {
        //  Central first difference: truncation ~ (h^2/6) f''' with f''' ~ L q^3;
        //  cancellation ~ eps L / h.
        b.truncation   = (1.0 / 6.0) * L * q3 * h * h;
        b.cancellation = 8.0 * kEps * L / h;
        b.bound        = b.truncation + b.cancellation;
        b.normalized   = b.bound / (L * q);
    } else {
        //  Central second difference: truncation ~ (h^2/12) f'''' with
        //  f'''' ~ L q^4; cancellation ~ eps L / h^2.
        b.truncation   = (1.0 / 12.0) * L * q4 * h * h;
        b.cancellation = 32.0 * kEps * L / (h * h);
        b.bound        = b.truncation + b.cancellation;
        b.normalized   = b.bound / (L * q * q);
    }
    return b;
}

double bboxDiagonal(const std::vector<double> & data, int count, int ncomp) {
    if (count <= 0) return 0.0;
    int n = std::min(ncomp, 3);
    std::vector<double> lo(n), hi(n);
    for (int c = 0; c < n; ++c) {
        lo[c] = hi[c] = data[c];
    }
    for (int i = 1; i < count; ++i) {
        for (int c = 0; c < n; ++c) {
            double x = data[i * ncomp + c];
            lo[c] = std::min(lo[c], x);
            hi[c] = std::max(hi[c], x);
        }
    }
    double d2 = 0.0;
    for (int c = 0; c < n; ++c) {
        double d = hi[c] - lo[c];
        d2 += d * d;
    }
    return std::sqrt(d2);
}

struct FDAll {
    std::vector<double> P, Du, Dv, Duu, Duv, Dvv;
    void resize(int n) {
        P.assign(n, 0.0);
        Du.assign(n, 0.0);
        Dv.assign(n, 0.0);
        Duu.assign(n, 0.0);
        Duv.assign(n, 0.0);
        Dvv.assign(n, 0.0);
    }
};

std::vector<double> evalPosition(const Surface & s,
        const std::vector<double> & patchPoints, int ncomp,
        double u, double v) {
    std::vector<double> out(ncomp, 0.0);
    double uv[2] = {u, v};
    s.Evaluate(uv, patchPoints.data(), PD(ncomp, ncomp), out.data());
    return out;
}

void evalAll(const Surface & s, const std::vector<double> & patchPoints,
        int ncomp, double u, double v, FDAll & out) {
    out.resize(ncomp);
    double uv[2] = {u, v};
    s.Evaluate(uv, patchPoints.data(), PD(ncomp, ncomp),
               out.P.data(), out.Du.data(), out.Dv.data(),
               out.Duu.data(), out.Duv.data(), out.Dvv.data());
}

FDAll evalFiniteDifferences(const Surface & s,
        const std::vector<double> & patchPoints, int ncomp,
        double u, double v, double h) {
    FDAll r;
    r.resize(ncomp);
    const std::vector<double> c  = evalPosition(s, patchPoints, ncomp, u, v);
    const std::vector<double> pu = evalPosition(s, patchPoints, ncomp, u + h, v);
    const std::vector<double> mu = evalPosition(s, patchPoints, ncomp, u - h, v);
    const std::vector<double> pv = evalPosition(s, patchPoints, ncomp, u, v + h);
    const std::vector<double> mv = evalPosition(s, patchPoints, ncomp, u, v - h);
    const std::vector<double> pp = evalPosition(s, patchPoints, ncomp, u + h, v + h);
    const std::vector<double> pm = evalPosition(s, patchPoints, ncomp, u + h, v - h);
    const std::vector<double> mp = evalPosition(s, patchPoints, ncomp, u - h, v + h);
    const std::vector<double> mm = evalPosition(s, patchPoints, ncomp, u - h, v - h);

    for (int i = 0; i < ncomp; ++i) {
        r.P[i]   = c[i];
        r.Du[i]  = (pu[i] - mu[i]) / (2.0 * h);
        r.Dv[i]  = (pv[i] - mv[i]) / (2.0 * h);
        r.Duu[i] = (pu[i] - 2.0 * c[i] + mu[i]) / (h * h);
        r.Dvv[i] = (pv[i] - 2.0 * c[i] + mv[i]) / (h * h);
        r.Duv[i] = (pp[i] - pm[i] - mp[i] + mm[i]) / (4.0 * h * h);
    }
    return r;
}

bool allFinite(const std::vector<double> & a) {
    for (double x : a) {
        if (!std::isfinite(x)) return false;
    }
    return true;
}

bool allFinite(const FDAll & a) {
    return allFinite(a.P) && allFinite(a.Du) && allFinite(a.Dv) &&
           allFinite(a.Duu) && allFinite(a.Duv) && allFinite(a.Dvv);
}

//
//  A mesh description independent of Bfr/Far construction (so the same data
//  can be re-used after affine transforms or face-cycle permutations).
//
struct MeshSpec {
    Sdc::SchemeType scheme = Sdc::SCHEME_CATMARK;
    Sdc::Options    options;
    std::vector<double> vtx;       // stride 3
    std::vector<int>    faceSizes;
    std::vector<int>    faceVerts;

    bool              hasFvar = false;
    int               fvarNumValues = 0;
    int               fvarValueStride = 2;
    std::vector<double> fvarValues; // stride 2
    std::vector<int>    fvarIndices; // per face-vertex

    MeshSpec() {
        options.SetFVarLinearInterpolation(Sdc::Options::FVAR_LINEAR_ALL);
    }
};

std::unique_ptr<Far::TopologyRefiner>
makeRefiner(const MeshSpec & m) {
    Far::TopologyDescriptor desc;
    desc.numVertices = (int)(m.vtx.size() / 3);
    desc.numFaces    = (int)m.faceSizes.size();
    desc.numVertsPerFace    = m.faceSizes.data();
    desc.vertIndicesPerFace = m.faceVerts.data();

    Far::TopologyDescriptor::FVarChannel channel;
    if (m.hasFvar) {
        channel.numValues  = m.fvarNumValues;
        channel.valueIndices = m.fvarIndices.data();
        desc.numFVarChannels = 1;
        desc.fvarChannels    = &channel;
    }
    typedef Far::TopologyRefinerFactory<Far::TopologyDescriptor> Factory;
    Factory::Options opts(m.scheme, m.options);
    std::unique_ptr<Far::TopologyRefiner> r(Factory::Create(desc, opts));
    if (!r) throw std::runtime_error("TopologyRefiner creation failed");
    return r;
}

//
//  A Bfr Surface together with the data needed to evaluate it independently.
//
struct SurfaceEval {
    Surface             surf;
    std::vector<double> meshValues;   // full source table/array
    int                 meshStride = 3;
    int                 ncomp = 3;
    std::vector<double> controlPoints; // gathered by cv indices
    std::vector<double> patchPoints;
    std::vector<int>    cvIndices;
    int                 numControlPoints = 0;
    double              L = 0.0;       // control net bbox diagonal

    void prepare(const std::vector<double> & values, int stride, int pointSize) {
        meshValues = values;
        meshStride = stride;
        ncomp      = pointSize;

        numControlPoints = surf.GetNumControlPoints();
        int numPatchPoints = surf.GetNumPatchPoints();

        controlPoints.assign((size_t)numControlPoints * pointSize, 0.0);
        patchPoints.assign((size_t)numPatchPoints * pointSize, 0.0);

        surf.GatherControlPoints(values.data(), PD(pointSize, stride),
                                 controlPoints.data(), PD(pointSize, pointSize));
        surf.PreparePatchPoints(values.data(), PD(pointSize, stride),
                                patchPoints.data(), PD(pointSize, pointSize));

        cvIndices.resize(numControlPoints);
        surf.GetControlPointIndices(cvIndices.data());

        L = bboxDiagonal(controlPoints, numControlPoints, pointSize);
    }
};

SurfaceEval makeVertexSurfaceEval(Far::TopologyRefiner const & mesh,
        int face, const std::vector<double> & vtx) {
    Bfr::RefinerSurfaceFactory<> factory(mesh);
    SurfaceEval e;
    Surface s;
    if (!factory.InitVertexSurface(face, &s)) {
        throw std::runtime_error("InitVertexSurface failed");
    }
    e.surf = s;
    e.prepare(vtx, 3, 3);
    return e;
}

const Bfr::internal::SurfaceData &
surfaceDataOf(Surface const & s) {
    //  Surface<> is documented/asserted to consist only of its SurfaceData.
    static_assert(sizeof(Surface) == sizeof(Bfr::internal::SurfaceData),
                  "Surface layout is expected to match SurfaceData");
    return reinterpret_cast<Bfr::internal::SurfaceData const &>(s);
}

std::string typeName(Sdc::SchemeType scheme, bool patchIsRegular) {
    if (scheme == Sdc::SCHEME_LOOP) {
        return patchIsRegular ? "LOOP" : "GREGORY_TRIANGLE";
    }
    return patchIsRegular ? "REGULAR" : "GREGORY_BASIS";
}

struct SampleInfo {
    std::string name;
    int         face = 0;
    int         patch = -1;
    int         depth = 0;
    int         U = 0, V = 0;
    bool        rotated = false;
    int         orientation = 0; // triangle: -1 rotated, +1 not; quad: 0
    bool        patchIsRegular = true;
    std::string patchType;
    double      q = 1.0;
    double      uv[2] = {0.0, 0.0};
    double      L = 0.0;
    int         ncomp = 3;
};

void printSample(const SampleInfo & si) {
    std::cout << "SAMPLE group=derivative-reference case=" << si.name
              << " face=" << si.face << " patch=" << si.patch
              << " patch_type=" << si.patchType
              << " depth=" << si.depth
              << " U=" << si.U << " V=" << si.V
              << " rotated=" << (si.rotated ? 1 : 0)
              << " orientation=" << si.orientation
              << " uv=(" << si.uv[0] << "," << si.uv[1] << ")"
              << " L=" << si.L << " q=" << si.q
              << " h1=" << kH1 << " h2=" << kH2 << "\n";
    const ErrBudget b1h1 = makeBudget(1, si.L, si.q, kH1);
    const ErrBudget b2h1 = makeBudget(2, si.L, si.q, kH1);
    const ErrBudget b2h2 = makeBudget(2, si.L, si.q, kH2);
    std::cout << "  budget order1(h1) trunc=" << b1h1.truncation
              << " cancel=" << b1h1.cancellation
              << " bound=" << b1h1.bound
              << " normalized=" << b1h1.normalized
              << " | order2(h1) trunc=" << b2h1.truncation
              << " cancel=" << b2h1.cancellation
              << " bound=" << b2h1.bound
              << " normalized=" << b2h1.normalized
              << " | order2(h2) trunc=" << b2h2.truncation
              << " cancel=" << b2h2.cancellation
              << " bound=" << b2h2.bound
              << " normalized=" << b2h2.normalized
              << " (ceiling=" << kSecondBudgetNormalized << ")\n";
}

void printCalibration(const SampleInfo & si) {
    const ErrBudget b2h1 = makeBudget(2, si.L, si.q, kH1);
    const ErrBudget b2h2 = makeBudget(2, si.L, si.q, kH2);
    std::cout << "CALIBRATION sample=" << si.name
              << " L=" << si.L << " q=" << si.q
              << " h1=" << kH1 << " h2=" << kH2
              << " order2_normalized_h1=" << b2h1.normalized
              << " order2_normalized_h2=" << b2h2.normalized
              << " ceiling=" << kSecondBudgetNormalized
              << " (same steps and budget formula reused for rotated samples)\n";
}

//
//  Verify that every finite-difference neighbor of a Loop triangular subpatch
//  stays in the same subpatch (and the same smooth interior region).
//
bool sameLoopSubPatch(Bfr::internal::IrregularPatchType const & tree,
        const Bfr::Parameterization & param,
        const double uv[2], int patchIndex, double h) {
    double base[2] = {uv[0], uv[1]};
    int subFace = 0;
    if (param.HasSubFaces()) {
        subFace = param.ConvertCoordToNormalizedSubFace(base, base);
    }
    int basePatch = tree.FindSubPatch(base[0], base[1], subFace);
    if (basePatch != patchIndex) return false;

    const double du[5] = {0.0,  h, -h,  h, -h};
    const double dv[5] = {0.0,  h,  h, -h, -h};
    for (int i = 0; i < 5; ++i) {
        double p[2] = {uv[0] + du[i], uv[1] + dv[i]};
        int sf = 0;
        if (param.HasSubFaces()) {
            sf = param.ConvertCoordToNormalizedSubFace(p, p);
        }
        if (tree.FindSubPatch(p[0], p[1], sf) != patchIndex) return false;
    }
    return true;
}

//
//  Derivative-reference group.
//
struct DerivativeReferenceResult {
    int  cases = 0;
    int  detectedScaleCases = 0;
    int  detectedMixedSignCases = 0;
    int  finitePositionCases = 0;
    bool genericFailure = false;
};

struct SampleComparisons {
    FDAll analytic;
    FDAll fd1;
    FDAll fd2;
};

struct DerivativeFailure {
    std::string sample;
    std::string derivative;
    int         component = 0;
    std::string assertion;
    double      analytic = 0.0;
    double      fd = 0.0;
    double      err = 0.0;
    double      bound = 0.0;
    double      L = 0.0;
    double      q = 0.0;
};

DerivativeReferenceResult
checkDerivativeReferenceSample(const SurfaceEval & e, const SampleInfo & si) {
    DerivativeReferenceResult result;
    ++result.cases;

    SampleComparisons cmp;
    evalAll(e.surf, e.patchPoints, e.ncomp, si.uv[0], si.uv[1], cmp.analytic);
    cmp.fd1 = evalFiniteDifferences(e.surf, e.patchPoints, e.ncomp,
                                    si.uv[0], si.uv[1], kH1);
    cmp.fd2 = evalFiniteDifferences(e.surf, e.patchPoints, e.ncomp,
                                    si.uv[0], si.uv[1], kH2);

    bool finite = allFinite(cmp.analytic) && allFinite(cmp.fd1) && allFinite(cmp.fd2);
    if (!finite) {
        result.genericFailure = true;
        std::cout << "FAIL group=derivative-reference case=" << si.name
                  << " assertion=nonfinite_position_or_derivative\n";
        return result;
    }
    ++result.finitePositionCases;

    printSample(si);
    std::cout << "  analytic P=";
    for (int c = 0; c < si.ncomp; ++c) std::cout << (c ? "," : "") << cmp.analytic.P[c];
    std::cout << " Du=";
    for (int c = 0; c < si.ncomp; ++c) std::cout << (c ? "," : "") << cmp.analytic.Du[c];
    std::cout << " Dv=";
    for (int c = 0; c < si.ncomp; ++c) std::cout << (c ? "," : "") << cmp.analytic.Dv[c];
    std::cout << " Duu=";
    for (int c = 0; c < si.ncomp; ++c) std::cout << (c ? "," : "") << cmp.analytic.Duu[c];
    std::cout << " Duv=";
    for (int c = 0; c < si.ncomp; ++c) std::cout << (c ? "," : "") << cmp.analytic.Duv[c];
    std::cout << " Dvv=";
    for (int c = 0; c < si.ncomp; ++c) std::cout << (c ? "," : "") << cmp.analytic.Dvv[c];
    std::cout << "\n";

    for (int c = 0; c < si.ncomp; ++c) {
        std::cout << "  fd(h1) P=" << cmp.fd1.P[c] << " Du=" << cmp.fd1.Du[c]
                  << " Dv=" << cmp.fd1.Dv[c] << " Duu=" << cmp.fd1.Duu[c]
                  << " Duv=" << cmp.fd1.Duv[c] << " Dvv=" << cmp.fd1.Dvv[c]
                  << " | fd(h2) P=" << cmp.fd2.P[c] << " Du=" << cmp.fd2.Du[c]
                  << " Dv=" << cmp.fd2.Dv[c] << " Duu=" << cmp.fd2.Duu[c]
                  << " Duv=" << cmp.fd2.Duv[c] << " Dvv=" << cmp.fd2.Dvv[c] << "\n";
    }

    struct DerivRef {
        const char * name;
        const std::vector<double> & analytic;
        const std::vector<double> & fd1;
        const std::vector<double> & fd2;
        int order;
    };
    const DerivRef refs[6] = {
        {"P",   cmp.analytic.P,   cmp.fd1.P,   cmp.fd2.P,   0},
        {"Du",  cmp.analytic.Du,  cmp.fd1.Du,  cmp.fd2.Du,  1},
        {"Dv",  cmp.analytic.Dv,  cmp.fd1.Dv,  cmp.fd2.Dv,  1},
        {"Duu", cmp.analytic.Duu, cmp.fd1.Duu, cmp.fd2.Duu, 2},
        {"Duv", cmp.analytic.Duv, cmp.fd1.Duv, cmp.fd2.Duv, 2},
        {"Dvv", cmp.analytic.Dvv, cmp.fd1.Dvv, cmp.fd2.Dvv, 2},
    };

    bool firstBad = false;
    bool mixedSignBad = false;
    bool otherBad = false;
    std::vector<DerivativeFailure> firstFailures;
    std::vector<DerivativeFailure> mixedFailures;
    std::vector<DerivativeFailure> otherFailures;

    for (const DerivRef & r : refs) {
        const ErrBudget b1 = makeBudget(r.order, si.L, si.q, kH1);
        const ErrBudget b2 = makeBudget(r.order, si.L, si.q, kH2);
        if (r.order == 2) {
            //  The adopted ceiling may not be raised: check the normalized
            //  second-derivative budget for both step sizes.
            if (!(b1.normalized <= kSecondBudgetNormalized) ||
                !(b2.normalized <= kSecondBudgetNormalized)) {
                result.genericFailure = true;
                otherFailures.push_back({si.name, r.name, -1,
                    "second_derivative_budget_ceiling",
                    b1.normalized, b2.normalized, 0.0,
                    kSecondBudgetNormalized, si.L, si.q});
            }
        } else if (r.order == 1) {
            if (!(b1.normalized <= kFirstBudgetNormalized) ||
                !(b2.normalized <= kFirstBudgetNormalized)) {
                result.genericFailure = true;
                otherFailures.push_back({si.name, r.name, -1,
                    "first_derivative_budget_ceiling",
                    b1.normalized, b2.normalized, 0.0,
                    kFirstBudgetNormalized, si.L, si.q});
            }
        }

        for (int c = 0; c < si.ncomp; ++c) {
            double a = r.analytic[c];
            double f1 = r.fd1[c];
            double f2 = r.fd2[c];
            double e1 = std::fabs(a - f1);
            double e2 = std::fabs(a - f2);

            const bool withinBudget = (e1 <= b1.bound && e2 <= b2.bound);

            //  Both step sizes must demonstrate convergence only where the
            //  measured error is above the floating-point cancellation floor
            //  (otherwise the finer step can be dominated by cancellation and
            //  no monotone convergence should be demanded).
            const double floorErr = 128.0 * kEps * si.L *
                                    std::pow(si.q, (double)r.order);
            bool converged = true;
            if (r.order > 0 && e1 > 10.0 * floorErr &&
                e1 > b1.cancellation) {
                converged = (e2 <= 0.75 * e1 + floorErr);
            }
            if (!withinBudget || !converged) {
                DerivativeFailure f = {si.name, r.name, c,
                    withinBudget ? "two_step_convergence" :
                    ((r.order == 0) ? "position_mismatch" :
                    ((r.order == 1) ? "first_derivative_within_budget" :
                                      "second_derivative_within_budget")),
                    a, f1, e1, b1.bound, si.L, si.q};
                if (r.order == 1) {
                    if (!withinBudget) {
                        firstBad = true;
                        firstFailures.push_back(f);
                    } else {
                        otherBad = true;
                        otherFailures.push_back(f);
                    }
                } else if (r.order == 2) {
                    double magDiff = std::fabs(std::fabs(a) - std::fabs(f2));
                    double magTol  = 1e-3 * si.L * si.q * si.q;
                    if (!withinBudget &&
                        (a < 0.0) != (f2 < 0.0) && magDiff <= magTol) {
                        mixedSignBad = true;
                        mixedFailures.push_back(
                            {f.sample, f.derivative, f.component,
                             "mixed_sign_reversed", a, f2, e2, b2.bound,
                             si.L, si.q});
                    } else {
                        otherBad = true;
                        otherFailures.push_back(f);
                    }
                } else {
                    otherBad = true;
                    otherFailures.push_back(f);
                }
            }
        }
    }
    //  p0/p1 (or q=2^depth) scale sanity: q must be the actual subpatch
    //  parameter scale, not guessed.  The selection function verifies this.

    if (firstBad) {
        ++result.detectedScaleCases;
        for (const DerivativeFailure & f : firstFailures) {
            std::cout << "FAIL group=derivative-reference case=" << f.sample
                      << " derivative=" << f.derivative
                      << " component=" << f.component
                      << " assertion=" << f.assertion
                      << " analytic=" << f.analytic << " fd=" << f.fd
                      << " err=" << f.err << " bound=" << f.bound
                      << " L=" << f.L << " q=" << f.q << "\n";
        }
    }
    if (mixedSignBad) {
        ++result.detectedMixedSignCases;
        for (const DerivativeFailure & f : mixedFailures) {
            std::cout << "FAIL group=derivative-reference case=" << f.sample
                      << " derivative=" << f.derivative
                      << " component=" << f.component
                      << " assertion=" << f.assertion
                      << " analytic=" << f.analytic << " fd=" << f.fd
                      << " err=" << f.err << " bound=" << f.bound
                      << " L=" << f.L << " q=" << f.q << "\n";
        }
    }
    if (otherBad) {
        result.genericFailure = true;
        for (const DerivativeFailure & f : otherFailures) {
            std::cout << "FAIL group=derivative-reference case=" << f.sample
                      << " derivative=" << f.derivative
                      << " component=" << f.component
                      << " assertion=" << f.assertion
                      << " analytic=" << f.analytic << " fd=" << f.fd
                      << " err=" << f.err << " bound=" << f.bound
                      << " L=" << f.L << " q=" << f.q << "\n";
        }
    }
    return result;
}

//
//  Sample selection.  The subpatch identity (type, depth, U/V, rotation,
//  parameter scale) is read from the actual PatchTree and re-verified with
//  FindSubPatch; it is never inferred from the coordinate alone.
//
bool buildLoopSamples(const SurfaceEval & e, std::vector<SampleInfo> & out) {
    if (e.surf.IsRegular() || e.surf.IsLinear()) return false;
    Bfr::internal::IrregularPatchType const & tree = surfaceDataOf(e.surf).getIrregPatch();
    Bfr::Parameterization param = e.surf.GetParameterization();

    int face = 0;
    bool haveDepth1Rotated = false;
    bool haveDepth2Rotated = false;
    bool haveNonRotated   = false;

    for (int i = 0; i < tree.GetNumPatches(); ++i) {
        Far::PatchParam pp = tree.GetSubPatchParam(i);
        if (!pp.IsRegular()) continue;      // exact comparison: regular LOOP subpatch
        if (pp.GetBoundary() != 0) continue;

        bool rotated = pp.IsTriangleRotated();
        int depth = (int)pp.GetDepth();
        bool want = false;
        std::string name;
        if (rotated && depth == 1 && !haveDepth1Rotated) {
            want = true; haveDepth1Rotated = true;
            name = "loop_depth1_rotated";
        } else if (rotated && depth == 2 && !haveDepth2Rotated) {
            want = true; haveDepth2Rotated = true;
            name = "loop_depth2_rotated";
        } else if (!rotated && !haveNonRotated) {
            want = true; haveNonRotated = true;
            name = "loop_nonrotated_control";
        }
        if (!want) continue;

        //  Local interior sample (0.4,0.3); convert through the actual
        //  PatchParam so the reported UV is the real face coordinate.
        double u = 0.4, v = 0.3;
        pp.UnnormalizeTriangle(u, v);
        double baseUv[2] = {u, v};
        if (!sameLoopSubPatch(tree, param, baseUv, i, kH1)) continue;

        //  The observed reported points (0.3,0.35) and (0.15,0.175) must be
        //  reproduced for the two rotated samples.
        if (name == "loop_depth1_rotated") {
            if (std::fabs(u - 0.3) > 1e-12 || std::fabs(v - 0.35) > 1e-12) continue;
        }
        if (name == "loop_depth2_rotated") {
            if (std::fabs(u - 0.15) > 1e-12 || std::fabs(v - 0.175) > 1e-12) continue;
        }

        double frac = pp.GetParamFraction();
        double q = 1.0 / frac;
        if (std::fabs(q - std::pow(2.0, depth)) > 1e-9) continue;

        SampleInfo si;
        si.name = name;
        si.face = face;
        si.patch = i;
        si.depth = depth;
        si.U = (int)pp.GetU();
        si.V = (int)pp.GetV();
        si.rotated = rotated;
        si.orientation = rotated ? -1 : 1;
        si.patchIsRegular = true;
        si.patchType = typeName(Sdc::SCHEME_LOOP, true);
        si.q = q;
        si.uv[0] = u;
        si.uv[1] = v;
        si.L = e.L;
        si.ncomp = 3;
        out.push_back(si);
        if (out.size() == 3) break; // the required depth1/depth2/control trio
    }
    //  Calibrate on the non-rotated normal path first; the rotated samples
    //  then reuse exactly the same step sizes and budget formula.
    std::stable_sort(out.begin(), out.end(),
        [](const SampleInfo & a, const SampleInfo & b) {
            return (a.rotated ? 1 : 0) < (b.rotated ? 1 : 0);
        });
    return haveDepth1Rotated && haveDepth2Rotated && haveNonRotated;
}

bool buildCatmarkCubeSamples(const SurfaceEval & e, std::vector<SampleInfo> & out) {
    if (e.surf.IsRegular() || e.surf.IsLinear()) return false;
    Bfr::internal::IrregularPatchType const & tree = surfaceDataOf(e.surf).getIrregPatch();
    Bfr::Parameterization param = e.surf.GetParameterization();

    bool haveRegular = false;
    bool haveGregory = false;
    for (int i = 0; i < tree.GetNumPatches(); ++i) {
        Far::PatchParam pp = tree.GetSubPatchParam(i);
        if (pp.GetBoundary() != 0) continue;

        bool regular = pp.IsRegular();
        if (regular && haveRegular) continue;
        if (!regular && haveGregory) continue;
        if (regular) haveRegular = true; else haveGregory = true;

        if (regular) {
            //  regular Catmull-Clark subpatch (bicubic B-spline)
        }

        double u = 0.4, v = 0.3;
        pp.Unnormalize(u, v);

        //  Verify the sample and all FD neighbors are in this same subpatch.
        double base[2] = {u, v};
        int subFace = 0;
        if (param.HasSubFaces()) {
            subFace = param.ConvertCoordToNormalizedSubFace(base, base);
        }
        if (tree.FindSubPatch(base[0], base[1], subFace) != i) continue;
        bool same = true;
        const double du[5] = {0.0, kH1, -kH1, kH1, -kH1};
        const double dv[5] = {0.0, kH1, kH1, -kH1, -kH1};
        for (int k = 0; k < 5; ++k) {
            double p[2] = {u + du[k], v + dv[k]};
            int sf = 0;
            if (param.HasSubFaces()) {
                sf = param.ConvertCoordToNormalizedSubFace(p, p);
            }
            if (tree.FindSubPatch(p[0], p[1], sf) != i) same = false;
        }
        if (!same) continue;

        double frac = pp.GetParamFraction();
        double q = 1.0 / frac;
        int depth = (int)pp.GetDepth();
        if (std::fabs(q - std::pow(2.0, depth)) > 1e-9) continue;

        SampleInfo si;
        si.name = regular ? "cc_cube_regular_subpatch" : "cc_cube_gregory_quad";
        si.face = 0;
        si.patch = i;
        si.depth = depth;
        si.U = (int)pp.GetU();
        si.V = (int)pp.GetV();
        si.rotated = false;             // quad path has no triangular rotation
        si.orientation = 0;
        si.patchIsRegular = regular;
        si.patchType = typeName(Sdc::SCHEME_CATMARK, regular);
        si.q = q;
        si.uv[0] = u;
        si.uv[1] = v;
        si.L = e.L;
        si.ncomp = 3;
        out.push_back(si);

        if (haveRegular && haveGregory) break;
    }
    //  Calibrate on the regular (non-rotated quad) path first.
    std::stable_sort(out.begin(), out.end(),
        [](const SampleInfo & a, const SampleInfo & b) {
            return (a.patchIsRegular ? 0 : 1) < (b.patchIsRegular ? 0 : 1);
        });
    return haveRegular && haveGregory;
}

DerivativeReferenceResult runDerivativeReference() {
    DerivativeReferenceResult total;

    //  ---- Loop: non-symmetric tetrahedron ---------------------------------
    MeshSpec tet;
    tet.scheme = Sdc::SCHEME_LOOP;
    tet.faceSizes = {3, 3, 3, 3};
    tet.faceVerts = {0,2,1, 0,1,3, 1,2,3, 2,0,3};
    tet.vtx = { 0,0,0,  1.3,.1,.2,  -.2,1.1,.4,  .3,-.4,1.7 };

    {
        std::unique_ptr<Far::TopologyRefiner> mesh = makeRefiner(tet);
        SurfaceEval e = makeVertexSurfaceEval(*mesh, 0, tet.vtx);
        std::vector<SampleInfo> samples;
        if (!buildLoopSamples(e, samples)) {
            std::cout << "FAIL group=derivative-reference assertion=loop_sample_selection\n";
            total.genericFailure = true;
            return total;
        }
        for (size_t i = 0; i < samples.size(); ++i) {
            if (i == 0) printCalibration(samples[i]);
            DerivativeReferenceResult r = checkDerivativeReferenceSample(e, samples[i]);
            total.cases += r.cases;
            total.finitePositionCases += r.finitePositionCases;
            total.detectedScaleCases += r.detectedScaleCases;
            total.detectedMixedSignCases += r.detectedMixedSignCases;
            total.genericFailure = total.genericFailure || r.genericFailure;
        }
    }

    //  ---- Catmull-Clark: asymmetric cube ----------------------------------
    MeshSpec cube;
    cube.scheme = Sdc::SCHEME_CATMARK;
    cube.faceSizes = {4,4,4,4,4,4};
    cube.faceVerts = {0,1,2,3, 4,7,6,5, 0,4,5,1, 1,5,6,2, 2,6,7,3, 3,7,4,0};
    cube.vtx = { -1.1,-0.9,-1.2,  1.3,-0.8,-1.0,  1.2,1.1,-0.9,  -0.9,1.3,-1.4,
                 -1.2,-1.0,1.1,   1.1,-0.7,1.3,   1.4,1.2,1.0,   -1.0,0.8,1.2 };
    {
        std::unique_ptr<Far::TopologyRefiner> mesh = makeRefiner(cube);
        SurfaceEval e = makeVertexSurfaceEval(*mesh, 0, cube.vtx);
        std::vector<SampleInfo> samples;
        if (!buildCatmarkCubeSamples(e, samples)) {
            std::cout << "FAIL group=derivative-reference assertion=catmark_cube_sample_selection\n";
            total.genericFailure = true;
            return total;
        }
        for (size_t i = 0; i < samples.size(); ++i) {
            if (i == 0) printCalibration(samples[i]);
            DerivativeReferenceResult r = checkDerivativeReferenceSample(e, samples[i]);
            total.cases += r.cases;
            total.finitePositionCases += r.finitePositionCases;
            total.detectedScaleCases += r.detectedScaleCases;
            total.detectedMixedSignCases += r.detectedMixedSignCases;
            total.genericFailure = total.genericFailure || r.genericFailure;
        }
    }

    //  ---- Catmull-Clark: planar regular grid (absolute zero-curvature) -----
    MeshSpec grid;
    grid.scheme = Sdc::SCHEME_CATMARK;
    {
        const double xs[5] = {0.0, 1.0, 2.1, 3.3, 4.6};
        const double ys[5] = {0.0, 0.9, 2.0, 3.2, 4.4};
        for (int j = 0; j < 5; ++j) {
            for (int i = 0; i < 5; ++i) {
                grid.vtx.push_back(xs[i]);
                grid.vtx.push_back(ys[j]);
                grid.vtx.push_back(0.0);
            }
        }
        for (int j = 0; j < 4; ++j) {
            for (int i = 0; i < 4; ++i) {
                grid.faceSizes.push_back(4);
                int v0 = j * 5 + i;
                grid.faceVerts.push_back(v0);
                grid.faceVerts.push_back(v0 + 1);
                grid.faceVerts.push_back(v0 + 6);
                grid.faceVerts.push_back(v0 + 5);
            }
        }
    }
    {
        std::unique_ptr<Far::TopologyRefiner> mesh = makeRefiner(grid);
        SurfaceEval e = makeVertexSurfaceEval(*mesh, 5, grid.vtx); // central face
        SampleInfo si;
        si.name = "cc_grid_planar_regular";
        si.face = 5;
        si.patch = -1;
        si.depth = 0;
        si.q = 1.0;
        si.rotated = false;
        si.orientation = 0;
        si.patchIsRegular = true;
        si.patchType = "REGULAR";
        si.uv[0] = 0.31;
        si.uv[1] = 0.42;
        si.L = e.L;
        si.ncomp = 3;

        printCalibration(si);
        DerivativeReferenceResult r = checkDerivativeReferenceSample(e, si);
        total.cases += r.cases;
        total.finitePositionCases += r.finitePositionCases;
        total.genericFailure = total.genericFailure || r.genericFailure;

        //  Absolute zero-curvature reference: the plane is z=0, so the
        //  normal (z) second derivatives must be exactly zero, and the
        //  finite differences must agree absolutely.
        FDAll a;
        evalAll(e.surf, e.patchPoints, e.ncomp, si.uv[0], si.uv[1], a);
        FDAll f1 = evalFiniteDifferences(e.surf, e.patchPoints, e.ncomp,
                                        si.uv[0], si.uv[1], kH1);
        double absTol = 1e-12 * e.L;
        bool zeroCurvature =
            std::fabs(a.Duu[2]) <= absTol &&
            std::fabs(a.Duv[2]) <= absTol &&
            std::fabs(a.Dvv[2]) <= absTol &&
            std::fabs(f1.Duu[2]) <= absTol &&
            std::fabs(f1.Duv[2]) <= absTol &&
            std::fabs(f1.Dvv[2]) <= absTol;
        if (!zeroCurvature) {
            std::cout << "FAIL group=derivative-reference case=" << si.name
                      << " assertion=planar_zero_curvature"
                      << " analytic_duu=" << a.Duu[2]
                      << " analytic_duv=" << a.Duv[2]
                      << " analytic_dvv=" << a.Dvv[2]
                      << " fd_duu=" << f1.Duu[2]
                      << " fd_duv=" << f1.Duv[2]
                      << " fd_dvv=" << f1.Dvv[2]
                      << " abs_tol=" << absTol << "\n";
            total.genericFailure = true;
        } else {
            std::cout << "PLANAR_ZERO_CURVATURE case=" << si.name
                      << " abs_tol=" << absTol
                      << " analytic_duu_z=" << a.Duu[2]
                      << " analytic_duv_z=" << a.Duv[2]
                      << " analytic_dvv_z=" << a.Dvv[2]
                      << " fd_duu_z=" << f1.Duu[2]
                      << " fd_duv_z=" << f1.Duv[2]
                      << " fd_dvv_z=" << f1.Dvv[2] << "\n";
        }
    }

    //  Curved surfaces must retain a clearly non-zero mixed term.
    {
        std::unique_ptr<Far::TopologyRefiner> mesh = makeRefiner(tet);
        SurfaceEval e = makeVertexSurfaceEval(*mesh, 0, tet.vtx);
        std::vector<SampleInfo> samples;
        buildLoopSamples(e, samples);
        for (const SampleInfo & si : samples) {
            FDAll a;
            evalAll(e.surf, e.patchPoints, e.ncomp, si.uv[0], si.uv[1], a);
            double required = 1e-3 * si.L * si.q * si.q;
            if (!(std::fabs(a.Duv[2]) > required)) {
                std::cout << "FAIL group=derivative-reference case=" << si.name
                          << " assertion=curved_nonzero_mixed_term"
                          << " analytic_duv=" << a.Duv[2]
                          << " required=" << required << "\n";
                total.genericFailure = true;
            } else {
                std::cout << "CURVED_MIXED_TERM case=" << si.name
                          << " analytic_duv_z=" << a.Duv[2]
                          << " required=" << required << "\n";
            }
        }
    }

    if (total.genericFailure) {
        //  Generic failures are not accepted as detections by the caller.
    }
    if (total.detectedScaleCases > 0) {
        std::cout << "DETECTED derivative-scale cases="
                  << total.detectedScaleCases << "\n";
    }
    if (total.detectedMixedSignCases > 0) {
        std::cout << "DETECTED mixed-sign cases="
                  << total.detectedMixedSignCases << "\n";
    }
    if (total.detectedScaleCases == 0 && total.detectedMixedSignCases == 0 &&
        !total.genericFailure) {
        std::cout << "PASS derivative-reference cases=" << total.cases << "\n";
    }
    std::cout << "finite_positions group=derivative-reference cases="
              << total.finitePositionCases << "\n";
    return total;
}

//
//  Stencil-identity group.
//
struct StencilResult {
    int  cases = 0;
    int  finitePositionCases = 0;
    bool failure = false;
};

void reportStencilFailure(const std::string & sample, const char * assertion,
                          double analytic, double composed, double tolerance) {
    std::cout << "FAIL group=stencil-identity case=" << sample
              << " assertion=" << assertion
              << " analytic=" << analytic << " composed=" << composed
              << " tolerance=" << tolerance << "\n";
}

double composeWeights(const std::vector<double> & w,
                      const std::vector<double> & values, int stride,
                      const std::vector<int> & cv, int comp) {
    double sum = 0.0;
    for (size_t i = 0; i < w.size(); ++i) {
        int index = cv[i];
        sum += w[i] * values[(size_t)index * stride + comp];
    }
    return sum;
}

//  Template identity and explicit perturbation evidence for one surface.
StencilResult checkStencilForSample(const SurfaceEval & e, const SampleInfo & si) {
    StencilResult r;
    ++r.cases;

    int n = e.numControlPoints;
    std::vector<double> sP(n), sDu(n), sDv(n), sDuu(n), sDuv(n), sDvv(n);
    double stencilUv[2] = {si.uv[0], si.uv[1]};
    int returned = e.surf.EvaluateStencil(
            stencilUv,
            sP.data(), sDu.data(), sDv.data(), sDuu.data(), sDuv.data(), sDvv.data());

    if (returned != n) {
        reportStencilFailure(si.name, "stencil_size", n, returned, 0.0);
        r.failure = true;
        return r;
    }

    //  Position weights sum to one; every derivative template sums to zero.
    double tolSum = 1e-10 * std::max(1.0, e.L);
    auto sum = [](const std::vector<double> & w) {
        double s = 0.0; for (double x : w) s += x; return s;
    };
    bool sumsOk =
        std::fabs(sum(sP) - 1.0) <= tolSum &&
        std::fabs(sum(sDu)) <= tolSum && std::fabs(sum(sDv)) <= tolSum &&
        std::fabs(sum(sDuu)) <= tolSum && std::fabs(sum(sDuv)) <= tolSum &&
        std::fabs(sum(sDvv)) <= tolSum;
    if (!sumsOk) {
        reportStencilFailure(si.name, "template_weights_sum", 1.0, sum(sP), tolSum);
        r.failure = true;
        return r;
    }

    //  Compose the templates manually over GetControlPointIndices identities
    //  and compare with PreparePatchPoints + Evaluate (not ApplyStencil).
    FDAll analytic;
    evalAll(e.surf, e.patchPoints, e.ncomp, si.uv[0], si.uv[1], analytic);

    struct TemplateRef {
        const char * name;
        const std::vector<double> & w;
        const std::vector<double> & analytic;
        int order;
    };
    const TemplateRef refs[6] = {
        {"P",   sP,   analytic.P,   0},
        {"Du",  sDu,  analytic.Du,  1},
        {"Dv",  sDv,  analytic.Dv,  1},
        {"Duu", sDuu, analytic.Duu, 2},
        {"Duv", sDuv, analytic.Duv, 2},
        {"Dvv", sDvv, analytic.Dvv, 2},
    };

    for (const TemplateRef & ref : refs) {
        for (int c = 0; c < si.ncomp; ++c) {
            double composed = composeWeights(ref.w, e.meshValues, e.meshStride,
                                             e.cvIndices, c);
            ErrBudget b = makeBudget(ref.order, e.L, si.q, kH1);
            if (!(std::fabs(composed - ref.analytic[c]) <= b.bound + 1e-12)) {
                reportStencilFailure(si.name, "template_vs_evaluate",
                                     ref.analytic[c], composed, b.bound);
                r.failure = true;
            }
        }
    }

    //  Perturb each original control point and compare the position change
    //  and its parametric differences with the corresponding template entry.
    double delta = 1e-2 * std::max(e.L, 1e-8);
    FDAll baseFd = evalFiniteDifferences(e.surf, e.patchPoints, e.ncomp,
                                        si.uv[0], si.uv[1], kH1);
    std::vector<double> perturbedValues = e.meshValues;
    int perturbCv = -1;
    //  Choose the first component of the first returned index by default.
    //  Any component works (the surface is linear in control points).
    for (int ci = 0; ci < n; ++ci) {
        perturbedValues = e.meshValues;
        int vindex = e.cvIndices[ci];
        perturbCv = vindex;
        //  Perturb every component of this control point by the same delta:
        //  the template weight then applies independently to each component.
        for (int pc = 0; pc < e.ncomp; ++pc) {
            perturbedValues[(size_t)vindex * e.meshStride + pc] += delta;
        }

        std::vector<double> pp(e.patchPoints.size(), 0.0);
        e.surf.PreparePatchPoints(perturbedValues.data(),
                                  PD(e.ncomp, e.meshStride),
                                  pp.data(), PD(e.ncomp, e.ncomp));
        FDAll pert = evalFiniteDifferences(e.surf, pp, e.ncomp,
                                          si.uv[0], si.uv[1], kH1);

        double tolP  = 1e-7;
        double tolD1 = 1e-4;
        double tolD2 = 1e-3;

        auto checkChange = [&](const char * dname, int order,
                               const std::vector<double> & base,
                               const std::vector<double> & changed,
                               const std::vector<double> & w) {
            for (int c = 0; c < si.ncomp; ++c) {
                double d = (changed[c] - base[c]) / delta;
                double expected = w[ci];
                double tol = (order == 0) ? tolP : ((order == 1) ? tolD1 : tolD2);
                if (!(std::fabs(d - expected) <= tol)) {
                    reportStencilFailure(si.name, "perturbation_template", expected, d, tol);
                    r.failure = true;
                }
            }
        };
        checkChange("P", 0, baseFd.P, pert.P, sP);
        checkChange("Du", 1, baseFd.Du, pert.Du, sDu);
        checkChange("Dv", 1, baseFd.Dv, pert.Dv, sDv);
        checkChange("Duu", 2, baseFd.Duu, pert.Duu, sDuu);
        checkChange("Duv", 2, baseFd.Duv, pert.Duv, sDuv);
        checkChange("Dvv", 2, baseFd.Dvv, pert.Dvv, sDvv);

        ++r.cases; // one actual perturbation executed
    }
    (void)perturbCv;

    ++r.finitePositionCases;
    return r;
}

MeshSpec translateMesh(const MeshSpec & m, const double t[3]) {
    MeshSpec r = m;
    for (size_t i = 0; i + 2 < r.vtx.size(); i += 3) {
        r.vtx[i]     += t[0];
        r.vtx[i + 1] += t[1];
        r.vtx[i + 2] += t[2];
    }
    return r;
}

MeshSpec affineMesh(const MeshSpec & m, const double A[3][3], const double b[3]) {
    MeshSpec r = m;
    for (size_t i = 0; i + 2 < r.vtx.size(); i += 3) {
        double x = r.vtx[i], y = r.vtx[i + 1], z = r.vtx[i + 2];
        r.vtx[i]     = A[0][0]*x + A[0][1]*y + A[0][2]*z + b[0];
        r.vtx[i + 1] = A[1][0]*x + A[1][1]*y + A[1][2]*z + b[1];
        r.vtx[i + 2] = A[2][0]*x + A[2][1]*y + A[2][2]*z + b[2];
    }
    return r;
}

MeshSpec cycleMeshFaces(const MeshSpec & m, int shift) {
    MeshSpec r = m;
    size_t fv = 0;
    for (size_t f = 0; f < r.faceSizes.size(); ++f) {
        int s = r.faceSizes[f];
        int k = shift % s;
        if (k < 0) k += s;
        std::vector<int> tmp(s);
        for (int i = 0; i < s; ++i) tmp[i] = r.faceVerts[fv + (i + k) % s];
        for (int i = 0; i < s; ++i) r.faceVerts[fv + i] = tmp[i];
        if (r.hasFvar) {
            for (int i = 0; i < s; ++i)
                tmp[i] = r.fvarIndices[fv + (i + k) % s];
            for (int i = 0; i < s; ++i) r.fvarIndices[fv + i] = tmp[i];
        }
        fv += s;
    }
    return r;
}

void transformVector(const double A[3][3], const std::vector<double> & in,
                     std::vector<double> & out) {
    out.resize(in.size());
    for (size_t i = 0; i + 2 < in.size(); i += 3) {
        double x = in[i], y = in[i + 1], z = in[i + 2];
        out[i]     = A[0][0]*x + A[0][1]*y + A[0][2]*z;
        out[i + 1] = A[1][0]*x + A[1][1]*y + A[1][2]*z;
        out[i + 2] = A[2][0]*x + A[2][1]*y + A[2][2]*z;
    }
}

StencilResult runStencilIdentity() {
    StencilResult total;

    const double A[3][3] = {{1.4, 0.25, 0.0}, {0.0, 0.7, -0.15}, {0.1, 0.0, 1.1}};
    const double b[3]    = {0.3, -0.2, 0.5};
    const double t[3]    = {0.4, -0.7, 1.2};

    MeshSpec tet;
    tet.scheme = Sdc::SCHEME_LOOP;
    tet.faceSizes = {3, 3, 3, 3};
    tet.faceVerts = {0,2,1, 0,1,3, 1,2,3, 2,0,3};
    tet.vtx = { 0,0,0,  1.3,.1,.2,  -.2,1.1,.4,  .3,-.4,1.7 };

    MeshSpec cube;
    cube.scheme = Sdc::SCHEME_CATMARK;
    cube.faceSizes = {4,4,4,4,4,4};
    cube.faceVerts = {0,1,2,3, 4,7,6,5, 0,4,5,1, 1,5,6,2, 2,6,7,3, 3,7,4,0};
    cube.vtx = { -1.1,-0.9,-1.2,  1.3,-0.8,-1.0,  1.2,1.1,-0.9,  -0.9,1.3,-1.4,
                 -1.2,-1.0,1.1,   1.1,-0.7,1.3,   1.4,1.2,1.0,   -1.0,0.8,1.2 };

    MeshSpec grid;
    grid.scheme = Sdc::SCHEME_CATMARK;
    {
        const double xs[5] = {0.0, 1.0, 2.1, 3.3, 4.6};
        const double ys[5] = {0.0, 0.9, 2.0, 3.2, 4.4};
        for (int j = 0; j < 5; ++j) for (int i = 0; i < 5; ++i) {
            grid.vtx.push_back(xs[i]); grid.vtx.push_back(ys[j]); grid.vtx.push_back(0.0);
        }
        for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) {
            grid.faceSizes.push_back(4);
            int v0 = j * 5 + i;
            grid.faceVerts.push_back(v0); grid.faceVerts.push_back(v0 + 1);
            grid.faceVerts.push_back(v0 + 6); grid.faceVerts.push_back(v0 + 5);
        }
    }

    struct Primary {
        MeshSpec const * mesh;
        int face;
        double uv[2];
        std::string name;
        bool planar;
        double q;
    };
    Primary primaries[3] = {
        {&tet,  0, {0.3, 0.35},   "loop_depth1_rotated_identity",   false, 2.0},
        {&cube, 0, {0.1, 0.075},  "cc_cube_gregory_identity",       false, 4.0},
        {&grid, 5, {0.31, 0.42},  "cc_grid_planar_identity",        true,  1.0},
    };

    //  Base template/perturbation evidence for every exact derivative sample
    //  (all Loop depths/directions, both Catmull-Clark regions).
    {
        std::unique_ptr<Far::TopologyRefiner> mesh = makeRefiner(tet);
        SurfaceEval e = makeVertexSurfaceEval(*mesh, 0, tet.vtx);
        std::vector<SampleInfo> samples;
        buildLoopSamples(e, samples);
        for (SampleInfo si : samples) {
            si.name += "_identity";
            StencilResult r = checkStencilForSample(e, si);
            total.cases += r.cases;
            total.finitePositionCases += r.finitePositionCases;
            total.failure = total.failure || r.failure;
        }
    }
    {
        std::unique_ptr<Far::TopologyRefiner> mesh = makeRefiner(cube);
        SurfaceEval e = makeVertexSurfaceEval(*mesh, 0, cube.vtx);
        std::vector<SampleInfo> samples;
        buildCatmarkCubeSamples(e, samples);
        for (SampleInfo si : samples) {
            si.name += "_identity";
            StencilResult r = checkStencilForSample(e, si);
            total.cases += r.cases;
            total.finitePositionCases += r.finitePositionCases;
            total.failure = total.failure || r.failure;
        }
    }
    {
        std::unique_ptr<Far::TopologyRefiner> mesh = makeRefiner(grid);
        SurfaceEval e = makeVertexSurfaceEval(*mesh, 5, grid.vtx);
        SampleInfo si;
        si.name = "cc_grid_planar_identity";
        si.face = 5;
        si.uv[0] = 0.31;
        si.uv[1] = 0.42;
        si.q = 1.0;
        si.ncomp = 3;
        si.L = e.L;
        StencilResult r = checkStencilForSample(e, si);
        total.cases += r.cases;
        total.finitePositionCases += r.finitePositionCases;
        total.failure = total.failure || r.failure;
    }

    //  Translated and non-uniform-affine variants: repeat the template chain
    //  and compare partials by the coordinate transform (never by comparing
    //  the raw Du arrays).
    for (const Primary & p : primaries) {
        for (int variant = 0; variant < 2; ++variant) {
            MeshSpec m = (variant == 0) ? translateMesh(*p.mesh, t)
                                        : affineMesh(*p.mesh, A, b);
            std::unique_ptr<Far::TopologyRefiner> mesh = makeRefiner(m);
            SurfaceEval e = makeVertexSurfaceEval(*mesh, p.face, m.vtx);
            SampleInfo si;
            si.name = p.name + ((variant == 0) ? "_translated" : "_affine");
            si.face = p.face;
            si.uv[0] = p.uv[0];
            si.uv[1] = p.uv[1];
            si.q = p.q;
            si.ncomp = 3;
            si.L = e.L;

            StencilResult r = checkStencilForSample(e, si);
            total.cases += r.cases;
            total.finitePositionCases += r.finitePositionCases;
            total.failure = total.failure || r.failure;

            if (variant == 1) {
                //  Compare partial derivatives via the affine coordinate map.
                std::unique_ptr<Far::TopologyRefiner> baseMesh = makeRefiner(*p.mesh);
                SurfaceEval base = makeVertexSurfaceEval(*baseMesh, p.face, p.mesh->vtx);
                FDAll oldD, newD;
                evalAll(base.surf, base.patchPoints, 3, p.uv[0], p.uv[1], oldD);
                evalAll(e.surf, e.patchPoints, 3, p.uv[0], p.uv[1], newD);

                auto checkAffine = [&](const char * dname, const std::vector<double> & oldv,
                                       const std::vector<double> & newv, int order) {
                    std::vector<double> expected;
                    transformVector(A, oldv, expected);
                    double scale = e.L * std::pow(si.q, (double)order);
                    double tol = 1e-9 * std::max(1.0, scale);
                    for (size_t c = 0; c + 2 < oldv.size(); c += 3) {
                        for (int j = 0; j < 3; ++j) {
                            if (!(std::fabs(newv[c + j] - expected[c + j]) <= tol)) {
                                reportStencilFailure(si.name, "affine_derivative_transform",
                                                     expected[c + j], newv[c + j], tol);
                                total.failure = true;
                            }
                        }
                    }
                };
                checkAffine("Du", oldD.Du, newD.Du, 1);
                checkAffine("Dv", oldD.Dv, newD.Dv, 1);
                checkAffine("Duu", oldD.Duu, newD.Duu, 2);
                checkAffine("Duv", oldD.Duv, newD.Duv, 2);
                checkAffine("Dvv", oldD.Dvv, newD.Dvv, 2);
                //  Position: P' = A P + b
                std::vector<double> expectedP;
                transformVector(A, oldD.P, expectedP);
                double tolP = 1e-9 * std::max(1.0, e.L);
                for (size_t c = 0; c + 2 < oldD.P.size(); c += 3) {
                    for (int j = 0; j < 3; ++j) {
                        if (!(std::fabs(newD.P[c + j] - (expectedP[c + j] + b[j])) <= tolP)) {
                            reportStencilFailure(si.name, "affine_position_transform",
                                                 expectedP[c + j] + b[j], newD.P[c + j], tolP);
                            total.failure = true;
                        }
                    }
                }
                ++total.cases;
            }
            ++total.cases;
        }
    }

    //  Face-vertex cycle starting point: repeat the template chain.
    for (const Primary & p : primaries) {
        MeshSpec m = cycleMeshFaces(*p.mesh, 1);
        std::unique_ptr<Far::TopologyRefiner> mesh = makeRefiner(m);
        SurfaceEval e = makeVertexSurfaceEval(*mesh, p.face, m.vtx);
        SampleInfo si;
        si.name = p.name + "_face_cycle";
        si.face = p.face;
        si.uv[0] = p.uv[0];
        si.uv[1] = p.uv[1];
        si.q = p.q;
        si.ncomp = 3;
        si.L = e.L;
        StencilResult r = checkStencilForSample(e, si);
        total.cases += r.cases;
        total.finitePositionCases += r.finitePositionCases;
        total.failure = total.failure || r.failure;
    }

    if (!total.failure) {
        std::cout << "PASS stencil-identity cases=" << total.cases << "\n";
    }
    std::cout << "finite_positions group=stencil-identity cases="
              << total.finitePositionCases << "\n";
    return total;
}

//
//  face-varying sides group.
//
struct FvarResult {
    int  cases = 0;
    int  detectedFvarIndexCases = 0;
    int  finitePositionCases = 0;
    bool genericFailure = false;
};

static const int kFvarFaceA = 5;
static const int kFvarFaceB = 6;
static const int kFvarBaseValueIndex = 1000;

void cornerUV(int face, int corner, double uv[2]) {
    if (face == kFvarFaceA) {
        static const double uvs[4][2] = {{0.0,0.0},{1.0,0.0},{1.0,1.0},{0.0,1.0}};
        uv[0] = uvs[corner][0]; uv[1] = uvs[corner][1];
    } else if (face == kFvarFaceB) {
        //  Deliberately discontinuous across the shared edge: left edge at
        //  u=0.25 while face A's adjacent right edge is at u=1.
        static const double uvs[4][2] = {{0.25,0.5},{1.25,0.5},{1.25,1.5},{0.25,1.5}};
        uv[0] = uvs[corner][0]; uv[1] = uvs[corner][1];
    } else {
        static const double uvs[4][2] = {{0.0,0.0},{1.0,0.0},{1.0,1.0},{0.0,1.0}};
        uv[0] = uvs[corner][0]; uv[1] = uvs[corner][1];
    }
}

MeshSpec makeFvarMesh(bool permuteValues, int geometryVariant,
                      const double t[3], const double A[3][3], const double b[3],
                      bool cycleFaces) {
    MeshSpec m;
    m.scheme = Sdc::SCHEME_CATMARK;
    m.options.SetFVarLinearInterpolation(Sdc::Options::FVAR_LINEAR_ALL);

    const double xs[5] = {0.0, 1.0, 2.1, 3.3, 4.6};
    const double ys[5] = {0.0, 0.9, 2.0, 3.2, 4.4};
    for (int j = 0; j < 5; ++j) for (int i = 0; i < 5; ++i) {
        m.vtx.push_back(xs[i]); m.vtx.push_back(ys[j]); m.vtx.push_back(0.0);
    }
    for (int j = 0; j < 4; ++j) for (int i = 0; i < 4; ++i) {
        m.faceSizes.push_back(4);
        int v0 = j * 5 + i;
        m.faceVerts.push_back(v0); m.faceVerts.push_back(v0 + 1);
        m.faceVerts.push_back(v0 + 6); m.faceVerts.push_back(v0 + 5);
    }

    m.hasFvar = true;
    m.fvarNumValues = kFvarBaseValueIndex + (int)m.faceSizes.size() * 4;
    m.fvarValues.assign((size_t)m.fvarNumValues * 2, 0.0);
    m.fvarIndices.resize(m.faceVerts.size());
    for (size_t f = 0; f < m.faceSizes.size(); ++f) {
        for (int c = 0; c < 4; ++c) {
            int index = kFvarBaseValueIndex + (int)f * 4 + c;
            m.fvarIndices[f * 4 + c] = index;
            double uv[2];
            cornerUV((int)f, c, uv);
            m.fvarValues[(size_t)index * 2 + 0] = uv[0];
            m.fvarValues[(size_t)index * 2 + 1] = uv[1];
        }
    }

    if (permuteValues) {
        //  Permute the UV value table and synchronously permute the per-corner
        //  indices: every corner keeps exactly the same UV value.
        const int count = (int)m.faceSizes.size() * 4;
        std::vector<int> perm(count);
        for (int i = 0; i < count; ++i) perm[i] = (i + 17) % count;
        std::vector<double> permuted(m.fvarValues.size(), 0.0);
        for (int i = 0; i < count; ++i) {
            int from = kFvarBaseValueIndex + i;
            int to   = kFvarBaseValueIndex + perm[i];
            permuted[(size_t)to * 2 + 0] = m.fvarValues[(size_t)from * 2 + 0];
            permuted[(size_t)to * 2 + 1] = m.fvarValues[(size_t)from * 2 + 1];
        }
        m.fvarValues = permuted;
        for (size_t i = 0; i < m.fvarIndices.size(); ++i) {
            int rel = m.fvarIndices[i] - kFvarBaseValueIndex;
            m.fvarIndices[i] = kFvarBaseValueIndex + perm[rel];
        }
    }

    if (geometryVariant == 1) {
        for (size_t i = 0; i + 2 < m.vtx.size(); i += 3) {
            m.vtx[i] += t[0]; m.vtx[i + 1] += t[1]; m.vtx[i + 2] += t[2];
        }
    } else if (geometryVariant == 2) {
        for (size_t i = 0; i + 2 < m.vtx.size(); i += 3) {
            double x = m.vtx[i], y = m.vtx[i + 1], z = m.vtx[i + 2];
            m.vtx[i]     = A[0][0]*x + A[0][1]*y + A[0][2]*z + b[0];
            m.vtx[i + 1] = A[1][0]*x + A[1][1]*y + A[1][2]*z + b[1];
            m.vtx[i + 2] = A[2][0]*x + A[2][1]*y + A[2][2]*z + b[2];
        }
    }
    if (cycleFaces) {
        m = cycleMeshFaces(m, 1);
    }
    return m;
}

FvarResult runFvarSides() {
    FvarResult total;
    const double t[3]  = {0.4, -0.7, 1.2};
    const double A[3][3] = {{1.4, 0.25, 0.0}, {0.0, 0.7, -0.15}, {0.1, 0.0, 1.1}};
    const double b[3]  = {0.3, -0.2, 0.5};

    struct Variant {
        std::string name;
        MeshSpec spec;
    };
    std::vector<Variant> variants;
    variants.push_back({"fvar_base",          makeFvarMesh(false, 0, t, A, b, false)});
    variants.push_back({"fvar_permuted_uv",   makeFvarMesh(true,  0, t, A, b, false)});
    variants.push_back({"fvar_translated",    makeFvarMesh(false, 1, t, A, b, false)});
    variants.push_back({"fvar_affine",        makeFvarMesh(false, 2, t, A, b, false)});
    variants.push_back({"fvar_face_cycle",    makeFvarMesh(false, 0, t, A, b, true)});

    double uvsample[2][2] = {{0.31, 0.42}, {0.37, 0.29}};

    //  The UV identity numbering must be obviously distinct from the
    //  geometry numbering, and the per-corner values must differ across the
    //  shared geometric edge (verified per variant below).
    {
        int numGeometryVerts = (int)(variants[0].spec.vtx.size() / 3);
        bool numberingDistinct = true;
        for (int idx : variants[0].spec.fvarIndices) {
            if (idx < kFvarBaseValueIndex || idx >= variants[0].spec.fvarNumValues)
                numberingDistinct = false;
        }
        for (int v : variants[0].spec.faceVerts) {
            if (v < 0 || v >= numGeometryVerts) numberingDistinct = false;
        }
        if (!numberingDistinct) {
            std::cout << "FAIL group=fvar-sides case=fvar_base"
                      << " assertion=fvar_numbers_distinct_from_geometry\n";
            total.genericFailure = true;
        }
    }

    //  Base values (variant 0) used for invariance comparisons.
    std::vector<double> baseVertexP[2], baseFvarP[2];
    std::vector<double> baseVertexPCycle[2], baseFvarPCycle[2];
    std::vector<double> baseDu[2], baseDv[2], baseDuu[2], baseDuv[2], baseDvv[2];

    for (size_t vi = 0; vi < variants.size(); ++vi) {
        Variant & var = variants[vi];
        std::unique_ptr<Far::TopologyRefiner> mesh = makeRefiner(var.spec);
        Bfr::RefinerSurfaceFactory<> factory(*mesh);

        SurfaceEval vertexE[2], fvarE[2];
        for (int side = 0; side < 2; ++side) {
            int face = (side == 0) ? kFvarFaceA : kFvarFaceB;
            Surface vs, fs;
            if (!factory.InitVertexSurface(face, &vs) ||
                !factory.InitFaceVaryingSurface(face, &fs, 0)) {
                std::cout << "FAIL group=fvar-sides case=" << var.name
                          << " assertion=fvar_surface_init face=" << face << "\n";
                total.genericFailure = true;
                continue;
            }
            vertexE[side].surf = vs;
            vertexE[side].prepare(var.spec.vtx, 3, 3);
            fvarE[side].surf = fs;
            fvarE[side].prepare(var.spec.fvarValues, 2, 2);

            if (!fs.IsLinear() || fvarE[side].numControlPoints != 4) {
                std::cout << "FAIL group=fvar-sides case=" << var.name
                          << " assertion=fvar_linear_surface face=" << face << "\n";
                total.genericFailure = true;
            }
        }

        //  Shared geometry edge and UV discontinuity.
        int fA = kFvarFaceA * 4, fB = kFvarFaceB * 4;
        std::vector<int> shared;
        for (int a = 0; a < 4; ++a) for (int bb = 0; bb < 4; ++bb) {
            if (var.spec.faceVerts[fA + a] == var.spec.faceVerts[fB + bb])
                shared.push_back(var.spec.faceVerts[fA + a]);
        }
        std::sort(shared.begin(), shared.end());
        shared.erase(std::unique(shared.begin(), shared.end()), shared.end());
        if (shared.size() != 2) {
            std::cout << "FAIL group=fvar-sides case=" << var.name
                      << " assertion=geometric_shared_edge\n";
            total.genericFailure = true;
        }
        bool uvDiscontinuous = false;
        for (int sharedVert : shared) {
            double uvA[2] = {0,0}, uvB[2] = {0,0};
            bool foundA = false, foundB = false;
            for (int c = 0; c < 4; ++c) {
                if (var.spec.faceVerts[fA + c] == sharedVert) {
                    int idx = var.spec.fvarIndices[fA + c];
                    uvA[0] = var.spec.fvarValues[idx*2+0];
                    uvA[1] = var.spec.fvarValues[idx*2+1];
                    foundA = true;
                }
                if (var.spec.faceVerts[fB + c] == sharedVert) {
                    int idx = var.spec.fvarIndices[fB + c];
                    uvB[0] = var.spec.fvarValues[idx*2+0];
                    uvB[1] = var.spec.fvarValues[idx*2+1];
                    foundB = true;
                }
            }
            if (foundA && foundB &&
                (std::fabs(uvA[0]-uvB[0]) + std::fabs(uvA[1]-uvB[1]) > 0.1)) {
                uvDiscontinuous = true;
            }
        }
        if (!uvDiscontinuous) {
            std::cout << "FAIL group=fvar-sides case=" << var.name
                      << " assertion=uv_discontinuous_across_shared_edge\n";
            total.genericFailure = true;
        }

        for (int side = 0; side < 2; ++side) {
            int face = (side == 0) ? kFvarFaceA : kFvarFaceB;
            ++total.cases;
            bool nonIndexFailure = false;
            bool indexFailure = false;

            //  Expected per-corner fvar identities come from the original
            //  input table, not from Bfr's (possibly mutated) path.
            std::vector<int> expected(4);
            for (int c = 0; c < 4; ++c)
                expected[c] = var.spec.fvarIndices[face * 4 + c];

            std::vector<int> got(4);
            fvarE[side].surf.GetControlPointIndices(got.data());
            if (got != expected) {
                std::cout << "FAIL group=fvar-sides case=" << var.name
                          << " side=" << (side == 0 ? "A" : "B")
                          << " assertion=fvar_index_identity";
                for (int c = 0; c < 4; ++c) std::cout << " expected" << c << "=" << expected[c];
                for (int c = 0; c < 4; ++c) std::cout << " got" << c << "=" << got[c];
                std::cout << "\n";
                indexFailure = true;
            }

            std::vector<double> sP(fvarE[side].numControlPoints, 0.0);
            double fvarStencilUv[2] = {uvsample[side][0], uvsample[side][1]};
            int ret = fvarE[side].surf.EvaluateStencil(fvarStencilUv, sP.data());
            if (ret != fvarE[side].numControlPoints) {
                std::cout << "FAIL group=fvar-sides case=" << var.name
                          << " assertion=fvar_stencil_size\n";
                nonIndexFailure = true;
            }

            double u = uvsample[side][0], v = uvsample[side][1];
            std::vector<double> evalP = evalPosition(fvarE[side].surf,
                    fvarE[side].patchPoints, 2, u, v);

            //  Position-only reference vs template composition on the
            //  face-varying Surface (each side's own index table).
            std::vector<double> composedP(2, 0.0);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < fvarE[side].numControlPoints; ++i)
                    composedP[c] += sP[i] *
                        var.spec.fvarValues[(size_t)got[i] * 2 + c];
            for (int c = 0; c < 2; ++c) {
                if (!(std::isfinite(evalP[c]) && std::isfinite(composedP[c])) ||
                    std::fabs(evalP[c] - composedP[c]) > 1e-12) {
                    std::cout << "FAIL group=fvar-sides case=" << var.name
                              << " side=" << (side == 0 ? "A" : "B")
                              << " assertion=fvar_position_vs_template comp=" << c
                              << " eval=" << evalP[c] << " composed=" << composedP[c]
                              << " tol=1e-12\n";
                    nonIndexFailure = true;
                }
            }

            //  Independent bilinear reference from the original UV table and
            //  the expected per-corner identities.
            double w[4] = {(1.0-u)*(1.0-v), u*(1.0-v), u*v, (1.0-u)*v};
            std::vector<double> manual(2, 0.0);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 4; ++i)
                    manual[c] += w[i] *
                        var.spec.fvarValues[(size_t)expected[i] * 2 + c];
            for (int c = 0; c < 2; ++c) {
                if (std::fabs(evalP[c] - manual[c]) > 1e-12) {
                    std::cout << "FAIL group=fvar-sides case=" << var.name
                              << " side=" << (side == 0 ? "A" : "B")
                              << " assertion=fvar_independent_bilinear comp=" << c
                              << " eval=" << evalP[c] << " expected=" << manual[c]
                              << " tol=1e-12\n";
                    indexFailure = true;
                }
            }

            //  The four parametric corners must interpolate exactly the
            //  original per-corner UV values.
            static const double corners[4][2] = {{0,0},{1,0},{1,1},{0,1}};
            for (int k = 0; k < 4; ++k) {
                std::vector<double> pk = evalPosition(fvarE[side].surf,
                        fvarE[side].patchPoints, 2, corners[k][0], corners[k][1]);
                for (int c = 0; c < 2; ++c) {
                    double expectedVal =
                        var.spec.fvarValues[(size_t)expected[k] * 2 + c];
                    if (std::fabs(pk[c] - expectedVal) > 1e-12) {
                        std::cout << "FAIL group=fvar-sides case=" << var.name
                                  << " side=" << (side == 0 ? "A" : "B")
                                  << " assertion=fvar_corner_value corner=" << k
                                  << " comp=" << c << " eval=" << pk[c]
                                  << " expected=" << expectedVal << " tol=1e-12\n";
                        indexFailure = true;
                    }
                }
            }

            //  Geometry side evidence chain.
            std::vector<double> gsP(vertexE[side].numControlPoints, 0.0);
            double geomStencilUv[2] = {u, v};
            int gret = vertexE[side].surf.EvaluateStencil(geomStencilUv, gsP.data());
            if (gret != vertexE[side].numControlPoints) {
                std::cout << "FAIL group=fvar-sides case=" << var.name
                          << " assertion=geometry_stencil_size\n";
                nonIndexFailure = true;
            }
            std::vector<double> gP = evalPosition(vertexE[side].surf,
                    vertexE[side].patchPoints, 3, u, v);
            std::vector<double> gComposed(3, 0.0);
            for (int c = 0; c < 3; ++c)
                for (int i = 0; i < vertexE[side].numControlPoints; ++i) {
                    int idx = vertexE[side].cvIndices[i];
                    gComposed[c] += gsP[i] * var.spec.vtx[(size_t)idx * 3 + c];
                }
            for (int c = 0; c < 3; ++c) {
                if (!(std::isfinite(gP[c]) && std::isfinite(gComposed[c])) ||
                    std::fabs(gP[c] - gComposed[c]) > 1e-10) {
                    std::cout << "FAIL group=fvar-sides case=" << var.name
                              << " side=" << (side == 0 ? "A" : "B")
                              << " assertion=geometry_position_vs_template comp=" << c
                              << " eval=" << gP[c] << " composed=" << gComposed[c]
                              << " tol=1e-10\n";
                    nonIndexFailure = true;
                }
            }

            bool positionsFinite = allFinite(gP) && allFinite(evalP);
            if (positionsFinite) ++total.finitePositionCases;
            else nonIndexFailure = true;

            //  Invariance / coordinate-transform comparisons.
            if (vi > 0) {
                ++total.cases;
                bool invFailure = false;
                const std::vector<double> & baseP = baseVertexP[side];
                const std::vector<double> & baseF = baseFvarP[side];
                double tolPos = 1e-10 * std::max(1.0, vertexE[side].L);
                if (var.name == "fvar_permuted_uv") {
                    for (int c = 0; c < 3; ++c)
                        if (std::fabs(gP[c] - baseP[c]) > tolPos) invFailure = true;
                    for (int c = 0; c < 2; ++c)
                        if (std::fabs(evalP[c] - baseF[c]) > tolPos) invFailure = true;
                    if (invFailure) {
                        std::cout << "FAIL group=fvar-sides case=" << var.name
                                  << " side=" << (side == 0 ? "A" : "B")
                                  << " assertion=uv_permutation_physical_invariance\n";
                    }
                } else if (var.name == "fvar_translated") {
                    for (int c = 0; c < 3; ++c)
                        if (std::fabs(gP[c] - (baseP[c] + t[c])) > tolPos) invFailure = true;
                    for (int c = 0; c < 2; ++c)
                        if (std::fabs(evalP[c] - baseF[c]) > tolPos) invFailure = true;
                    if (invFailure) {
                        std::cout << "FAIL group=fvar-sides case=" << var.name
                                  << " side=" << (side == 0 ? "A" : "B")
                                  << " assertion=translation_invariance\n";
                    }
                } else if (var.name == "fvar_affine") {
                    FDAll newD;
                    evalAll(vertexE[side].surf, vertexE[side].patchPoints, 3,
                            u, v, newD);
                    FDAll oldD;
                    oldD.Du = baseDu[side];   oldD.Dv = baseDv[side];
                    oldD.Duu = baseDuu[side]; oldD.Duv = baseDuv[side];
                    oldD.Dvv = baseDvv[side];
                    double tolDer = 1e-8 * std::max(1.0, vertexE[side].L);
                    auto checkAff = [&](const std::vector<double> & oldv,
                                        const std::vector<double> & newv) {
                        std::vector<double> expv;
                        transformVector(A, oldv, expv);
                        for (size_t c = 0; c + 2 < oldv.size(); c += 3)
                            for (int j = 0; j < 3; ++j)
                                if (std::fabs(newv[c+j] - expv[c+j]) > tolDer)
                                    invFailure = true;
                    };
                    checkAff(oldD.Du, newD.Du);
                    checkAff(oldD.Dv, newD.Dv);
                    checkAff(oldD.Duu, newD.Duu);
                    checkAff(oldD.Duv, newD.Duv);
                    checkAff(oldD.Dvv, newD.Dvv);
                    std::vector<double> expP;
                    transformVector(A, baseP, expP);
                    for (int c = 0; c < 3; ++c)
                        if (std::fabs(gP[c] - (expP[c] + b[c])) > tolPos)
                            invFailure = true;
                    for (int c = 0; c < 2; ++c)
                        if (std::fabs(evalP[c] - baseF[c]) > tolPos)
                            invFailure = true;
                    if (invFailure) {
                        std::cout << "FAIL group=fvar-sides case=" << var.name
                                  << " side=" << (side == 0 ? "A" : "B")
                                  << " assertion=affine_coordinate_transform\n";
                    }
                } else if (var.name == "fvar_face_cycle") {
                    //  A one-corner cycle maps old (u,v) to (1-v,u).
                    for (int c = 0; c < 3; ++c)
                        if (std::fabs(gP[c] - baseVertexPCycle[side][c]) > tolPos)
                            invFailure = true;
                    for (int c = 0; c < 2; ++c)
                        if (std::fabs(evalP[c] - baseFvarPCycle[side][c]) > tolPos)
                            invFailure = true;
                    if (invFailure) {
                        std::cout << "FAIL group=fvar-sides case=" << var.name
                                  << " side=" << (side == 0 ? "A" : "B")
                                  << " assertion=face_cycle_parameterization\n";
                    }
                }
                if (invFailure) nonIndexFailure = true;
            }

            if (vi == 0) {
                baseVertexP[side] = gP;
                baseFvarP[side] = evalP;
                FDAll d;
                evalAll(vertexE[side].surf, vertexE[side].patchPoints, 3,
                        u, v, d);
                baseDu[side] = d.Du;   baseDv[side] = d.Dv;
                baseDuu[side] = d.Duu; baseDuv[side] = d.Duv;
                baseDvv[side] = d.Dvv;
                double oldu = 1.0 - v, oldv = u;
                baseVertexPCycle[side] = evalPosition(vertexE[side].surf,
                        vertexE[side].patchPoints, 3, oldu, oldv);
                baseFvarPCycle[side] = evalPosition(fvarE[side].surf,
                        fvarE[side].patchPoints, 2, oldu, oldv);
            }

            if (indexFailure) ++total.detectedFvarIndexCases;
            if (nonIndexFailure) total.genericFailure = true;
        }
    }

    if (total.detectedFvarIndexCases > 0) {
        std::cout << "DETECTED fvar-index cases=" << total.detectedFvarIndexCases << "\n";
    } else if (!total.genericFailure) {
        std::cout << "PASS fvar-sides cases=" << total.cases << "\n";
    }
    std::cout << "finite_positions group=fvar-sides cases="
              << total.finitePositionCases << "\n";
    return total;
}

} // namespace

int main(int argc, char ** argv) {
    std::string group = "all";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--group" && i + 1 < argc) {
            group = argv[++i];
        } else if (std::strncmp(argv[i], "--group=", 8) == 0) {
            group = std::string(argv[i] + 8);
        }
    }
    bool hardFailure = false;
    try {
        if (group == "all" || group == "derivative-reference") {
            DerivativeReferenceResult r = runDerivativeReference();
            hardFailure = hardFailure || r.genericFailure;
        }
        if (group == "all" || group == "stencil-identity") {
            StencilResult r = runStencilIdentity();
            hardFailure = hardFailure || r.failure;
        }
        if (group == "all" || group == "fvar-sides") {
            FvarResult r = runFvarSides();
            hardFailure = hardFailure || r.genericFailure;
        }
    } catch (std::exception const & e) {
        std::cout << "FAIL group=" << group << " assertion=exception "
                  << e.what() << "\n";
        return 2;
    }
    return hardFailure ? 1 : 0;
}
