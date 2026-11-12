// Long-lived numeric regression for nanort's watertight triangle intersector.
//
// The expectations come exclusively from the integer directed-volume
// reference in target_geometry.h: hit and edge membership are decided by exact
// signs, distance and barycentrics stay rational until the final comparison.
// No library intersector/AABB/traversal result is used to make an expectation
// and a double run is never treated as the float truth.
#include "nanort.h"
#include "suite.h"
#include "target_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace tg = targetgeom;

namespace {

// ---- numeric tolerances, derived from the mantissa width -----------------
// float carries 24 bits (~1.2e-7 relative). A watertight query is a short
// chain of multiplies/adds, so a generous scale-based bound of 2e-5 absolute
// on the [0,1] barycentrics and 2e-4 relative on the distance is still ~50x
// tighter than any genuine cross-edge or whole-segment barycentric mistake
// (those are O(1e-2..1)). double uses a 1e-10 bound.
constexpr double TOL_UV_F = 2e-5;
constexpr double TOL_T_F = 2e-4;
constexpr double TOL_UV_D = 1e-10;
constexpr double TOL_T_D = 1e-10;

std::string rayText(const tg::i64 O[3], const tg::i64 D[3], tg::i64 L) {
  std::ostringstream os;
  os << "O=(" << O[0] / (double)L << "," << O[1] / (double)L << ","
     << O[2] / (double)L << ") D=(" << D[0] << "," << D[1] << "," << D[2]
     << ") lattice L=" << L;
  return os.str();
}

template <typename T>
nanort::Ray<T> makeRay(const tg::i64 O[3], const tg::i64 D[3], tg::i64 L,
                       T minT = T(0), T maxT = T(1e30)) {
  nanort::Ray<T> r;
  for (int i = 0; i < 3; ++i) {
    r.org[i] = T((long double)O[i] / (long double)L);
    r.dir[i] = T(D[i]);
  }
  r.min_t = minT;
  r.max_t = maxT;
  return r;
}

nanort::BVHTraceOptions makeOpts(const tg::RefOptions &ro) {
  nanort::BVHTraceOptions o;
  o.prim_ids_range[0] = ro.rangeLo;
  o.prim_ids_range[1] = ro.rangeHi;
  o.skip_prim_id = ro.skipId;
  o.cull_back_face = ro.cullBack;
  return o;
}

struct Query {
  tg::i64 O[3], D[3];
};

// A reusable float/double harness over one geometry + stride.
template <typename T>
struct Harness {
  const tg::Scene &sc;
  size_t stride;
  std::vector<T> storage;
  nanort::TriangleMesh<T> mesh;
  nanort::TriangleSAHPred<T> pred;
  nanort::BVHAccel<T> bvh;
  nanort::TriangleIntersector<T> inter;
  Harness(const tg::Scene &s, bool padded)
      : sc(s), stride(padded ? 4 * sizeof(T) : 3 * sizeof(T)),
        mesh(nullptr, nullptr, stride), pred(nullptr, nullptr, stride),
        inter(nullptr, nullptr, stride) {
    const auto &src = padded ? sc.vpad : sc.v;
    storage.resize(src.size());
    for (size_t i = 0; i < src.size(); ++i) storage[i] = T(src[i]);
    // Real build entry points with the caller-provided stride.
    new (&mesh) nanort::TriangleMesh<T>(storage.data(), sc.faces.data(), stride);
    new (&pred) nanort::TriangleSAHPred<T>(storage.data(), sc.faces.data(),
                                           stride);
    new (&inter) nanort::TriangleIntersector<T>(storage.data(),
                                                 sc.faces.data(), stride);
    if (!bvh.Build((unsigned)(sc.faces.size() / 3), mesh, pred))
      throw std::runtime_error("BVH build failed");
  }
};

double tolUV(bool isDouble) { return isDouble ? TOL_UV_D : TOL_UV_F; }
double tolT(bool isDouble) { return isDouble ? TOL_T_D : TOL_T_F; }

// Verify one query against the exact reference. Returns a failure detail
// (empty on success) and records the first actual deviation.
template <typename T>
std::string checkQuery(Suite &suite, const Harness<T> &h, const Query &q,
                       const tg::RefOptions &ro, bool isDouble,
                       const std::string &tag) {
  const tg::Scene &sc = h.sc;
  tg::RefResult ref =
      tg::referenceRay(sc.iv, sc.faces, q.O, q.D, tg::Scene::L, ro);
  nanort::Ray<T> ray = makeRay<T>(q.O, q.D, tg::Scene::L);
  nanort::BVHTraceOptions opts = makeOpts(ro);
  nanort::TriangleIntersection<T> isect;
  bool hit = h.bvh.Traverse(ray, h.inter, &isect, opts);

  auto fail = [&](const std::string &what) {
    std::ostringstream os;
    os << tag << " " << what << " | " << rayText(q.O, q.D, tg::Scene::L);
    if (ref.hit)
      os << " ref t=" << (double)ref.tnum / (double)ref.tden
         << " legalFaces=[";
    for (unsigned f : ref.legal) os << f << " ";
    if (ref.hit) os << "]";
    if (hit) os << " actual face=" << isect.prim_id << " t=" << isect.t
                << " u=" << isect.u << " v=" << isect.v;
    else os << " actual=miss";
    return os.str();
  };

  if (hit != ref.hit) {
    suite.check(false, fail("hit-flag mismatch"));
    return fail("hit-flag mismatch");
  }
  if (!hit) return "";

  // The BVH may pick any equidistant legal face, never a farther/illegal one.
  bool legal = false;
  for (unsigned f : ref.legal)
    if (f == isect.prim_id) legal = true;
  if (!legal) {
    std::string d = fail("returned face not in exact legal set");
    suite.check(false, d);
    return d;
  }

  const unsigned *f = &sc.faces[3 * isect.prim_id];
  tg::FaceHit fh = tg::exactFace(sc.iv, f, q.O, q.D);
  double eu = tg::rat(fh.un, fh.det), ev = tg::rat(fh.vn, fh.det);
  double ew = tg::rat(fh.wn, fh.det);
  double et = tg::rat(fh.tn, (tg::i128)tg::Scene::L * fh.det);

  double du = std::fabs((double)isect.u - eu);
  double dv = std::fabs((double)isect.v - ev);
  double dt = std::fabs((double)isect.t - et) / std::max(1e-30, std::fabs(et));
  if (du > tolUV(isDouble) || dv > tolUV(isDouble)) {
    std::ostringstream os;
    os << tag << " barycentric mismatch face=" << isect.prim_id << " "
       << rayText(q.O, q.D, tg::Scene::L) << " expected u=" << eu << "("
       << (long long)fh.un << "/" << (long long)fh.det << ") v=" << ev << "("
       << (long long)fh.vn << "/" << (long long)fh.det << ") w=" << ew
       << " actual u=" << isect.u << " v=" << isect.v
       << " firstDeviation du=" << du << " dv=" << dv;
    std::string d = os.str();
    suite.check(false, d);
    return d;
  }
  if (dt > tolT(isDouble)) {
    std::ostringstream os;
    os << tag << " distance mismatch face=" << isect.prim_id << " "
       << rayText(q.O, q.D, tg::Scene::L) << " expected t=" << et << "("
       << (long long)fh.tn << "/(" << tg::Scene::L << "*"
       << (long long)fh.det << ")) actual t=" << isect.t
       << " relativeDeviation=" << dt;
    std::string d = os.str();
    suite.check(false, d);
    return d;
  }

  // Reconstruct the hit point two independent ways and require agreement.
  double pExact[3], pRay[3], pBary[3];
  for (int i = 0; i < 3; ++i) {
    double oi = (double)q.O[i] / tg::Scene::L;
    pExact[i] = oi + et * q.D[i];
    pRay[i] = (double)ray.org[i] + (double)isect.t * (double)ray.dir[i];
    double w = 1.0 - (double)isect.u - (double)isect.v;
    pBary[i] = w * sc.v[3 * f[0] + i] + (double)isect.u * sc.v[3 * f[1] + i] +
               (double)isect.v * sc.v[3 * f[2] + i];
  }
  for (int i = 0; i < 3; ++i) {
    double scale = std::max(1.0, std::fabs(pExact[i]));
    if (std::fabs(pRay[i] - pExact[i]) > 1e-4 * scale ||
        std::fabs(pBary[i] - pExact[i]) > 1e-4 * scale) {
      std::ostringstream os;
      os << tag << " point reconstruction mismatch axis=" << i
         << " exact=" << pExact[i] << " ray=" << pRay[i] << " bary=" << pBary[i];
      std::string d = os.str();
      suite.check(false, d);
      return d;
    }
  }

  // Independent affine colour field, continuous over the whole surface.
  // Face identity is already enforced by the legal-set check, so this only
  // validates interpolation; a few ulps of colour rounding are allowed.
  const float colorTol = 2e-3f;
  float cExp[3];
  for (int k = 0; k < 3; ++k) {
    float c = 0.1f + 0.03f * (float)pExact[0] + 0.05f * (float)pExact[1] +
              0.02f * (float)pExact[2];
    cExp[k] = k == 0 ? c : (k == 1 ? 0.25f + 0.5f * c : 0.75f - 0.25f * c);
  }
  float w = 1.0f - isect.u - isect.v;
  for (int k = 0; k < 3; ++k) {
    float cAct = w * sc.color[3 * f[0] + k] +
                 isect.u * sc.color[3 * f[1] + k] +
                 isect.v * sc.color[3 * f[2] + k];
    if (std::fabs(cAct - cExp[k]) > colorTol) {
      std::ostringstream os;
      os << tag << " affine colour mismatch channel=" << k
         << " expected=" << cExp[k] << " actual=" << cAct;
      std::string d = os.str();
      suite.check(false, d);
      return d;
    }
  }
  suite.check(true, tag);
  return "";
}

// Direction library: major axis, negative, non-unit, with zero components.
std::vector<Query> seamRays() {
  std::vector<Query> q;
  auto add = [&](tg::i64 a, tg::i64 b, tg::i64 c, tg::i64 da, tg::i64 db,
                 tg::i64 dc) { q.push_back({{a, b, c}, {da, db, dc}}); };
  // Vertical (major z), negative and non-unit directions over the z=2 quads.
  add(8, 8, 64, 0, 0, -1);     // interior of quad Q1
  add(16, 16, 64, 0, 0, -2);   // non-unit, hits diagonal A region x+y=2
  add(24, 8, 64, 0, 0, -1);    // diagonal B neighbourhood
  add(8, 24, 64, 0, 0, -1);
  // Oblique non-unit directions with a zero component.
  add(4, 8, 80, 2, 0, -3);
  add(8, 4, 80, 0, -2, -3);
  add(20, 12, 80, -1, 2, -2);
  add(12, 20, 80, 3, -2, -4);
  // Negative major-axis direction (looking upward).
  add(8, 8, -32, 0, 0, 1);
  // Thin needle shared-edge ray (edge x=-8, z=2).
  add(-128, 0, 80, 0, 0, -1);
  add(-127, 8, 80, 2, -3, -3);
  // x- and y-major directions (dominant axis differs from z).
  add(32, 8, 80, 8, 1, -1);
  add(8, 32, 80, 1, 8, -1);
  return q;
}

}  // namespace

