//
// Bilinear patch primitive test suite.
//
// Exercises nanort-patch.h through BVHAccel::Build()/Traverse():
//   - planar / near-planar reference cases (no NaN from tiny quadratic term)
//   - two roots on one patch, non-unit ray direction, trimming away the
//     nearest root
//   - parametric hole: strict interior excluded, boundary kept
//   - BVH behaviour: nearest hit across faces, skip_prim_id, back-face
//     culling, prim id range, bounding box, state reset between rays
//   - window-less rendering example writing bilinear.ppm
//
// Prints one marker line per group:
//   BILINEAR_PLANAR_OK / BILINEAR_TWO_ROOTS_OK / BILINEAR_TRIM_OK /
//   BILINEAR_BVH_OK / BILINEAR_RENDER_OK
//
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "nanort.h"
#include "nanort-patch.h"

#include "examples/bilinear_patch/render.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char *expr, int line) {
  if (!ok) {
    std::cerr << "CHECK failed at line " << line << ": " << expr << std::endl;
    g_failures++;
  }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// Builds a single-patch BVH and traces one ray. Returns Traverse()'s result.
template <typename T>
bool TraceSingle(const T *vertices, const unsigned int *faces,
                 const nanort::Ray<T> &ray,
                 nanort::BilinearPatchIntersection<T> *isect,
                 const nanort::BVHTraceOptions &options,
                 const T *trims = NULL) {
  nanort::BilinearPatchMesh<T> mesh(vertices, faces, 3 * sizeof(T), trims);
  nanort::BilinearPatchSAHPred<T> pred(vertices, faces, 3 * sizeof(T));
  nanort::BVHAccel<T> accel;
  if (!accel.Build(1, mesh, pred)) {
    return false;
  }
  nanort::BilinearPatchIntersector<T> intersector(mesh);
  return accel.Traverse(ray, intersector, isect, options);
}

template <typename T>
nanort::Ray<T> MakeRay(T ox, T oy, T oz, T dx, T dy, T dz, T max_t) {
  nanort::Ray<T> ray;
  ray.org[0] = ox;
  ray.org[1] = oy;
  ray.org[2] = oz;
  ray.dir[0] = dx;
  ray.dir[1] = dy;
  ray.dir[2] = dz;
  ray.min_t = static_cast<T>(0.0);
  ray.max_t = max_t;
  return ray;
}

void TestPlanar() {
  // 1) Planar quad, non-unit ray direction: t keeps the caller's
  //    parameterization (z: 3 - 6 t = 0 -> t = 0.5).
  {
    const double v[] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    const unsigned int f[] = {0, 1, 2, 3};
    nanort::BilinearPatchIntersection<double> hit;
    nanort::Ray<double> ray = MakeRay<double>(0.25, -0.5, 3, 0, 0, -6, 10);
    CHECK(TraceSingle(v, f, ray, &hit, nanort::BVHTraceOptions()));
    CHECK(Near(hit.t, 0.5, 1e-12));
    CHECK(Near(hit.u, 0.625, 1e-12));
    CHECK(Near(hit.v, 0.25, 1e-12));
    CHECK(hit.prim_id == 0);
    CHECK(Near(hit.normal[0], 0.0, 1e-12));
    CHECK(Near(hit.normal[1], 0.0, 1e-12));
    CHECK(Near(hit.normal[2], 1.0, 1e-12));  // oriented towards the ray
  }

  // 2) Non-coplanar corners from checks/baseline.cpp: the triangle path
  //    reports t = 2 at the center; the true bilinear surface gives t = 2.5.
  {
    const double v[] = {-1, -1, 0, 1, -1, 0, 1, 1, 2, -1, 1, 0};
    const unsigned int f[] = {0, 1, 2, 3};
    nanort::BilinearPatchIntersection<double> hit;
    nanort::Ray<double> ray = MakeRay<double>(0, 0, 3, 0, 0, -1, 10);
    CHECK(TraceSingle(v, f, ray, &hit, nanort::BVHTraceOptions()));
    CHECK(Near(hit.t, 2.5, 1e-12));
    CHECK(Near(hit.u, 0.5, 1e-12));
    CHECK(Near(hit.v, 0.5, 1e-12));
  }

  // 3) Near-planar patch (tiny quadratic term): must not produce NaN and
  //    must not lose the finite intersection.
  {
    const double v[] = {-1, -1, 0, 1, -1, 0, 1, 1, 1e-28, -1, 1, 0};
    const unsigned int f[] = {0, 1, 2, 3};
    nanort::BilinearPatchIntersection<double> hit;
    nanort::Ray<double> ray = MakeRay<double>(0, 0, 3, 0, 0, -1, 10);
    CHECK(TraceSingle(v, f, ray, &hit, nanort::BVHTraceOptions()));
    CHECK(std::isfinite(hit.t));
    CHECK(Near(hit.t, 3.0, 1e-20));
    CHECK(std::isfinite(hit.normal[0]) && std::isfinite(hit.normal[1]) &&
          std::isfinite(hit.normal[2]));
  }

  // 4) Affinely parameterized parallelogram (sheared, still planar).
  {
    const double v[] = {0, 0, 1, 2, 0, 1, 3, 1, 1, 1, 1, 1};
    const unsigned int f[] = {0, 1, 2, 3};
    nanort::BilinearPatchIntersection<double> hit;
    nanort::Ray<double> ray = MakeRay<double>(1.5, 0.5, 4, 0, 0, -1, 10);
    CHECK(TraceSingle(v, f, ray, &hit, nanort::BVHTraceOptions()));
    CHECK(Near(hit.t, 3.0, 1e-12));
    CHECK(Near(hit.u, 0.5, 1e-12));
    CHECK(Near(hit.v, 0.5, 1e-12));
  }

  // 5) Same planar reference in single precision.
  {
    const float v[] = {-1, -1, 0, 1, -1, 0, 1, 1, 0, -1, 1, 0};
    const unsigned int f[] = {0, 1, 2, 3};
    nanort::BilinearPatchIntersection<float> hit;
    nanort::Ray<float> ray = MakeRay<float>(0.25f, -0.5f, 3.0f, 0, 0, -6.0f, 10.0f);
    CHECK(TraceSingle(v, f, ray, &hit, nanort::BVHTraceOptions()));
    CHECK(Near(hit.t, 0.5, 1e-5));
    CHECK(Near(hit.u, 0.625, 1e-5));
    CHECK(Near(hit.v, 0.25, 1e-5));
    CHECK(Near(hit.normal[2], 1.0, 1e-5));
  }

  if (g_failures == 0) {
    std::cout << "BILINEAR_PLANAR_OK" << std::endl;
  }
}

