// Standalone mutation probe. It is compiled against a temporary nanort.h
// source copy and asserts the exact-reference behaviour of the production
// intersector. A genuine content failure prints a DETECTED marker with the
// first expected/actual difference and exits with status 3 (the same
// CONTENT_FAILURE contract as the main suite driver).
#include "nanort.h"
#include "target_geometry.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace tg = targetgeom;

namespace {
struct ContentFailure {
  std::string msg;
};

double faceT(const tg::FaceHit &h, tg::i64 L) {
  return tg::rat(h.tn, (tg::i128)L * h.det);
}
}  // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: mutation_driver {no-edge-fallback|packed-intersector}\n");
    return 1;
  }
  const std::string mode = argv[1];
  try {
    if (mode == "no-edge-fallback") {
      // Tall thin needle. On the probe ray the V edge function's two nonzero
      // float products round to the same value
      //   fl(Ax*Cy) = fl(Ay*Cx) = 2863311616  ->  float V = 0
      // while their exact double difference is nonzero (0.02587890625). The
      // double edge fallback recovers the tiny V and keeps the distance on the
      // exact rational t=2; with the fallback disabled V stays zero and the
      // distance drifts grossly (~2.10), a hit at the wrong position.
      std::vector<float> v;
      std::vector<unsigned> f;
      std::vector<tg::i64> iv;
      tg::tallNeedle(v, f, iv);
      const tg::i64 L = 16;
      tg::i64 O[3] = {-64, -63, 96}, D[3] = {2, -3, -3};
      tg::FaceHit fh = tg::exactFace(iv, f.data(), O, D);
      if (!fh.hit) throw std::string("reference setup must hit");
      const double expT = faceT(fh, L);
      const double expU = tg::rat(fh.un, fh.det);
      const double expV = tg::rat(fh.vn, fh.det);

      nanort::TriangleMesh<float> mesh(v.data(), f.data(), 3 * sizeof(float));
      nanort::TriangleSAHPred<float> pred(v.data(), f.data(), 3 * sizeof(float));
      nanort::BVHAccel<float> bvh;
      if (!bvh.Build(1, mesh, pred)) throw std::string("BVH build failed");
      nanort::TriangleIntersector<float> inter(v.data(), f.data(),
                                                3 * sizeof(float));
      nanort::Ray<float> ray;
      for (int i = 0; i < 3; ++i) {
        ray.org[i] = float((long double)O[i] / (long double)L);
        ray.dir[i] = float(D[i]);
      }
      ray.min_t = 0.0f;
      ray.max_t = 1e30f;
      nanort::TriangleIntersection<float> h;
      bool hit = bvh.Traverse(ray, inter, &h);
      if (!hit) {
        throw std::string("DETECTED no-edge-fallback first expected/actual: "
                          "hit expected 1 actual 0 (edge ray dropped)");
      }
      double relT = std::fabs((double)h.t - expT) / std::fabs(expT);
      double du = std::fabs((double)h.u - expU);
      double dv = std::fabs((double)h.v - expV);
      if (relT > 2e-4 || du > 2e-5 || dv > 2e-5) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "DETECTED no-edge-fallback first expected/actual: "
                      "t expected %.9g actual %.9g (rel %.4g); u expected "
                      "%.9g actual %.9g; v expected %.9g actual %.9g",
                      expT, (double)h.t, relT, expU, (double)h.u, expV,
                      (double)h.v);
        throw std::string(buf);
      }
      std::puts("no-edge-fallback probe unexpectedly passed");
      return 0;
    }

    if (mode == "packed-intersector") {
      // Padded vertex layout. Mesh/SAHP keep the caller stride; only a broken
      // intersector that hard-codes 3*sizeof(T) reads the padding slots.
      std::vector<float> vpad;
      std::vector<unsigned> f;
      std::vector<tg::i64> iv16;
      tg::paddedNeedle(vpad, f, iv16);
      const size_t stride = 4 * sizeof(float);
      // Ray on the /256 lattice: O=(1/256,0,4), straight down.
      const tg::i64 L = 256;
      std::vector<tg::i64> iv256;
      for (tg::i64 x : iv16) iv256.push_back(x * 16);  // /16 -> /256
      tg::i64 O[3] = {1, 0, 1024}, D[3] = {0, 0, -1};
      tg::FaceHit fh = tg::exactFace(iv256, f.data(), O, D);
      if (!fh.hit) throw std::string("reference setup must hit");
      const double expU = tg::rat(fh.un, fh.det);
      const double expV = tg::rat(fh.vn, fh.det);
      const double expT = faceT(fh, L);

      nanort::TriangleMesh<float> mesh(vpad.data(), f.data(), stride);
      nanort::TriangleSAHPred<float> pred(vpad.data(), f.data(), stride);
      nanort::BVHAccel<float> bvh;
      if (!bvh.Build(1, mesh, pred)) throw std::string("BVH build failed");
      nanort::TriangleIntersector<float> inter(vpad.data(), f.data(), stride);
      nanort::Ray<float> ray;
      ray.org[0] = 1.0f / 256.0f;
      ray.org[1] = 0.0f;
      ray.org[2] = 4.0f;
      ray.dir[0] = 0.0f;
      ray.dir[1] = 0.0f;
      ray.dir[2] = -1.0f;
      ray.min_t = 0.0f;
      ray.max_t = 1e30f;
      nanort::TriangleIntersection<float> h;
      bool hit = bvh.Traverse(ray, inter, &h);
      if (!hit) {
        throw std::string("DETECTED packed-intersector first expected/actual: "
                          "hit expected 1 actual 0");
      }
      double du = std::fabs((double)h.u - expU);
      double dv = std::fabs((double)h.v - expV);
      double relT = std::fabs((double)h.t - expT) / std::fabs(expT);
      if (du > 2e-5 || dv > 2e-5 || relT > 2e-4) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "DETECTED packed-intersector first expected/actual: "
                      "u expected %.9g actual %.9g (du %.4g); v expected "
                      "%.9g actual %.9g; t expected %.9g actual %.9g",
                      expU, (double)h.u, du, expV, (double)h.v, expT,
                      (double)h.t);
        throw std::string(buf);
      }
      std::puts("packed-intersector probe unexpectedly passed");
      return 0;
    }

    std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
    return 1;
  } catch (const ContentFailure &) {
    return 3;
  } catch (const std::string &e) {
    std::fprintf(stderr, "CONTENT_FAILURE %s\n", e.c_str());
    return 3;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "runtime error: %s\n", e.what());
    return 1;
  }
}
