//
// Motion BVH checks for NanoRT, registered through task_support/check.h.
//
// Groups:
//   deformation : keyframed vertex deformation vs. independent per-face
//                 reference intersection, hit filters, padded vertices.
//   bounds      : per-key-time node bounds, query-time node bounds and
//                 time-varying node culling (Intersect call counting).
//   persistence : serialization round-trip, legacy static files and
//                 motion -> static rebuild on a single BVHAccel object.
//
#include "task_support/check.h"

#include <cmath>
#include <limits>
#include <vector>

#include "nanort_motion.h"

namespace {

template <typename T>
T hit_tol();

template <>
float hit_tol<float>() {
  return 2e-4f;
}

template <>
double hit_tol<double>() {
  return 1e-9;
}

template <typename T>
nanort::Ray<T> MakeRay(T ox, T oy, T oz, T dx, T dy, T dz, T time) {
  nanort::Ray<T> ray;
  ray.org[0] = ox;
  ray.org[1] = oy;
  ray.org[2] = oz;
  ray.dir[0] = dx;
  ray.dir[1] = dy;
  ray.dir[2] = dz;
  ray.min_t = static_cast<T>(0.0);
  ray.max_t = static_cast<T>(100.0);
  ray.time = time;
  return ray;
}

// Independent per-face reference: manually interpolates vertices at `time`
// and intersects every face with a plain Moeller-Trumbore routine.
template <typename T>
struct RefHit {
  bool hit;
  T t, u, v;
  unsigned int prim_id;
};

template <typename T>
RefHit<T> RefTrace(const T *times, size_t num_keys, const T *const *verts,
                   const unsigned int *faces, unsigned int num_faces,
                   size_t stride, const T org[3], const T dir[3], T min_t,
                   T max_t, T time) {
  RefHit<T> out;
  out.hit = false;
  out.t = max_t;
  out.u = static_cast<T>(0.0);
  out.v = static_cast<T>(0.0);
  out.prim_id = static_cast<unsigned int>(-1);

  size_t seg = 0;
  T alpha = static_cast<T>(0.0);
  if (!nanort::MotionTimeSegment(times, num_keys, time, &seg, &alpha)) {
    return out;
  }

  const T *va = verts[seg];
  const T *vb = verts[seg + 1];
  const T w0 = static_cast<T>(1.0) - alpha;
  const T w1 = alpha;

  T best = max_t;
  for (unsigned int p = 0; p < num_faces; ++p) {
    T P[3][3];
    for (int j = 0; j < 3; ++j) {
      const unsigned int f = faces[3 * p + j];
      const T *a = reinterpret_cast<const T *>(
          reinterpret_cast<const unsigned char *>(va) + f * stride);
      const T *b = reinterpret_cast<const T *>(
          reinterpret_cast<const unsigned char *>(vb) + f * stride);
      for (int k = 0; k < 3; ++k) {
        P[j][k] = w0 * a[k] + w1 * b[k];
      }
    }

    T e1[3] = {P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2]};
    T e2[3] = {P[2][0] - P[0][0], P[2][1] - P[0][1], P[2][2] - P[0][2]};

    T pv[3] = {dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2],
               dir[0] * e2[1] - dir[1] * e2[0]};
    T det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (det == static_cast<T>(0.0)) {
      continue;
    }
    T inv = static_cast<T>(1.0) / det;

    T tv[3] = {org[0] - P[0][0], org[1] - P[0][1], org[2] - P[0][2]};
    T u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) * inv;
    if (u < static_cast<T>(0.0) || u > static_cast<T>(1.0)) {
      continue;
    }

    T qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2],
               tv[0] * e1[1] - tv[1] * e1[0]};
    T v = (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]) * inv;
    if (v < static_cast<T>(0.0) || u + v > static_cast<T>(1.0)) {
      continue;
    }

    T t = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) * inv;
    if (t < min_t || t > best) {
      continue;
    }

    best = t;
    out.hit = true;
    out.t = t;
    out.u = u;
    out.v = v;
    out.prim_id = p;
  }

  return out;
}

template <typename T>
bool HitMatches(const nanort::TriangleIntersection<T> &h, const RefHit<T> &r) {
  if (h.prim_id != r.prim_id) {
    return false;
  }
  const T tol = hit_tol<T>();
  if (std::fabs(h.t - r.t) > tol * (static_cast<T>(1.0) + std::fabs(r.t))) {
    return false;
  }
  if (std::fabs(h.u - r.u) > tol || std::fabs(h.v - r.v) > tol) {
    return false;
  }
  return true;
}

// Intersector wrapper counting the number of primitive intersection tests,
// used to prove node-level culling happens before any leaf is reached.
template <typename T>
struct CountingIntersector {
  explicit CountingIntersector(const nanort::MotionTriangleMesh<T> &mesh)
      : iso(mesh), count(0) {}

  nanort::MotionTriangleIntersector<T> iso;
  mutable size_t count;

  bool Intersect(T *t, unsigned int prim_index) const {
    ++count;
    return iso.Intersect(t, prim_index);
  }
  T GetT() const { return iso.GetT(); }
  void Update(T t, unsigned int prim_idx) const { iso.Update(t, prim_idx); }
  void PrepareTraversal(const nanort::Ray<T> &ray,
                        const nanort::BVHTraceOptions &options) const {
    iso.PrepareTraversal(ray, options);
  }
  void PostTraversal(const nanort::Ray<T> &ray, bool hit,
                     nanort::TriangleIntersection<T> *isect) const {
    iso.PostTraversal(ray, hit, isect);
  }
};