void TestTwoRoots() {
  // Hyperbolic paraboloid P(u,v) = (u, v, uv).
  const double v[] = {0, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 0};
  const unsigned int f[] = {0, 1, 2, 3};

  // Ray (0,0,-0.16) + t(1,1,1) crosses the surface at t = 0.2 and t = 0.8.
  {
    nanort::BilinearPatchIntersection<double> hit;
    nanort::Ray<double> ray = MakeRay<double>(0, 0, -0.16, 1, 1, 1, 10);
    CHECK(TraceSingle(v, f, ray, &hit, nanort::BVHTraceOptions()));
    CHECK(Near(hit.t, 0.2, 1e-12));
    CHECK(Near(hit.u, 0.2, 1e-12));
    CHECK(Near(hit.v, 0.2, 1e-12));
  }

  // Non-unit direction: t scales accordingly (roots at t = 0.1 and 0.4),
  // parameter coordinates stay the same.
  {
    nanort::BilinearPatchIntersection<double> hit;
    nanort::Ray<double> ray = MakeRay<double>(0, 0, -0.16, 2, 2, 2, 10);
    CHECK(TraceSingle(v, f, ray, &hit, nanort::BVHTraceOptions()));
    CHECK(Near(hit.t, 0.1, 1e-12));
    CHECK(Near(hit.u, 0.2, 1e-12));
    CHECK(Near(hit.v, 0.2, 1e-12));
  }

  // Cut a hole around the first root: the second root must still be found.
  {
    const double trims[] = {0.125, 0.125, 0.25, 0.25};
    nanort::BilinearPatchIntersection<double> hit;
    nanort::Ray<double> ray = MakeRay<double>(0, 0, -0.16, 1, 1, 1, 10);
    CHECK(TraceSingle(v, f, ray, &hit, nanort::BVHTraceOptions(), trims));
    CHECK(Near(hit.t, 0.8, 1e-12));
    CHECK(Near(hit.u, 0.8, 1e-12));
    CHECK(Near(hit.v, 0.8, 1e-12));
  }

  // Ray interval: max_t between the two roots keeps only the first.
  {
    nanort::BilinearPatchIntersection<double> hit;
    nanort::Ray<double> ray = MakeRay<double>(0, 0, -0.16, 1, 1, 1, 0.5);
    CHECK(TraceSingle(v, f, ray, &hit, nanort::BVHTraceOptions()));
    CHECK(Near(hit.t, 0.2, 1e-12));
  }
  // max_t below both roots: no hit.
  {
    nanort::BilinearPatchIntersection<double> hit;
    nanort::Ray<double> ray = MakeRay<double>(0, 0, -0.16, 1, 1, 1, 0.1);
    CHECK(!TraceSingle(v, f, ray, &hit, nanort::BVHTraceOptions()));
  }

  if (g_failures == 0) {
    std::cout << "BILINEAR_TWO_ROOTS_OK" << std::endl;
  }
}