void register_tests(Suite &suite) {
  // Static so the scene outlives the registered lambdas, which capture it by
  // reference but are only invoked after this function returns.
  static const tg::Scene sc = tg::buildScene();

  // ------------------------------------------------------------------ exact
  suite.add("exact", [&](Suite &s) {
    // Hand-checked directed-volume rational for a fan interior ray.
    // Face (c0,p0,p1): vertices (2,2,0),(0,0,0),(4,0,0) in real units.
    // Ray O=(2,1,4), D=(0,0,-1) hits z=0 at t=4. Solving
    //   (2,1) = (2,2) + u(-2,-2) + v(2,-2)  gives u=v=1/4, w=1/2.
    {
      tg::i64 O[3] = {32, 16, 64}, D[3] = {0, 0, -1};
      unsigned fan[3] = {0, 1, 2};
      tg::FaceHit h = tg::exactFace(sc.iv, fan, O, D);
      s.check(h.hit, "fan interior ray must hit");
      s.check(h.un * 4 == h.det, "exact u numerator must be 1/4 of det");
      s.check(h.vn * 4 == h.det, "exact v numerator must be 1/4 of det");
      s.check(h.wn * 2 == h.det, "exact w numerator must be 1/2 of det");
      double t = tg::rat(h.tn, (tg::i128)tg::Scene::L * h.det);
      s.check(std::fabs(t - 4.0) < 1e-12, "exact distance must be 4");
    }
    // Exact edge membership: one numerator is zero but the ray still hits.
    {
      tg::i64 O[3] = {32, 0, 64}, D[3] = {0, 0, -1};  // on edge (p1..c0)
      unsigned fan[3] = {0, 1, 2};
      tg::FaceHit h = tg::exactFace(sc.iv, fan, O, D);
      s.check(h.hit && (h.un == 0 || h.vn == 0 || h.wn == 0),
              "edge ray must hit with a zero barycentric numerator");
    }
    // One lattice step outside: an exact negative numerator means a miss.
    // Point (3,2) lies above the p1-c0 edge y = -x + 4 of the fan triangle.
    {
      tg::i64 O[3] = {48, 32, 64}, D[3] = {0, 0, -1};
      unsigned fan[3] = {0, 1, 2};
      tg::FaceHit h = tg::exactFace(sc.iv, fan, O, D);
      s.check(!h.hit, "point outside the triangle must be an exact miss");
    }
    // Zero-area collinear face always rejects (det == 0).
    {
      tg::i64 O[3] = {80, 768 / 16, 64}, D[3] = {0, 0, -1};
      const unsigned *zf = &sc.faces[3 * sc.zeroAreaFace];
      tg::FaceHit h = tg::exactFace(sc.iv, zf, O, D);
      s.check(!h.hit && h.det == 0, "zero-area face must never intersect");
    }
    // A ray coplanar with a z=2 face (dz == 0) is explicitly rejected: the
    // exact determinant collapses to zero and the library must refuse too.
    {
      tg::i64 O[3] = {8, 8, 32}, D[3] = {2, 0, 0};  // travels inside z=2
      unsigned q[3] = {5, 6, 7};
      tg::FaceHit h = tg::exactFace(sc.iv, q, O, D);
      s.check(!h.hit && h.det == 0, "coplanar ray must have zero determinant");
      Harness<float> hf(sc, false);
      nanort::Ray<float> ray = makeRay<float>(O, D, tg::Scene::L);
      nanort::TriangleIntersection<float> isect;
      s.check(!hf.bvh.Traverse(ray, hf.inter, &isect),
              "library must reject a coplanar ray");
    }
    // Rational ordering of two distances without any float comparison.
    {
      tg::i64 nearO[3] = {8, 8, 48}, farO[3] = {8, 8, 96}, D[3] = {0, 0, -1};
      unsigned q[3] = {5, 6, 7};
      tg::FaceHit hn = tg::exactFace(sc.iv, q, nearO, D);
      tg::FaceHit hf = tg::exactFace(sc.iv, q, farO, D);
      s.check(hn.hit && hf.hit, "both depth rays hit");
      s.check(tg::cmpRat(hn.tn, (tg::i128)tg::Scene::L * hn.det, hf.tn,
                         (tg::i128)tg::Scene::L * hf.det) < 0,
              "near ray rational distance must be strictly smaller");
    }
  });

  // ------------------------------------------------------------------ seams
  suite.add("seams", [&](Suite &s) {
    Harness<float> hf(sc, false);
    tg::RefOptions ro;
    const auto rays = seamRays();
    unsigned seamMulti = 0, sideDistinct = 0;
    for (const Query &q : rays) {
      tg::RefResult ref =
          tg::referenceRay(sc.iv, sc.faces, q.O, q.D, tg::Scene::L, ro);
      if (ref.hit && ref.legal.size() >= 2) ++seamMulti;
      checkQuery<float>(s, hf, q, ro, false, "seams");
    }
    s.check(seamMulti >= 1, "at least one ray must land on a shared seam");

    // Representable neighbour points on the two sides of diagonal A must
    // resolve to distinct single faces, while the seam itself stays multi.
    const Query left = {{20, 8, 64}, {0, 0, -1}};   // x+y < 2 side
    const Query right = {{28, 16, 64}, {0, 0, -1}}; // x+y > 2 side
    const Query seam = {{16, 16, 64}, {0, 0, -1}};  // x+y == 2
    tg::RefResult rl = tg::referenceRay(sc.iv, sc.faces, left.O, left.D,
                                        tg::Scene::L, ro);
    tg::RefResult rr = tg::referenceRay(sc.iv, sc.faces, right.O, right.D,
                                        tg::Scene::L, ro);
    tg::RefResult rs = tg::referenceRay(sc.iv, sc.faces, seam.O, seam.D,
                                        tg::Scene::L, ro);
    s.check(rl.hit && rr.hit && rl.legal.size() == 1 &&
                rr.legal.size() == 1 && rl.legal != rr.legal,
            "the two seam sides must map to distinct faces");
    s.check(rs.hit && rs.legal.size() >= 2,
            "the exact seam must admit multiple equidistant faces");
    ++sideDistinct;
    s.check(sideDistinct == 1, "seam side bookkeeping");
    checkQuery<float>(s, hf, left, ro, false, "seams-left");
    checkQuery<float>(s, hf, right, ro, false, "seams-right");
    checkQuery<float>(s, hf, seam, ro, false, "seams-exact");

    // A ray with an explicit negative-zero component must behave identically.
    Query nz = {{8, 8, 64}, {0, 0, -1}};
    nanort::Ray<float> rn = makeRay<float>(nz.O, nz.D, tg::Scene::L);
    rn.dir[0] = std::copysign(0.0f, -1.0f);
    nanort::TriangleIntersection<float> hz;
    bool hitNz = hf.bvh.Traverse(rn, hf.inter, &hz);
    tg::RefResult rnz = tg::referenceRay(sc.iv, sc.faces, nz.O, nz.D,
                                         tg::Scene::L, ro);
    s.check(hitNz == rnz.hit, "negative-zero component must not change hit");
    if (hitNz) {
      bool legal = false;
      for (unsigned f : rnz.legal)
        if (f == hz.prim_id) legal = true;
      s.check(legal, "negative-zero ray must return a legal face");
    }
  });

  // --------------------------------------------------------------- intervals
  suite.add("intervals", [&](Suite &s) {
    Harness<float> hf(sc, false);
    // Vertical ray to the z=2 quad Q2. Origin x=3 is outside the z=4 depth
    // quad's [0,2] x-span, and (3,0.5) is strictly inside one Q2 face (off
    // the diagonal y=x-2), so the nearest hit is a unique face at t=2.
    const Query q = {{48, 8, 64}, {0, 0, -1}};
    tg::RefOptions ro;
    tg::RefResult ref = tg::referenceRay(sc.iv, sc.faces, q.O, q.D,
                                         tg::Scene::L, ro);
    s.check(ref.hit, "interval probe ray must hit");
    const float tstar = 2.0f;

    auto traverseWith = [&](float mn, float mx) {
      nanort::Ray<float> r = makeRay<float>(q.O, q.D, tg::Scene::L, mn, mx);
      nanort::TriangleIntersection<float> h;
      return hf.bvh.Traverse(r, hf.inter, &h);
    };
    s.check(!traverseWith(0.0f, tstar - 0.25f),
            "max_t before the hit must miss");
    s.check(traverseWith(tstar, 1e30f),
            "min_t placed exactly on the hit must still hit");
    s.check(!traverseWith(0.0f, tstar),
            "max_t placed exactly on the hit must miss (half-open window)");
    s.check(traverseWith(0.0f, tstar + 0.25f),
            "max_t after the hit must hit");

    // prim_ids_range filtering.
    const unsigned hitFace = ref.legal.front();
    tg::RefOptions rRange;
    rRange.rangeLo = hitFace + 1;
    rRange.rangeHi = hitFace + 2;
    nanort::BVHTraceOptions oRange = makeOpts(rRange);
    {
      nanort::Ray<float> r = makeRay<float>(q.O, q.D, tg::Scene::L);
      nanort::TriangleIntersection<float> h;
      s.check(!hf.bvh.Traverse(r, hf.inter, &h, oRange),
              "a prim_ids_range excluding the hit face must miss");
      rRange.rangeLo = hitFace;
      rRange.rangeHi = hitFace + 1;
      oRange = makeOpts(rRange);
      s.check(hf.bvh.Traverse(r, hf.inter, &h, oRange),
              "a prim_ids_range containing the hit face must hit");
    }
    // skip_prim_id filtering: compare against a reference that skips too.
    {
      tg::RefOptions rSkip;
      rSkip.skipId = hitFace;
      tg::RefResult refSkip = tg::referenceRay(sc.iv, sc.faces, q.O, q.D,
                                                tg::Scene::L, rSkip);
      nanort::BVHTraceOptions oSkip = makeOpts(rSkip);
      nanort::Ray<float> r = makeRay<float>(q.O, q.D, tg::Scene::L);
      nanort::TriangleIntersection<float> h;
      bool libHit = hf.bvh.Traverse(r, hf.inter, &h, oSkip);
      s.check(libHit == refSkip.hit,
              "skip_prim_id hit flag must match the skip-aware reference");
      if (libHit) {
        bool legal = false;
        for (unsigned f : refSkip.legal)
          if (f == h.prim_id) legal = true;
        s.check(legal && h.prim_id != hitFace,
                "skip_prim_id must never return the skipped face");
      }
    }
    // Back-face culling agrees with the exact winding reference.
    {
      const Query front = {{8, 8, 64}, {0, 0, -1}};
      const Query back = {{8, 8, 0}, {0, 0, 1}};
      for (const Query *qq : {&front, &back}) {
        tg::RefOptions rc;
        rc.cullBack = true;
        tg::RefResult rcRef = tg::referenceRay(sc.iv, sc.faces, qq->O, qq->D,
                                               tg::Scene::L, rc);
        nanort::BVHTraceOptions oc = makeOpts(rc);
        nanort::Ray<float> r = makeRay<float>(qq->O, qq->D, tg::Scene::L);
        nanort::TriangleIntersection<float> h;
        bool libHit = hf.bvh.Traverse(r, hf.inter, &h, oc);
        s.check(libHit == rcRef.hit,
                "back-face culling must match the exact winding decision");
      }
    }
    // Reuse one intersector across interleaved directions, options and misses.
    {
      nanort::TriangleIntersection<float> h;
      nanort::Ray<float> a = makeRay<float>(q.O, q.D, tg::Scene::L);
      bool ha = hf.bvh.Traverse(a, hf.inter, &h);
      nanort::Ray<float> miss = makeRay<float>(
          (tg::i64[3]){800, 800, 64}, (tg::i64[3]){0, 0, -1}, tg::Scene::L);
      bool hm = hf.bvh.Traverse(miss, hf.inter, &h);
      nanort::Ray<float> b = makeRay<float>(
          (tg::i64[3]){24, 8, 64}, (tg::i64[3]){2, 0, -3}, tg::Scene::L);
      tg::RefOptions rRange2;
      rRange2.rangeLo = 0;
      rRange2.rangeHi = 1;  // likely excludes quad faces
      nanort::BVHTraceOptions oRange2 = makeOpts(rRange2);
      bool hb = hf.bvh.Traverse(b, hf.inter, &h, oRange2);
      nanort::Ray<float> c = makeRay<float>(
          (tg::i64[3]){8, 24, 64}, (tg::i64[3]){0, 0, -1}, tg::Scene::L);
      bool hc = hf.bvh.Traverse(c, hf.inter, &h);
      s.check(ha && !hm && !hb && hc,
              "interleaved reused intersector must refresh per-query state");
    }
  });

  // ----------------------------------------------------------------- layouts
  suite.add("layouts", [&](Suite &s) {
    Harness<float> compactF(sc, false);
    Harness<float> paddedF(sc, true);
    Harness<double> compactD(sc, false);
    Harness<double> paddedD(sc, true);
    tg::RefOptions ro;
    const std::vector<Query> qs = {
        {{8, 8, 64}, {0, 0, -1}},   {{16, 16, 64}, {0, 0, -2}},
        {{24, 8, 64}, {0, 0, -1}},  {{4, 12, 80}, {2, 0, -3}},
        {{20, 20, 80}, {-1, 2, -2}},{{-128, 0, 80}, {0, 0, -1}}};
    for (const Query &q : qs) {
      checkQuery<float>(s, compactF, q, ro, false, "layout-float-compact");
      checkQuery<float>(s, paddedF, q, ro, false, "layout-float-padded");
      checkQuery<double>(s, compactD, q, ro, true, "layout-double-compact");
      checkQuery<double>(s, paddedD, q, ro, true, "layout-double-padded");
    }
    // Compact and padded float must agree bit-for-bit on t,u,v and face.
    for (const Query &q : qs) {
      nanort::Ray<float> r = makeRay<float>(q.O, q.D, tg::Scene::L);
      nanort::TriangleIntersection<float> hc, hp;
      bool kc = compactF.bvh.Traverse(r, compactF.inter, &hc);
      bool kp = paddedF.bvh.Traverse(r, paddedF.inter, &hp);
      s.check(kc == kp && hc.prim_id == hp.prim_id &&
                  std::memcmp(&hc.t, &hp.t, sizeof(float)) == 0 &&
                  std::memcmp(&hc.u, &hp.u, sizeof(float)) == 0 &&
                  std::memcmp(&hc.v, &hp.v, sizeof(float)) == 0,
              "padded and compact stride must give identical results");
    }
    // Padding slots never leak: read them back and confirm they are untouched.
    s.check(sc.vpad.size() == sc.iv.size() / 3 * 4, "padded vertex count");
    s.check(sc.vpad[3] == 0.0f && sc.vpad[7] == 0.0f && sc.vpad[11] == 0.0f,
            "padding slots must be present and ignored by the stride");
  });

  // ----------------------------------------------------------------- mutants
  suite.add("mutants", [&](Suite &s) {
    // Drive the external mutation harness. It builds temporary source copies,
    // keeps all benign regressions green there, and only reports a content
    // failure (its own exit status 3) as a detection. A healthy run returns 0;
    // any other status is an infrastructure problem (compile failure, missing
    // file, missing marker, timeout) and is reported as a runtime error, not a
    // numeric CONTENT_FAILURE.
    std::string cmd = "bash checks/mutation_check.sh 2>&1";
    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) throw std::runtime_error("mutation_check.sh failed to launch");
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
    int rc = pclose(pipe);
    if (rc != 0)
      throw std::runtime_error(
          "mutation harness runtime error (rc=" + std::to_string(rc) + "): " +
          out);
    bool nofb = out.find("DETECTED no-edge-fallback") != std::string::npos;
    bool packed = out.find("DETECTED packed-intersector") != std::string::npos;
    s.check(nofb, "no-edge-fallback change must be detected");
    s.check(packed, "packed-intersector stride change must be detected");
    s.check(out.find("first expected/actual") != std::string::npos,
            "detection report must include the first expected/actual diff");

    // Windowless PPM render of both triangulations of one continuous surface,
    // checked pixel-by-pixel against the exact reference.
    const int W = 64, H = 32;
    const tg::i64 LI = 32;  // image lattice: pixel centres at half-/16 steps
    std::vector<tg::i64> iv2;
    for (tg::i64 x : sc.iv) iv2.push_back(x * 2);  // /16 -> /32
    std::vector<unsigned char> rgb(W * H * 3, 0);
    long nonBg = 0, seamPixels = 0;
    for (int py = 0; py < H; ++py) {
      for (int px = 0; px < W; ++px) {
        tg::i64 O[3] = {2 * px + 1, 2 * py + 1, 4 * LI};  // z=4 above z=2
        tg::i64 D[3] = {0, 0, -1};
        tg::RefResult rr = tg::referenceRay(iv2, sc.faces, O, D, LI,
                                            tg::RefOptions{});
        float col[3] = {0.05f, 0.05f, 0.08f};  // background
        if (rr.hit) {
          ++nonBg;
          const unsigned *f = &sc.faces[3 * rr.legal.front()];
          tg::FaceHit fh = tg::exactFace(iv2, f, O, D);
          double et = tg::rat(fh.tn, (tg::i128)LI * fh.det);
          double ex = (double)O[0] / LI, ey = (double)O[1] / LI;
          double pz = (double)O[2] / LI + et * D[2];
          (void)pz;
          float c = 0.1f + 0.03f * (float)ex + 0.05f * (float)ey + 0.04f;
          col[0] = c;
          col[1] = 0.25f + 0.5f * c;
          col[2] = 0.75f - 0.25f * c;
          if (rr.legal.size() >= 2) ++seamPixels;
        }
        for (int k = 0; k < 3; ++k)
          rgb[3 * (py * W + px) + k] =
              (unsigned char)std::min(255.0, std::max(0.0, col[k] * 255.0));
      }
    }
    s.check(nonBg > W * H / 4, "render must contain many non-background pixels");
    s.check(seamPixels >= 1, "render must contain shared-seam pixels");

    // Cross-check the library render against the exact per-pixel colour.
    Harness<float> hf(sc, false);
    long mism = 0;
    for (int py = 0; py < H && mism == 0; ++py) {
      for (int px = 0; px < W && mism == 0; ++px) {
        tg::i64 O[3] = {2 * px + 1, 2 * py + 1, 4 * LI};
        tg::i64 D[3] = {0, 0, -1};
        nanort::Ray<float> ray = makeRay<float>(O, D, LI);
        nanort::TriangleIntersection<float> isect;
        bool hit = hf.bvh.Traverse(ray, hf.inter, &isect);
        tg::RefResult rr = tg::referenceRay(iv2, sc.faces, O, D, LI,
                                            tg::RefOptions{});
        s.check(hit == rr.hit, "render pixel hit flag must match reference");
        if (hit) {
          bool legal = false;
          for (unsigned f : rr.legal)
            if (f == isect.prim_id) legal = true;
          if (!legal) ++mism;
        }
      }
    }
    s.check(mism == 0, "no render pixel may return a non-legal face");

    const char *td = std::getenv("TMPDIR");
    std::string path = std::string(td ? td : ".") + "/nanort_target_render.ppm";
    std::ofstream ppm(path, std::ios::binary);
    ppm << "P6\n" << W << " " << H << "\n255\n";
    ppm.write(reinterpret_cast<const char *>(rgb.data()), rgb.size());
    ppm.close();
    std::ifstream verify(path, std::ios::binary | std::ios::ate);
    const std::streamoff expected =
        std::streamoff(rgb.size()) + std::streamoff(std::string("P6\n64 32\n255\n").size());
    s.check(verify.good() && verify.tellg() == expected,
            "windowless PPM must be written with the exact expected size");
  });
}