// Quad (2 triangles, 4 vertices) deforming back and forth over 3 key frames
// with non-uniform time spacing {0, 0.3, 1}.
template <typename T>
struct FlexData {
  T times[3];
  std::vector<T> frame[3];
  const T *frame_ptrs[3];
  unsigned int faces[6];

  FlexData() {
    times[0] = static_cast<T>(0.0);
    times[1] = static_cast<T>(0.3);
    times[2] = static_cast<T>(1.0);

    const T coords[3][12] = {
        // t = 0.0 : flat quad at z = 2
        {T(-1), T(-1), T(2), T(1), T(-1), T(2), T(1), T(1), T(2), T(-1), T(1),
         T(2)},
        // t = 0.3 : shifted +0.5 in x, bottom edge z = 4, top edge z = 6
        {T(-0.5), T(-1), T(4), T(1.5), T(-1), T(4), T(1.5), T(1), T(6),
         T(-0.5), T(1), T(6)},
        // t = 1.0 : shifted -0.5 in x, flat quad at z = 1
        {T(-1.5), T(-1), T(1), T(0.5), T(-1), T(1), T(0.5), T(1), T(1),
         T(-1.5), T(1), T(1)}};

    for (int k = 0; k < 3; ++k) {
      frame[k].assign(coords[k], coords[k] + 12);
      frame_ptrs[k] = &frame[k][0];
    }

    faces[0] = 0;
    faces[1] = 1;
    faces[2] = 2;
    faces[3] = 0;
    faces[4] = 2;
    faces[5] = 3;
  }
};

template <typename T>
int RunFlex() {
  FlexData<T> d;
  nanort::MotionTriangleMesh<T> mesh(d.times, 3, d.frame_ptrs, d.faces, 2,
                                     3 * sizeof(T));

  nanort::Ray<T> default_ray;
  TASK_CHECK(default_ray.time == static_cast<T>(0.0));

  nanort::BVHAccel<T> accel;
  TASK_CHECK(accel.BuildMotion(mesh));
  TASK_CHECK(accel.HasMotion());
  TASK_CHECK(accel.GetNumMotionKeyTimes() == 3);

  nanort::MotionTriangleIntersector<T> isector(mesh);

  const T query_times[] = {T(0.0), T(0.15), T(0.3), T(0.65), T(1.0)};
  const T dirs[3][3] = {{T(0.0), T(0.0), T(1.0)},
                        {T(0.2), T(-0.1), T(1.0)},
                        {T(-0.3), T(0.4), T(1.0)}};

  unsigned int num_hits = 0;
  for (size_t ti = 0; ti < sizeof(query_times) / sizeof(query_times[0]);
       ++ti) {
    for (size_t ri = 0; ri < 3; ++ri) {
      nanort::Ray<T> ray = MakeRay<T>(T(0.0), T(0.0), T(0.0), dirs[ri][0],
                                      dirs[ri][1], dirs[ri][2],
                                      query_times[ti]);
      nanort::TriangleIntersection<T> hit;
      const bool h = accel.Traverse(ray, isector, &hit);

      const T org[3] = {T(0.0), T(0.0), T(0.0)};
      const RefHit<T> ref =
          RefTrace<T>(d.times, 3, d.frame_ptrs, d.faces, 2, 3 * sizeof(T), org,
                      ray.dir, ray.min_t, ray.max_t, query_times[ti]);

      TASK_CHECK(h == ref.hit);
      if (h && ref.hit) {
        TASK_CHECK(HitMatches(hit, ref));
        ++num_hits;
      }
    }
  }
  TASK_CHECK(num_hits > 0);

  // Exact key time query must use that key frame exactly: compare against
  // the static intersector bound to the key frame 1 vertices.
  {
    nanort::Ray<T> ray = MakeRay<T>(T(0.0), T(0.0), T(0.0), T(0.0), T(0.0),
                                    T(1.0), T(0.3));
    nanort::TriangleIntersection<T> hit;
    TASK_CHECK(accel.Traverse(ray, isector, &hit));

    nanort::TriangleIntersector<T> siso(d.frame_ptrs[1], d.faces,
                                        3 * sizeof(T));
    siso.PrepareTraversal(ray, nanort::BVHTraceOptions());
    T best = ray.max_t;
    for (unsigned int p = 0; p < 2; ++p) {
      T local_t = best;
      if (siso.Intersect(&local_t, p)) {
        best = local_t;
        siso.Update(best, p);
      }
    }
    TASK_CHECK(hit.t == best);
  }

  // Out-of-domain and non-finite times: miss without touching any primitive.
  {
    CountingIntersector<T> counting(mesh);
    const T bad_times[] = {T(-0.5), T(1.5),
                           std::numeric_limits<T>::quiet_NaN(),
                           std::numeric_limits<T>::infinity(),
                           -std::numeric_limits<T>::infinity()};
    for (size_t i = 0; i < sizeof(bad_times) / sizeof(bad_times[0]); ++i) {
      nanort::Ray<T> ray = MakeRay<T>(T(0.0), T(0.0), T(0.0), T(0.0), T(0.0),
                                      T(1.0), bad_times[i]);
      nanort::TriangleIntersection<T> hit;
      counting.count = 0;
      TASK_CHECK(!accel.Traverse(ray, counting, &hit));
      TASK_CHECK(counting.count == 0);
    }
  }

  // Invalid key time arrays must be rejected.
  {
    T dup_times[3] = {T(0.0), T(0.0), T(1.0)};
    nanort::MotionTriangleMesh<T> dup_mesh(dup_times, 3, d.frame_ptrs, d.faces,
                                           2, 3 * sizeof(T));
    nanort::BVHAccel<T> bad_accel;
    TASK_CHECK(!bad_accel.BuildMotion(dup_mesh));
    TASK_CHECK(!bad_accel.HasMotion());

    T nan_times[3] = {T(0.0), std::numeric_limits<T>::quiet_NaN(), T(1.0)};
    nanort::MotionTriangleMesh<T> nan_mesh(nan_times, 3, d.frame_ptrs, d.faces,
                                           2, 3 * sizeof(T));
    TASK_CHECK(!bad_accel.BuildMotion(nan_mesh));
  }

  return 0;
}