void TestTrim() {
  // Unit planar quad with a centered parametric hole (0.25,0.25)-(0.75,0.75).
  const double v[] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
  const unsigned int f[] = {0, 1, 2, 3};
  const double trims[] = {0.25, 0.25, 0.75, 0.75};

  // Strict interior of the hole: miss.
  {
    nanort::BilinearPatchIntersection<double> hit;
    nanort::Ray<double> ray = MakeRay<double>(0.5, 0.5, 1, 0, 0, -1, 10);
    CHECK(!TraceSingle(v, f, ray, &hit, nanort::BVHTraceOptions(), trims));
  }
  // Hole boundary (u == u0): boundary belongs to the surface -> hit.
  {
    nanort::BilinearPatchIntersection<double> hit;
    nanort::Ray<double> ray = MakeRay<double>(0.25, 0.5, 1, 0, 0, -1, 10);
    CHECK(TraceSingle(v, f, ray, &hit, nanort::BVHTraceOptions(), trims));
    CHECK(Near(hit.t, 1.0, 1e-12));
    CHECK(Near(hit.u, 0.25, 1e-12));
    CHECK(Near(hit.v, 0.5, 1e-12));
  }
  // Hole corner (u0, v0): also surface -> hit.
  {
    nanort::BilinearPatchIntersection<double> hit;
    nanort::Ray<double> ray = MakeRay<double>(0.25, 0.25, 1, 0, 0, -1, 10);
    CHECK(TraceSingle(v, f, ray, &hit, nanort::BVHTraceOptions(), trims));
    CHECK(Near(hit.u, 0.25, 1e-12));
    CHECK(Near(hit.v, 0.25, 1e-12));
  }
  // Outside the hole: hit.
  {
    nanort::BilinearPatchIntersection<double> hit;
    nanort::Ray<double> ray = MakeRay<double>(0.1, 0.5, 1, 0, 0, -1, 10);
    CHECK(TraceSingle(v, f, ray, &hit, nanort::BVHTraceOptions(), trims));
    CHECK(Near(hit.u, 0.1, 1e-12));
  }
  // Same rays without trims all hit.
  {
    nanort::BilinearPatchIntersection<double> hit;
    nanort::Ray<double> ray = MakeRay<double>(0.5, 0.5, 1, 0, 0, -1, 10);
    CHECK(TraceSingle(v, f, ray, &hit, nanort::BVHTraceOptions()));
    CHECK(Near(hit.t, 1.0, 1e-12));
  }

  if (g_failures == 0) {
    std::cout << "BILINEAR_TRIM_OK" << std::endl;
  }
}