int FlexFloat() { return RunFlex<float>(); }
int FlexDouble() { return RunFlex<double>(); }

// Two triangles swapping their front/back order over the shutter interval.
int FrontBackSwap() {
  typedef float T;

  const T times[3] = {T(0.0), T(0.5), T(1.0)};
  // key 0: A at z=2, B at z=4; key 2: A at z=4, B at z=2 (order swapped).
  const T coords[3][18] = {
      {T(-1), T(-1), T(2), T(1), T(-1), T(2), T(0), T(1), T(2), T(-0.8),
       T(-0.8), T(4), T(0.8), T(-0.8), T(4), T(0), T(0.9), T(4)},
      {T(-1), T(-1), T(3), T(1), T(-1), T(3), T(0), T(1), T(3), T(-0.8),
       T(-0.8), T(3), T(0.8), T(-0.8), T(3), T(0), T(0.9), T(3)},
      {T(-1), T(-1), T(4), T(1), T(-1), T(4), T(0), T(1), T(4), T(-0.8),
       T(-0.8), T(2), T(0.8), T(-0.8), T(2), T(0), T(0.9), T(2)}};

  std::vector<T> frames[3];
  const T *frame_ptrs[3];
  for (int k = 0; k < 3; ++k) {
    frames[k].assign(coords[k], coords[k] + 18);
    frame_ptrs[k] = &frames[k][0];
  }
  const unsigned int faces[6] = {0, 1, 2, 3, 4, 5};

  nanort::MotionTriangleMesh<T> mesh(times, 3, frame_ptrs, faces, 2,
                                     3 * sizeof(T));
  nanort::BVHAccel<T> accel;
  TASK_CHECK(accel.BuildMotion(mesh));
  nanort::MotionTriangleIntersector<T> isector(mesh);

  const T org[3] = {T(0.0), T(0.0), T(0.0)};
  const T dir[3] = {T(0.0), T(0.0), T(1.0)};

  const T query_times[] = {T(0.0), T(0.25), T(0.75), T(1.0)};
  const unsigned int expect_prim[] = {0, 0, 1, 1};
  const T expect_t[] = {T(2.0), T(2.5), T(2.5), T(2.0)};

  for (size_t i = 0; i < 4; ++i) {
    nanort::Ray<T> ray = MakeRay<T>(org[0], org[1], org[2], dir[0], dir[1],
                                    dir[2], query_times[i]);
    nanort::TriangleIntersection<T> hit;
    TASK_CHECK(accel.Traverse(ray, isector, &hit));
    TASK_CHECK(hit.prim_id == expect_prim[i]);
    TASK_CHECK(std::fabs(hit.t - expect_t[i]) < hit_tol<T>());

    const RefHit<T> ref = RefTrace<T>(times, 3, frame_ptrs, faces, 2,
                                      3 * sizeof(T), org, dir, ray.min_t,
                                      ray.max_t, query_times[i]);
    TASK_CHECK(ref.hit);
    TASK_CHECK(HitMatches(hit, ref));
  }

  return 0;
}

// Vertex struct with extra attributes: must be read through the byte stride,
// never as tightly packed XYZ.
template <typename T>
struct PaddedVert {
  T x, y, z;
  T nx, ny, nz;
  int material;
};

int PaddedStride() {
  typedef double T;

  const T times[3] = {T(0.0), T(0.7), T(1.0)};

  PaddedVert<T> frames[3][3];
  const T pos[3][9] = {{T(-1), T(-1), T(2), T(1), T(-1), T(2), T(0), T(1),
                        T(2)},
                       {T(-1), T(-1), T(3), T(1), T(-0.5), T(3), T(0), T(1),
                        T(4)},
                       {T(-1), T(-1), T(2), T(1), T(-1), T(2), T(0), T(1),
                        T(2)}};
  const T *frame_ptrs[3];
  for (int k = 0; k < 3; ++k) {
    for (int j = 0; j < 3; ++j) {
      frames[k][j].x = pos[k][3 * j + 0];
      frames[k][j].y = pos[k][3 * j + 1];
      frames[k][j].z = pos[k][3 * j + 2];
      // Junk attributes: a packed-XYZ misread would produce garbage geometry.
      frames[k][j].nx = T(1000 + 10 * k + j);
      frames[k][j].ny = T(-2000 - 10 * k - j);
      frames[k][j].nz = T(3000);
      frames[k][j].material = 7;
    }
    frame_ptrs[k] = &frames[k][0].x;
  }
  const unsigned int faces[3] = {0, 1, 2};

  nanort::MotionTriangleMesh<T> mesh(times, 3, frame_ptrs, faces, 1,
                                     sizeof(PaddedVert<T>));
  nanort::BVHAccel<T> accel;
  TASK_CHECK(accel.BuildMotion(mesh));
  nanort::MotionTriangleIntersector<T> isector(mesh);

  const T query_times[] = {T(0.0), T(0.35), T(0.7), T(0.85), T(1.0)};
  const T orgs[2][3] = {{T(0.1), T(0.0), T(0.0)}, {T(-0.2), T(0.3), T(0.0)}};

  unsigned int num_hits = 0;
  for (size_t ti = 0; ti < 5; ++ti) {
    for (size_t ri = 0; ri < 2; ++ri) {
      const T dir[3] = {T(0.0), T(0.0), T(1.0)};
      nanort::Ray<T> ray = MakeRay<T>(orgs[ri][0], orgs[ri][1], orgs[ri][2],
                                      dir[0], dir[1], dir[2],
                                      query_times[ti]);
      nanort::TriangleIntersection<T> hit;
      const bool h = accel.Traverse(ray, isector, &hit);

      const RefHit<T> ref =
          RefTrace<T>(times, 3, frame_ptrs, faces, 1, sizeof(PaddedVert<T>),
                      orgs[ri], dir, ray.min_t, ray.max_t, query_times[ti]);
      TASK_CHECK(h == ref.hit);
      if (h && ref.hit) {
        TASK_CHECK(HitMatches(hit, ref));
        ++num_hits;
      }
    }
  }
  TASK_CHECK(num_hits > 0);

  return 0;
}

// Two triangles sharing an edge; exercises min_t/max_t, skip_prim_id,
// prim_ids_range and back-face culling on a motion tree.
int SharedEdgeFilters() {
  typedef float T;

  const T times[3] = {T(0.0), T(0.5), T(1.0)};
  std::vector<T> frames[3];
  const T *frame_ptrs[3];
  const T zs[3] = {T(2.0), T(2.6), T(2.0)};
  for (int k = 0; k < 3; ++k) {
    const T z = zs[k];
    const T v[12] = {T(-1), T(-1), z, T(1), T(-1), z,
                     T(1),  T(1),  z, T(-1), T(1), z};
    frames[k].assign(v, v + 12);
    frame_ptrs[k] = &frames[k][0];
  }
  const unsigned int faces[6] = {0, 1, 2, 0, 2, 3};

  nanort::MotionTriangleMesh<T> mesh(times, 3, frame_ptrs, faces, 2,
                                     3 * sizeof(T));
  nanort::BVHAccel<T> accel;
  TASK_CHECK(accel.BuildMotion(mesh));
  nanort::MotionTriangleIntersector<T> isector(mesh);

  // t = 0.25 -> z = 2.3; point (0.3, -0.2) lies strictly inside triangle 0.
  const T qt = T(0.25);
  const T expect_z = T(2.3);

  {
    nanort::Ray<T> ray = MakeRay<T>(T(0.3), T(-0.2), T(0.0), T(0.0), T(0.0),
                                    T(1.0), qt);
    nanort::TriangleIntersection<T> hit;
    TASK_CHECK(accel.Traverse(ray, isector, &hit));
    TASK_CHECK(hit.prim_id == 0);
    TASK_CHECK(std::fabs(hit.t - expect_z) < hit_tol<T>());

    // Ray exactly onto the shared edge (diagonal through the origin).
    nanort::Ray<T> edge_ray = MakeRay<T>(T(0.0), T(0.0), T(0.0), T(0.0),
                                         T(0.0), T(1.0), qt);
    nanort::TriangleIntersection<T> edge_hit;
    TASK_CHECK(accel.Traverse(edge_ray, isector, &edge_hit));
    TASK_CHECK(std::fabs(edge_hit.t - expect_z) < hit_tol<T>());
  }

  // skip_prim_id
  {
    nanort::Ray<T> ray = MakeRay<T>(T(0.3), T(-0.2), T(0.0), T(0.0), T(0.0),
                                    T(1.0), qt);
    nanort::TriangleIntersection<T> hit;
    nanort::BVHTraceOptions opts;
    opts.skip_prim_id = 0;
    TASK_CHECK(!accel.Traverse(ray, isector, &hit, opts));
  }

  // prim_ids_range
  {
    nanort::Ray<T> ray = MakeRay<T>(T(0.3), T(-0.2), T(0.0), T(0.0), T(0.0),
                                    T(1.0), qt);
    nanort::TriangleIntersection<T> hit;
    nanort::BVHTraceOptions opts;
    opts.prim_ids_range[0] = 1;
    opts.prim_ids_range[1] = 2;
    TASK_CHECK(!accel.Traverse(ray, isector, &hit, opts));

    opts.prim_ids_range[0] = 0;
    opts.prim_ids_range[1] = 1;
    TASK_CHECK(accel.Traverse(ray, isector, &hit, opts));
    TASK_CHECK(hit.prim_id == 0);
  }

  // min_t / max_t
  {
    nanort::Ray<T> ray = MakeRay<T>(T(0.3), T(-0.2), T(0.0), T(0.0), T(0.0),
                                    T(1.0), qt);
    nanort::TriangleIntersection<T> hit;

    ray.min_t = T(3.0);
    TASK_CHECK(!accel.Traverse(ray, isector, &hit));

    ray.min_t = T(2.0);
    TASK_CHECK(accel.Traverse(ray, isector, &hit));

    ray.max_t = T(2.0);
    TASK_CHECK(!accel.Traverse(ray, isector, &hit));
  }

  // Back-face culling: the quad front side faces +z.
  {
    nanort::BVHTraceOptions cull;
    cull.cull_back_face = true;
    nanort::TriangleIntersection<T> hit;

    // Ray from above hits the front face: kept even with culling.
    nanort::Ray<T> front_ray = MakeRay<T>(T(0.3), T(-0.2), T(5.0), T(0.0),
                                          T(0.0), T(-1.0), qt);
    TASK_CHECK(accel.Traverse(front_ray, isector, &hit, cull));
    TASK_CHECK(std::fabs(hit.t - T(2.7)) < hit_tol<T>());

    // Ray from below hits the back face: culled with culling, hit without.
    nanort::Ray<T> back_ray = MakeRay<T>(T(0.3), T(-0.2), T(0.0), T(0.0),
                                         T(0.0), T(1.0), qt);
    TASK_CHECK(!accel.Traverse(back_ray, isector, &hit, cull));
    TASK_CHECK(accel.Traverse(back_ray, isector, &hit));
  }

  return 0;
}