void TestBVH() {
  // Two stacked unit quads: face 0 at z = 0, face 1 at z = 1.
  const double v[] = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0,  // face 0
                      0, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 1};  // face 1
  const unsigned int f[] = {0, 1, 2, 3, 4, 5, 6, 7};

  nanort::BilinearPatchMesh<double> mesh(v, f, 3 * sizeof(double));
  nanort::BilinearPatchSAHPred<double> pred(v, f, 3 * sizeof(double));
  nanort::BVHAccel<double> accel;
  CHECK(accel.Build(2, mesh, pred));

  // BVH bounding box must cover the whole surface (both layers).
  {
    double bmin[3], bmax[3];
    accel.BoundingBox(bmin, bmax);
    CHECK(bmin[2] <= 0.0 && bmax[2] >= 1.0);
    CHECK(bmin[0] <= 0.0 && bmax[0] >= 1.0);
    CHECK(bmin[1] <= 0.0 && bmax[1] >= 1.0);
  }

  nanort::BilinearPatchIntersector<double> intersector(mesh);
  nanort::BilinearPatchIntersection<double> hit;

  // Nearest hit across multiple faces; prim_id refers to the input face.
  {
    nanort::Ray<double> ray = MakeRay<double>(0.3, 0.3, 3, 0, 0, -1, 10);
    CHECK(accel.Traverse(ray, intersector, &hit));
    CHECK(hit.prim_id == 1);
    CHECK(Near(hit.t, 2.0, 1e-12));
    CHECK(Near(hit.normal[2], 1.0, 1e-12));
  }

  // skip_prim_id skips the nearer face.
  {
    nanort::BVHTraceOptions opt;
    opt.skip_prim_id = 1;
    nanort::Ray<double> ray = MakeRay<double>(0.3, 0.3, 3, 0, 0, -1, 10);
    CHECK(accel.Traverse(ray, intersector, &hit, opt));
    CHECK(hit.prim_id == 0);
    CHECK(Near(hit.t, 3.0, 1e-12));
  }

  // prim_ids_range restricts traversal to a subset of input faces.
  {
    nanort::BVHTraceOptions opt;
    opt.prim_ids_range[0] = 0;
    opt.prim_ids_range[1] = 1;  // only face 0
    nanort::Ray<double> ray = MakeRay<double>(0.3, 0.3, 3, 0, 0, -1, 10);
    CHECK(accel.Traverse(ray, intersector, &hit, opt));
    CHECK(hit.prim_id == 0);
    CHECK(Near(hit.t, 3.0, 1e-12));
  }

  // Back-face culling: ray from below hits the back side of face 0.
  {
    nanort::Ray<double> ray = MakeRay<double>(0.3, 0.3, -1, 0, 0, 1, 10);
    nanort::BVHTraceOptions opt;
    opt.cull_back_face = true;
    CHECK(!accel.Traverse(ray, intersector, &hit, opt));
    // Without culling it hits, with the normal flipped towards the ray.
    CHECK(accel.Traverse(ray, intersector, &hit));
    CHECK(hit.prim_id == 0);
    CHECK(Near(hit.t, 1.0, 1e-12));
    CHECK(Near(hit.normal[2], -1.0, 1e-12));
  }

  // Same intersector, consecutive different rays: no stale state. A ray
  // that misses everything must report a miss right after a hit.
  {
    nanort::Ray<double> ray_hit = MakeRay<double>(0.3, 0.3, 3, 0, 0, -1, 10);
    CHECK(accel.Traverse(ray_hit, intersector, &hit));
    nanort::Ray<double> ray_miss = MakeRay<double>(5, 5, 3, 0, 0, -1, 10);
    CHECK(!accel.Traverse(ray_miss, intersector, &hit));
    // And a following hit still reports correct, fresh parameters.
    nanort::Ray<double> ray_hit2 = MakeRay<double>(0.7, 0.7, 3, 0, 0, -1, 10);
    CHECK(accel.Traverse(ray_hit2, intersector, &hit));
    CHECK(hit.prim_id == 1);
    CHECK(Near(hit.t, 2.0, 1e-12));
    CHECK(Near(hit.u, 0.7, 1e-12));
    CHECK(Near(hit.v, 0.7, 1e-12));
  }

  // Bounding box of a non-coplanar patch covers the lifted corner.
  {
    const double w[] = {-1, -1, 0, 1, -1, 0, 1, 1, 2, -1, 1, 0};
    const unsigned int wf[] = {0, 1, 2, 3};
    nanort::BilinearPatchMesh<double> mesh2(w, wf, 3 * sizeof(double));
    nanort::BilinearPatchSAHPred<double> pred2(w, wf, 3 * sizeof(double));
    nanort::BVHAccel<double> accel2;
    CHECK(accel2.Build(1, mesh2, pred2));
    double bmin[3], bmax[3];
    accel2.BoundingBox(bmin, bmax);
    CHECK(bmax[2] >= 2.0);
  }

  if (g_failures == 0) {
    std::cout << "BILINEAR_BVH_OK" << std::endl;
  }
}

void TestRender(const std::string &tmpdir) {
  const std::string path = tmpdir + "/bilinear.ppm";
  CHECK(bilinear_patch_example::RenderBilinearPatchExample(path.c_str()));

  // Verify the PPM exists, is non-empty, and has a sane P6 header.
  FILE *fp = std::fopen(path.c_str(), "rb");
  CHECK(fp != NULL);
  if (fp) {
    char magic[3] = {0, 0, 0};
    CHECK(std::fread(magic, 1, 2, fp) == 2);
    CHECK(magic[0] == 'P' && magic[1] == '6');
    std::fseek(fp, 0, SEEK_END);
    const long size = std::ftell(fp);
    CHECK(size > 256 * 256 * 3 / 2);  // header + pixel data
    std::fclose(fp);
  }

  if (g_failures == 0) {
    std::cout << "BILINEAR_RENDER_OK" << std::endl;
  }
}

}  // namespace

int main(int argc, char **argv) {
  const std::string tmpdir = (argc > 1) ? argv[1] : ".";

  TestPlanar();
  TestTwoRoots();
  TestTrim();
  TestBVH();
  TestRender(tmpdir);

  if (g_failures != 0) {
    std::cerr << g_failures << " check(s) failed" << std::endl;
    return 1;
  }
  return 0;
}