// Verifies per-key node bounds and query-time node bounds, and that
// intermediate-time vertices are always contained.
int NodeKeyBounds() {
  typedef float T;
  const int kNumTris = 8;

  T times[3] = {T(0.0), T(0.4), T(1.0)};
  std::vector<T> frames[3];
  const T *frame_ptrs[3];
  unsigned int faces[3 * kNumTris];

  for (int k = 0; k < 3; ++k) {
    frames[k].resize(kNumTris * 9);
  }
  for (int i = 0; i < kNumTris; ++i) {
    faces[3 * i + 0] = 3 * i + 0;
    faces[3 * i + 1] = 3 * i + 1;
    faces[3 * i + 2] = 3 * i + 2;
    for (int k = 0; k < 3; ++k) {
      T cx = T(2 * i - 7), cy = T(0.0), cz = T(5.0);
      if (k == 1) {
        cy = T(1.0 + 0.1 * i);
      } else if (k == 2) {
        cx = T(2 * i - 8);
        cz = T(6.0);
      }
      const T off[3][3] = {
          {T(-0.4), T(-0.4), T(0)}, {T(0.4), T(-0.4), T(0)}, {T(0), T(0.4), T(0)}};
      for (int j = 0; j < 3; ++j) {
        frames[k][9 * i + 3 * j + 0] = cx + off[j][0];
        frames[k][9 * i + 3 * j + 1] = cy + off[j][1];
        frames[k][9 * i + 3 * j + 2] = cz + off[j][2];
      }
    }
  }
  for (int k = 0; k < 3; ++k) {
    frame_ptrs[k] = &frames[k][0];
  }

  nanort::MotionTriangleMesh<T> mesh(times, 3, frame_ptrs, faces, kNumTris,
                                     3 * sizeof(T));
  nanort::BVHBuildOptions<T> options;
  options.min_leaf_primitives = 1;

  nanort::BVHAccel<T> accel;
  TASK_CHECK(accel.BuildMotion(mesh, options));
  TASK_CHECK(accel.HasMotion());
  TASK_CHECK(accel.GetNumMotionKeyTimes() == 3);
  TASK_CHECK(accel.GetMotionKeyTimes()[0] == T(0.0));
  TASK_CHECK(accel.GetMotionKeyTimes()[1] == T(0.4));
  TASK_CHECK(accel.GetMotionKeyTimes()[2] == T(1.0));

  const std::vector<nanort::BVHNode<T> > &nodes = accel.GetNodes();
  const std::vector<unsigned int> &indices = accel.GetIndices();
  TASK_CHECK(nodes.size() > 1);

  // Independently compute the expected per-key node bounds (leaves from
  // their primitives, branches from their children) and compare exactly.
  const T kMax = std::numeric_limits<T>::max();
  std::vector<T> ebmin(nodes.size() * 3 * 3);
  std::vector<T> ebmax(nodes.size() * 3 * 3);
  for (size_t ni = nodes.size(); ni-- > 0;) {
    const nanort::BVHNode<T> &node = nodes[ni];
    for (size_t k = 0; k < 3; ++k) {
      T bmin[3] = {kMax, kMax, kMax};
      T bmax[3] = {-kMax, -kMax, -kMax};
      if (node.flag == 1) {
        for (unsigned int i = 0; i < node.data[0]; ++i) {
          const unsigned int prim = indices[node.data[1] + i];
          for (int j = 0; j < 3; ++j) {
            const unsigned int f = faces[3 * prim + j];
            for (int c = 0; c < 3; ++c) {
              const T v = frames[k][3 * f + c];
              bmin[c] = std::min(bmin[c], v);
              bmax[c] = std::max(bmax[c], v);
            }
          }
        }
      } else {
        for (int c = 0; c < 3; ++c) {
          bmin[c] = std::min(ebmin[(node.data[0] * 3 + k) * 3 + c],
                             ebmin[(node.data[1] * 3 + k) * 3 + c]);
          bmax[c] = std::max(ebmax[(node.data[0] * 3 + k) * 3 + c],
                             ebmax[(node.data[1] * 3 + k) * 3 + c]);
        }
      }
      for (int c = 0; c < 3; ++c) {
        ebmin[(ni * 3 + k) * 3 + c] = bmin[c];
        ebmax[(ni * 3 + k) * 3 + c] = bmax[c];
      }

      T gbmin[3], gbmax[3];
      TASK_CHECK(
          accel.GetMotionNodeBoundingBox(static_cast<unsigned int>(ni), k,
                                         gbmin, gbmax));
      for (int c = 0; c < 3; ++c) {
        TASK_CHECK(gbmin[c] == bmin[c]);
        TASK_CHECK(gbmax[c] == bmax[c]);
      }
    }
  }

  // At intermediate times, the query-time node bounds must contain every
  // interpolated vertex of the leaf primitives, and stay inside the swept
  // node bounds.
  const T eps = T(1e-5);
  const T query_times[] = {T(0.2), T(0.7)};
  for (size_t qi = 0; qi < 2; ++qi) {
    const T qt = query_times[qi];
    size_t seg = 0;
    T alpha = T(0.0);
    TASK_CHECK(nanort::MotionTimeSegment(times, 3, qt, &seg, &alpha));

    for (size_t ni = 0; ni < nodes.size(); ++ni) {
      if (nodes[ni].flag != 1) {
        continue;
      }
      T qbmin[3], qbmax[3];
      TASK_CHECK(accel.GetMotionNodeBoundingBoxAtTime(
          static_cast<unsigned int>(ni), qt, qbmin, qbmax));

      for (unsigned int i = 0; i < nodes[ni].data[0]; ++i) {
        const unsigned int prim = indices[nodes[ni].data[1] + i];
        for (int j = 0; j < 3; ++j) {
          const unsigned int f = faces[3 * prim + j];
          for (int c = 0; c < 3; ++c) {
            const T v = (T(1.0) - alpha) * frames[seg][3 * f + c] +
                        alpha * frames[seg + 1][3 * f + c];
            TASK_CHECK(v >= qbmin[c] - eps);
            TASK_CHECK(v <= qbmax[c] + eps);
          }
        }
      }

      for (int c = 0; c < 3; ++c) {
        TASK_CHECK(qbmin[c] >= nodes[ni].bmin[c] - eps);
        TASK_CHECK(qbmax[c] <= nodes[ni].bmax[c] + eps);
      }
    }
  }

  // The query-time bounds must be genuinely tighter than the whole-shutter
  // swept bounds: at t = 0 the root's y extent is the key frame 0 extent.
  {
    T rbmin[3], rbmax[3];
    TASK_CHECK(accel.GetMotionNodeBoundingBoxAtTime(0, T(0.0), rbmin, rbmax));
    TASK_CHECK(rbmax[1] <= T(0.4) + eps);
    TASK_CHECK(nodes[0].bmax[1] >= T(2.1) - eps);
    TASK_CHECK(rbmax[1] < nodes[0].bmax[1] - T(0.5));
  }

  // Invalid queries are rejected.
  {
    T bmin[3], bmax[3];
    TASK_CHECK(!accel.GetMotionNodeBoundingBoxAtTime(0, T(2.0), bmin, bmax));
    TASK_CHECK(!accel.GetMotionNodeBoundingBoxAtTime(
        0, std::numeric_limits<T>::quiet_NaN(), bmin, bmax));
    TASK_CHECK(!accel.GetMotionNodeBoundingBoxAtTime(9999, T(0.5), bmin,
                                                     bmax));
    TASK_CHECK(!accel.GetMotionNodeBoundingBox(0, 3, bmin, bmax));
    TASK_CHECK(!accel.GetMotionNodeBoundingBox(9999, 0, bmin, bmax));

    nanort::BVHAccel<T> static_accel;
    TASK_CHECK(!static_accel.GetMotionNodeBoundingBox(0, 0, bmin, bmax));
    TASK_CHECK(!static_accel.GetMotionNodeBoundingBoxAtTime(0, T(0.0), bmin,
                                                            bmax));
  }

  return 0;
}

// Many triangles whose swept bounds overlap the ray, but which are far away
// from the ray at the query time. The number of primitive Intersect calls
// proves the traversal culls nodes before reaching any leaf.
int TraversalPruning() {
  typedef float T;
  const int kNumTris = 64;

  T times[3] = {T(0.0), T(0.5), T(1.0)};
  std::vector<T> frames[3];
  const T *frame_ptrs[3];
  unsigned int faces[3 * kNumTris];

  for (int k = 0; k < 3; ++k) {
    frames[k].resize(kNumTris * 9);
  }
  for (int i = 0; i < kNumTris; ++i) {
    faces[3 * i + 0] = 3 * i + 0;
    faces[3 * i + 1] = 3 * i + 1;
    faces[3 * i + 2] = 3 * i + 2;
    const T cx0 = T(-9.6) + T(0.3) * T(i);
    for (int k = 0; k < 3; ++k) {
      // Key 0: near the ray. Keys 1 and 2: shifted +100 in x.
      const T cx = cx0 + (k == 0 ? T(0.0) : T(100.0));
      frames[k][9 * i + 0] = cx - T(0.15);
      frames[k][9 * i + 1] = T(-1.0);
      frames[k][9 * i + 2] = T(5.0);
      frames[k][9 * i + 3] = cx + T(0.15);
      frames[k][9 * i + 4] = T(-1.0);
      frames[k][9 * i + 5] = T(5.0);
      frames[k][9 * i + 6] = cx;
      frames[k][9 * i + 7] = T(1.0);
      frames[k][9 * i + 8] = T(5.0);
    }
  }
  for (int k = 0; k < 3; ++k) {
    frame_ptrs[k] = &frames[k][0];
  }

  nanort::MotionTriangleMesh<T> mesh(times, 3, frame_ptrs, faces, kNumTris,
                                     3 * sizeof(T));
  nanort::BVHBuildOptions<T> options;
  options.min_leaf_primitives = 1;

  nanort::BVHAccel<T> accel;
  TASK_CHECK(accel.BuildMotion(mesh, options));

  const std::vector<nanort::BVHNode<T> > &nodes = accel.GetNodes();

  // The whole-shutter swept root box straddles the ray at x = 0, so a
  // swept-box traversal would descend into the tree...
  TASK_CHECK(nodes[0].bmin[0] <= T(0.0));
  TASK_CHECK(nodes[0].bmax[0] >= T(0.0));

  // ...but the time-varying root box at t = 0.75 is far away from the ray.
  {
    T qbmin[3], qbmax[3];
    TASK_CHECK(accel.GetMotionNodeBoundingBoxAtTime(0, T(0.75), qbmin, qbmax));
    TASK_CHECK(qbmin[0] > T(50.0));
  }

  CountingIntersector<T> counting(mesh);
  nanort::TriangleIntersection<T> hit;

  // t = 0.75: all triangles are far from the ray. Zero primitive tests.
  {
    nanort::Ray<T> ray = MakeRay<T>(T(0.0), T(0.0), T(0.0), T(0.0), T(0.0),
                                    T(1.0), T(0.75));
    counting.count = 0;
    TASK_CHECK(!accel.Traverse(ray, counting, &hit));
    TASK_CHECK(counting.count == 0);
  }

  // t = 0.25: triangles are halfway (+50 in x). Still zero primitive tests.
  {
    nanort::Ray<T> ray = MakeRay<T>(T(0.0), T(0.0), T(0.0), T(0.0), T(0.0),
                                    T(1.0), T(0.25));
    counting.count = 0;
    TASK_CHECK(!accel.Traverse(ray, counting, &hit));
    TASK_CHECK(counting.count == 0);
  }

  // t = 0: the ray hits triangle 32 (centered at x = 0), exercising
  // primitive intersection for real.
  {
    nanort::Ray<T> ray = MakeRay<T>(T(0.0), T(0.0), T(0.0), T(0.0), T(0.0),
                                    T(1.0), T(0.0));
    counting.count = 0;
    TASK_CHECK(accel.Traverse(ray, counting, &hit));
    TASK_CHECK(counting.count > 0);
    TASK_CHECK(hit.prim_id == 32);
    TASK_CHECK(std::fabs(hit.t - T(5.0)) < hit_tol<T>());

    const T org[3] = {T(0.0), T(0.0), T(0.0)};
    const T dir[3] = {T(0.0), T(0.0), T(1.0)};
    const RefHit<T> ref = RefTrace<T>(times, 3, frame_ptrs, faces, kNumTris,
                                      3 * sizeof(T), org, dir, ray.min_t,
                                      ray.max_t, T(0.0));
    TASK_CHECK(ref.hit);
    TASK_CHECK(HitMatches(hit, ref));
  }

  return 0;
}

// Serialization round-trip of a motion tree (float and double).
template <typename T>
int RunRoundTrip(const char *path) {
  FlexData<T> d;
  nanort::MotionTriangleMesh<T> mesh(d.times, 3, d.frame_ptrs, d.faces, 2,
                                     3 * sizeof(T));

  nanort::BVHAccel<T> accel;
  TASK_CHECK(accel.BuildMotion(mesh));
  TASK_CHECK(accel.Dump(path));

  nanort::BVHAccel<T> loaded;
  TASK_CHECK(loaded.Load(path));
  TASK_CHECK(loaded.HasMotion());
  TASK_CHECK(loaded.GetNumMotionKeyTimes() == 3);
  for (size_t k = 0; k < 3; ++k) {
    TASK_CHECK(loaded.GetMotionKeyTimes()[k] == d.times[k]);
  }
  TASK_CHECK(loaded.GetNodes().size() == accel.GetNodes().size());

  // Per-key node bounds survive the round-trip exactly.
  for (size_t ni = 0; ni < accel.GetNodes().size(); ++ni) {
    for (size_t k = 0; k < 3; ++k) {
      T a0[3], a1[3], b0[3], b1[3];
      TASK_CHECK(accel.GetMotionNodeBoundingBox(
          static_cast<unsigned int>(ni), k, a0, a1));
      TASK_CHECK(loaded.GetMotionNodeBoundingBox(
          static_cast<unsigned int>(ni), k, b0, b1));
      for (int c = 0; c < 3; ++c) {
        TASK_CHECK(a0[c] == b0[c]);
        TASK_CHECK(a1[c] == b1[c]);
      }
    }
  }

  // Rebinding the same geometry to the loaded tree gives identical results.
  nanort::MotionTriangleIntersector<T> isector(mesh);
  const T query_times[] = {T(0.0), T(0.15), T(0.3), T(0.65), T(1.0)};
  unsigned int num_hits = 0;
  for (size_t i = 0; i < 5; ++i) {
    nanort::Ray<T> ray = MakeRay<T>(T(0.1), T(0.05), T(0.0), T(0.2), T(-0.1),
                                    T(1.0), query_times[i]);
    nanort::TriangleIntersection<T> h0, h1;
    const bool r0 = accel.Traverse(ray, isector, &h0);
    const bool r1 = loaded.Traverse(ray, isector, &h1);
    TASK_CHECK(r0 == r1);
    if (r0 && r1) {
      TASK_CHECK(h0.prim_id == h1.prim_id);
      TASK_CHECK(h0.t == h1.t);
      TASK_CHECK(h0.u == h1.u);
      TASK_CHECK(h0.v == h1.v);
      ++num_hits;
    }
  }
  TASK_CHECK(num_hits > 0);

  // Out-of-domain query on the loaded tree: miss.
  {
    nanort::Ray<T> ray = MakeRay<T>(T(0.0), T(0.0), T(0.0), T(0.0), T(0.0),
                                    T(1.0), T(2.0));
    nanort::TriangleIntersection<T> hit;
    TASK_CHECK(!loaded.Traverse(ray, isector, &hit));
  }

  return 0;
}

int SerializationRoundTrip() {
  TASK_CHECK(RunRoundTrip<float>(".task-build/motion-roundtrip-float.bvh") ==
             0);
  TASK_CHECK(RunRoundTrip<double>(".task-build/motion-roundtrip-double.bvh") ==
             0);
  return 0;
}

// Static files written by the previous (pre-motion) format must still load.
template <typename T>
int RunLegacy(const char *path) {
  struct Vertex {
    T x, y, z, padding;
  };
  const Vertex v[6] = {{-1, -1, 2, 777}, {1, -1, 2, 777}, {0, 1, 2, 777},
                       {-1, -1, 4, 777}, {1, -1, 4, 777}, {0, 1, 4, 777}};
  const unsigned int faces[6] = {0, 1, 2, 3, 4, 5};

  nanort::BVHAccel<T> accel;
  TASK_CHECK(accel.Load(path));
  TASK_CHECK(!accel.HasMotion());
  TASK_CHECK(accel.GetNumMotionKeyTimes() == 0);
  TASK_CHECK(accel.GetNodes().size() == 1);

  nanort::TriangleIntersector<T> isector(&v[0].x, faces, sizeof(Vertex));
  nanort::Ray<T> ray;
  ray.dir[2] = T(1.0);
  ray.max_t = T(10.0);
  ray.time = T(0.7);  // must be ignored by the static tree

  nanort::TriangleIntersection<T> hit;
  TASK_CHECK(accel.Traverse(ray, isector, &hit));
  TASK_CHECK(hit.prim_id == 0);
  TASK_CHECK(std::fabs(hit.t - T(2.0)) < hit_tol<T>());
  TASK_CHECK(std::fabs(hit.u - T(0.25)) < hit_tol<T>());
  TASK_CHECK(std::fabs(hit.v - T(0.5)) < hit_tol<T>());

  return 0;
}

int LegacyStaticFiles() {
  TASK_CHECK(RunLegacy<float>("task_support/legacy-float.bvh") == 0);
  TASK_CHECK(RunLegacy<double>("task_support/legacy-double.bvh") == 0);
  return 0;
}

// A single BVHAccel object: motion build first, then a regular static Build.
// Static queries afterwards must not see the old time axis, and the new
// Ray::time field must not change static behavior.
int MotionToStaticRebuild() {
  typedef float T;

  FlexData<T> d;
  nanort::MotionTriangleMesh<T> mesh(d.times, 3, d.frame_ptrs, d.faces, 2,
                                     3 * sizeof(T));

  nanort::BVHAccel<T> accel;
  TASK_CHECK(accel.BuildMotion(mesh));
  TASK_CHECK(accel.HasMotion());

  // Motion query at t = 0.15: the quad has moved away from z = 2.
  {
    nanort::MotionTriangleIntersector<T> miso(mesh);
    nanort::Ray<T> ray = MakeRay<T>(T(0.3), T(-0.2), T(0.0), T(0.0), T(0.0),
                                    T(1.0), T(0.15));
    nanort::TriangleIntersection<T> hit;
    TASK_CHECK(accel.Traverse(ray, miso, &hit));
    TASK_CHECK(hit.t > T(2.5));
  }

  // Rebuild as a static tree over key frame 0 of the same geometry.
  nanort::TriangleMesh<T> smesh(d.frame_ptrs[0], d.faces, 3 * sizeof(T));
  nanort::TriangleSAHPred<T> spred(d.frame_ptrs[0], d.faces, 3 * sizeof(T));
  TASK_CHECK(accel.Build(2, smesh, spred));
  TASK_CHECK(!accel.HasMotion());
  TASK_CHECK(accel.GetNumMotionKeyTimes() == 0);
  {
    T bmin[3], bmax[3];
    TASK_CHECK(!accel.GetMotionNodeBoundingBox(0, 0, bmin, bmax));
    TASK_CHECK(!accel.GetMotionNodeBoundingBoxAtTime(0, T(0.0), bmin, bmax));
  }

  // Static query with a stale ray time: uses frame 0 (z = 2), not the old
  // motion time axis.
  nanort::TriangleIntersector<T> siso(d.frame_ptrs[0], d.faces,
                                      3 * sizeof(T));
  nanort::Ray<T> ray = MakeRay<T>(T(0.3), T(-0.2), T(0.0), T(0.0), T(0.0),
                                  T(1.0), T(0.15));
  nanort::TriangleIntersection<T> hit;
  TASK_CHECK(accel.Traverse(ray, siso, &hit));
  TASK_CHECK(hit.prim_id == 0);
  TASK_CHECK(std::fabs(hit.t - T(2.0)) < hit_tol<T>());

  // A different ray time gives the identical static result.
  nanort::Ray<T> ray2 = ray;
  ray2.time = T(0.9);
  nanort::TriangleIntersection<T> hit2;
  TASK_CHECK(accel.Traverse(ray2, siso, &hit2));
  TASK_CHECK(hit2.prim_id == hit.prim_id);
  TASK_CHECK(hit2.t == hit.t);
  TASK_CHECK(hit2.u == hit.u);
  TASK_CHECK(hit2.v == hit.v);

  // Static dump/load after the rebuild carries no motion data.
  TASK_CHECK(accel.Dump(".task-build/static-after-motion.bvh"));
  nanort::BVHAccel<T> loaded;
  TASK_CHECK(loaded.Load(".task-build/static-after-motion.bvh"));
  TASK_CHECK(!loaded.HasMotion());
  nanort::TriangleIntersection<T> lhit;
  TASK_CHECK(loaded.Traverse(ray, siso, &lhit));
  TASK_CHECK(lhit.prim_id == 0);
  TASK_CHECK(std::fabs(lhit.t - T(2.0)) < hit_tol<T>());

  return 0;
}

}  // namespace

const struct task_case task_cases[] = {
    {"deformation", "flex_float", FlexFloat},
    {"deformation", "flex_double", FlexDouble},
    {"deformation", "front_back_swap", FrontBackSwap},
    {"deformation", "padded_stride", PaddedStride},
    {"deformation", "shared_edge_filters", SharedEdgeFilters},
    {"bounds", "node_key_bounds", NodeKeyBounds},
    {"bounds", "traversal_pruning", TraversalPruning},
    {"persistence", "serialization_roundtrip", SerializationRoundTrip},
    {"persistence", "legacy_static_files", LegacyStaticFiles},
    {"persistence", "motion_to_static_rebuild", MotionToStaticRebuild},
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
