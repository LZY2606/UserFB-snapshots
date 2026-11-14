//
// NanoRT motion blur extension: deforming triangle meshes with keyframed
// vertex positions.
//
// This header provides:
//
//   - MotionTriangleMesh<T>        : keyframed triangle mesh geometry. The
//                                    caller owns all arrays; they must stay
//                                    alive while the BVH is built and queried.
//   - MotionTriangleSAHPred<T>     : SAH predicator over swept bounds.
//   - MotionTriangleIntersector<T> : ray/triangle intersector which linearly
//                                    interpolates vertices at `Ray::time`.
//   - BVHAccel<T>::BuildMotion()   : builds a BVH once over the whole shutter
//                                    interval and attaches conservative node
//                                    bounds at every key time.
//
// Topology is constant over the shutter interval; only vertex positions
// change. Vertices are interpolated linearly within each key time segment.
//
/*
The MIT License (MIT)

Copyright (c) 2015 - Present: Light Transport Entertainment Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef NANORT_MOTION_H_
#define NANORT_MOTION_H_

#include "nanort.h"

namespace nanort {

///
/// @brief Keyframed motion(deforming) triangle mesh.
///
/// Stores pointers to caller-owned arrays:
///
///   - `key_times`        : `num_key_times` strictly increasing, finite key
///                          times (at least 2; typically 3 or more).
///   - `vertices`         : `vertices[k]` points to the vertex array of key
///                          frame `k`. Each vertex is `vertex_stride_bytes`
///                          apart and starts with XYZ, so vertex structs with
///                          extra attributes (normals, padding, ...) are
///                          supported.
///   - `faces`            : shared triangle indices (3 per face), valid for
///                          every key frame.
///
/// @tparam T real value type(float or double).
///
template <typename T>
class MotionTriangleMesh {
 public:
  MotionTriangleMesh(
      const T *key_times, size_t num_key_times,
      const T *const *vertices,  // vertices[k] = vertex array of key frame k
      const unsigned int *faces, size_t num_faces,
      const size_t vertex_stride_bytes)  // e.g. 12 for sizeof(float) * XYZ
      : key_times_(key_times),
        num_key_times_(num_key_times),
        vertices_(vertices),
        faces_(faces),
        num_faces_(num_faces),
        vertex_stride_bytes_(vertex_stride_bytes) {}

  ///
  /// Returns true when the key times are strictly increasing and finite and
  /// all arrays are present.
  ///
  bool IsValid() const {
    if ((key_times_ == NULL) || (vertices_ == NULL) || (faces_ == NULL)) {
      return false;
    }
    if (num_key_times_ < 2 || num_faces_ == 0) {
      return false;
    }
    if (vertex_stride_bytes_ < sizeof(T) * 3) {
      return false;
    }
    for (size_t k = 0; k < num_key_times_; k++) {
      if (vertices_[k] == NULL) {
        return false;
      }
      // Rejects NaN and +/-inf.
      if (!(key_times_[k] >= -std::numeric_limits<T>::max()) ||
          !(key_times_[k] <= std::numeric_limits<T>::max())) {
        return false;
      }
      if ((k > 0) && !(key_times_[k] > key_times_[k - 1])) {
        return false;
      }
    }
    return true;
  }

  /// Compute bounding box of `prim_index`th triangle at key `key_index`.
  void BoundingBoxAtKey(real3<T> *bmin, real3<T> *bmax,
                        unsigned int prim_index, size_t key_index) const {
    const T *vertices = vertices_[key_index];

    unsigned int vertex = faces_[3 * prim_index + 0];

    (*bmin)[0] = get_vertex_addr(vertices, vertex, vertex_stride_bytes_)[0];
    (*bmin)[1] = get_vertex_addr(vertices, vertex, vertex_stride_bytes_)[1];
    (*bmin)[2] = get_vertex_addr(vertices, vertex, vertex_stride_bytes_)[2];
    (*bmax)[0] = get_vertex_addr(vertices, vertex, vertex_stride_bytes_)[0];
    (*bmax)[1] = get_vertex_addr(vertices, vertex, vertex_stride_bytes_)[1];
    (*bmax)[2] = get_vertex_addr(vertices, vertex, vertex_stride_bytes_)[2];

    // remaining two vertices of the primitive
    for (unsigned int i = 1; i < 3; i++) {
      // xyz
      for (int k = 0; k < 3; k++) {
        T coord = get_vertex_addr<T>(vertices, faces_[3 * prim_index + i],
                                     vertex_stride_bytes_)[k];

        (*bmin)[k] = std::min((*bmin)[k], coord);
        (*bmax)[k] = std::max((*bmax)[k], coord);
      }
    }
  }

  /// Compute swept bounding box of `prim_index`th triangle over the whole
  /// shutter interval. Called for each primitive during BVH build.
  void BoundingBox(real3<T> *bmin, real3<T> *bmax,
                   unsigned int prim_index) const {
    BoundingBoxAtKey(bmin, bmax, prim_index, 0);

    for (size_t key = 1; key < num_key_times_; key++) {
      real3<T> key_bmin, key_bmax;
      BoundingBoxAtKey(&key_bmin, &key_bmax, prim_index, key);

      for (int k = 0; k < 3; k++) {
        (*bmin)[k] = std::min((*bmin)[k], key_bmin[k]);
        (*bmax)[k] = std::max((*bmax)[k], key_bmax[k]);
      }
    }
  }

  void BoundingBoxAndCenter(real3<T> *bmin, real3<T> *bmax, real3<T> *center,
                            unsigned int prim_index) const {
    BoundingBox(bmin, bmax, prim_index);
    *center = ((*bmin) + (*bmax)) * static_cast<T>(0.5);
  }

  //
  // Accessors
  //
  const T *GetKeyTimes() const { return key_times_; }
  size_t GetNumKeyTimes() const { return num_key_times_; }
  const T *const *GetVertices() const { return vertices_; }
  const T *GetVertices(size_t key_index) const { return vertices_[key_index]; }
  const unsigned int *GetFaces() const { return faces_; }
  size_t GetNumFaces() const { return num_faces_; }
  size_t GetVertexStrideBytes() const { return vertex_stride_bytes_; }

 private:
  const T *key_times_;
  const size_t num_key_times_;
  const T *const *vertices_;
  const unsigned int *faces_;
  const size_t num_faces_;
  const size_t vertex_stride_bytes_;
};

///
/// @brief SAH predicator for motion triangle meshes. Partitions primitives
/// using the center of their swept bounding box.
///
template <typename T = float>
class MotionTriangleSAHPred {
 public:
  explicit MotionTriangleSAHPred(const MotionTriangleMesh<T> &mesh)
      : axis_(0), pos_(static_cast<T>(0.0)), mesh_(&mesh) {}

  MotionTriangleSAHPred(const MotionTriangleSAHPred<T> &rhs)
      : axis_(rhs.axis_), pos_(rhs.pos_), mesh_(rhs.mesh_) {}

  void Set(int axis, T pos) const {
    axis_ = axis;
    pos_ = pos;
  }

  bool operator()(unsigned int i) const {
    int axis = axis_;
    T pos = pos_;

    real3<T> bmin, bmax;
    mesh_->BoundingBox(&bmin, &bmax, i);

    T center2 = bmin[axis] + bmax[axis];
    return (center2 < pos * static_cast<T>(2.0));
  }

 private:
  mutable int axis_;
  mutable T pos_;
  const MotionTriangleMesh<T> *mesh_;
};

///
/// @brief Ray/triangle intersector for motion triangle meshes.
///
/// Vertices are linearly interpolated between the two key frames bracketing
/// `Ray::time`; interpolation happens only for the triangles actually
/// intersected. Queries exactly at a key time use that key frame. Queries
/// with non-finite or out-of-domain times never touch vertex data and report
/// a miss.
///
/// Hit `t`, `u`, `v` and `prim_id` carry the same meaning as
/// `TriangleIntersector`.
///
/// @tparam T Precision(float or double)
/// @tparam H Intersection point information struct
///
template <typename T = float, class H = TriangleIntersection<T> >
class MotionTriangleIntersector {
 public:
  // Initialize from motion mesh object.
  explicit MotionTriangleIntersector(const MotionTriangleMesh<T> &mesh)
      : key_times_(mesh.GetKeyTimes()),
        num_key_times_(mesh.GetNumKeyTimes()),
        vertices_(mesh.GetVertices()),
        faces_(mesh.GetFaces()),
        vertex_stride_bytes_(mesh.GetVertexStrideBytes()) {}

  explicit MotionTriangleIntersector(const MotionTriangleMesh<T> *mesh)
      : key_times_(mesh->GetKeyTimes()),
        num_key_times_(mesh->GetNumKeyTimes()),
        vertices_(mesh->GetVertices()),
        faces_(mesh->GetFaces()),
        vertex_stride_bytes_(mesh->GetVertexStrideBytes()) {}

  MotionTriangleIntersector(const T *key_times, size_t num_key_times,
                            const T *const *vertices,
                            const unsigned int *faces,
                            const size_t vertex_stride_bytes)
      : key_times_(key_times),
        num_key_times_(num_key_times),
        vertices_(vertices),
        faces_(faces),
        vertex_stride_bytes_(vertex_stride_bytes) {}

  // For Watertight Ray/Triangle Intersection.
  typedef struct {
    T Sx;
    T Sy;
    T Sz;
    int kx;
    int ky;
    int kz;
  } RayCoeff;

  /// Do ray intersection stuff for `prim_index` th primitive and return hit
  /// distance `t`, barycentric coordinate `u` and `v`.
  /// Returns true if there's intersection.
  bool Intersect(T *t_inout, const unsigned int prim_index) const {
    if (!segment_valid_) {
      // Non-finite or out-of-domain ray time: never touch vertex data.
      return false;
    }

    if ((prim_index < trace_options_.prim_ids_range[0]) ||
        (prim_index >= trace_options_.prim_ids_range[1])) {
      return false;
    }

    // Self-intersection test.
    if (prim_index == trace_options_.skip_prim_id) {
      return false;
    }

    const unsigned int f0 = faces_[3 * prim_index + 0];
    const unsigned int f1 = faces_[3 * prim_index + 1];
    const unsigned int f2 = faces_[3 * prim_index + 2];

    const T *va = vertices_[segment_];
    const T *vb = vertices_[segment_ + 1];

    const T w0 = static_cast<T>(1.0) - alpha_;
    const T w1 = alpha_;

    // Interpolate vertex positions at the ray time. With `alpha_` equal to
    // 0 or 1 (exact key times) this reproduces the key frame exactly.
    real3<T> p0, p1, p2;
    for (int k = 0; k < 3; k++) {
      p0[k] = w0 * get_vertex_addr(va, f0, vertex_stride_bytes_)[k] +
              w1 * get_vertex_addr(vb, f0, vertex_stride_bytes_)[k];
      p1[k] = w0 * get_vertex_addr(va, f1, vertex_stride_bytes_)[k] +
              w1 * get_vertex_addr(vb, f1, vertex_stride_bytes_)[k];
      p2[k] = w0 * get_vertex_addr(va, f2, vertex_stride_bytes_)[k] +
              w1 * get_vertex_addr(vb, f2, vertex_stride_bytes_)[k];
    }

    const real3<T> A = p0 - ray_org_;
    const real3<T> B = p1 - ray_org_;
    const real3<T> C = p2 - ray_org_;

    const T Ax = A[ray_coeff_.kx] - ray_coeff_.Sx * A[ray_coeff_.kz];
    const T Ay = A[ray_coeff_.ky] - ray_coeff_.Sy * A[ray_coeff_.kz];
    const T Bx = B[ray_coeff_.kx] - ray_coeff_.Sx * B[ray_coeff_.kz];
    const T By = B[ray_coeff_.ky] - ray_coeff_.Sy * B[ray_coeff_.kz];
    const T Cx = C[ray_coeff_.kx] - ray_coeff_.Sx * C[ray_coeff_.kz];
    const T Cy = C[ray_coeff_.ky] - ray_coeff_.Sy * C[ray_coeff_.kz];

    T U = Cx * By - Cy * Bx;
    T V = Ax * Cy - Ay * Cx;
    T W = Bx * Ay - By * Ax;

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wfloat-equal"
#endif

    // Fall back to test against edges using double precision.
    if (U == static_cast<T>(0.0) || V == static_cast<T>(0.0) ||
        W == static_cast<T>(0.0)) {
      double CxBy = static_cast<double>(Cx) * static_cast<double>(By);
      double CyBx = static_cast<double>(Cy) * static_cast<double>(Bx);
      U = static_cast<T>(CxBy - CyBx);

      double AxCy = static_cast<double>(Ax) * static_cast<double>(Cy);
      double AyCx = static_cast<double>(Ay) * static_cast<double>(Cx);
      V = static_cast<T>(AxCy - AyCx);

      double BxAy = static_cast<double>(Bx) * static_cast<double>(Ay);
      double ByAx = static_cast<double>(By) * static_cast<double>(Ax);
      W = static_cast<T>(BxAy - ByAx);
    }

    if (U < static_cast<T>(0.0) || V < static_cast<T>(0.0) ||
        W < static_cast<T>(0.0)) {
      if (trace_options_.cull_back_face ||
          (U > static_cast<T>(0.0) || V > static_cast<T>(0.0) ||
           W > static_cast<T>(0.0))) {
        return false;
      }
    }

    T det = U + V + W;
    if (det == static_cast<T>(0.0)) return false;

#ifdef __clang__
#pragma clang diagnostic pop
#endif

    const T Az = ray_coeff_.Sz * A[ray_coeff_.kz];
    const T Bz = ray_coeff_.Sz * B[ray_coeff_.kz];
    const T Cz = ray_coeff_.Sz * C[ray_coeff_.kz];
    const T D = U * Az + V * Bz + W * Cz;

    const T rcpDet = static_cast<T>(1.0) / det;
    T tt = D * rcpDet;

    if (tt > (*t_inout)) {
      return false;
    }

    if (tt < t_min_) {
      return false;
    }

    (*t_inout) = tt;
    // Use Möller-Trumbore style barycentric coordinates
    // U + V + W = 1.0 and interp(p) = U * p0 + V * p1 + W * p2
    // We want interp(p) = (1 - u - v) * p0 + u * v1 + v * p2;
    // => u = V, v = W.
    u_ = V * rcpDet;
    v_ = W * rcpDet;

    return true;
  }

  /// Returns the nearest hit distance.
  T GetT() const { return t_; }

  /// Update is called when initializing intersection and nearest hit is found.
  void Update(T t, unsigned int prim_idx) const {
    t_ = t;
    prim_id_ = prim_idx;
  }

  /// Prepare BVH traversal (e.g. compute inverse ray direction and resolve
  /// the motion segment of `ray.time`).
  /// This function is called only once in BVH traversal.
  void PrepareTraversal(const Ray<T> &ray,
                        const BVHTraceOptions &trace_options) const {
    ray_org_[0] = ray.org[0];
    ray_org_[1] = ray.org[1];
    ray_org_[2] = ray.org[2];

    // Calculate dimension where the ray direction is maximal.
    ray_coeff_.kz = 0;
    T absDir = std::fabs(ray.dir[0]);
    if (absDir < std::fabs(ray.dir[1])) {
      ray_coeff_.kz = 1;
      absDir = std::fabs(ray.dir[1]);
    }
    if (absDir < std::fabs(ray.dir[2])) {
      ray_coeff_.kz = 2;
      absDir = std::fabs(ray.dir[2]);
    }

    ray_coeff_.kx = ray_coeff_.kz + 1;
    if (ray_coeff_.kx == 3) ray_coeff_.kx = 0;
    ray_coeff_.ky = ray_coeff_.kx + 1;
    if (ray_coeff_.ky == 3) ray_coeff_.ky = 0;

    // Swap kx and ky dimension to preserve winding direction of triangles.
    if (ray.dir[ray_coeff_.kz] < static_cast<T>(0.0))
      std::swap(ray_coeff_.kx, ray_coeff_.ky);

    // Calculate shear constants.
    ray_coeff_.Sx = ray.dir[ray_coeff_.kx] / ray.dir[ray_coeff_.kz];
    ray_coeff_.Sy = ray.dir[ray_coeff_.ky] / ray.dir[ray_coeff_.kz];
    ray_coeff_.Sz = static_cast<T>(1.0) / ray.dir[ray_coeff_.kz];

    trace_options_ = trace_options;

    t_min_ = ray.min_t;

    u_ = static_cast<T>(0.0);
    v_ = static_cast<T>(0.0);

    // Resolve the motion segment once per ray. This uses the same helper as
    // the BVH node culling, so both always agree on the segment.
    segment_valid_ = MotionTimeSegment(key_times_, num_key_times_, ray.time,
                                       &segment_, &alpha_);
  }

  /// Post BVH traversal stuff.
  /// Fill `isect` if there is a hit.
  void PostTraversal(const Ray<T> &ray, bool hit, H *isect) const {
    if (hit && isect) {
      (*isect).t = t_;
      (*isect).u = u_;
      (*isect).v = v_;
      (*isect).prim_id = prim_id_;
    }
    (void)ray;
  }

 private:
  const T *key_times_;
  const size_t num_key_times_;
  const T *const *vertices_;
  const unsigned int *faces_;
  const size_t vertex_stride_bytes_;

  mutable real3<T> ray_org_;
  mutable RayCoeff ray_coeff_;
  mutable BVHTraceOptions trace_options_;
  mutable T t_min_;

  mutable bool segment_valid_;
  mutable size_t segment_;
  mutable T alpha_;

  mutable T t_;
  mutable T u_;
  mutable T v_;
  mutable unsigned int prim_id_;
};

//
// Build a BVH over a motion triangle mesh.
//
// The tree topology and the swept node bounds stored in `BVHNode` are built
// once with the regular `Build()` path; afterwards conservative node bounds
// at every key time are attached so that `Traverse()` can cull nodes with
// time-varying bounds. No geometry is copied: queries interpolate the
// caller-owned vertex arrays directly.
//
template <typename T>
bool BVHAccel<T>::BuildMotion(const MotionTriangleMesh<T> &mesh,
                              const BVHBuildOptions<T> &options) {
  if (!mesh.IsValid()) {
    return false;
  }

  MotionTriangleSAHPred<T> pred(mesh);

  // Build() resets any previous motion state and builds the tree over the
  // swept bounds of the whole shutter interval.
  if (!Build(static_cast<unsigned int>(mesh.GetNumFaces()), mesh, pred,
             options)) {
    return false;
  }

  const size_t num_keys = mesh.GetNumKeyTimes();
  const size_t num_nodes = nodes_.size();

  motion_times_.assign(mesh.GetKeyTimes(), mesh.GetKeyTimes() + num_keys);
  motion_bboxes_.assign(num_nodes * num_keys, BBox<T>());

  // Node indices guarantee parents < children, so iterating in reverse
  // propagates leaf bounds up to the root.
  for (size_t ni = num_nodes; ni-- > 0;) {
    const BVHNode<T> &node = nodes_[ni];

    for (size_t k = 0; k < num_keys; k++) {
      BBox<T> &dst = motion_bboxes_[ni * num_keys + k];

      if (node.flag == 1) {
        // Leaf node: union of the primitive bounds at this key time.
        const unsigned int num_prims = node.data[0];
        const unsigned int offset = node.data[1];

        for (unsigned int i = 0; i < num_prims; i++) {
          const unsigned int prim_idx = indices_[offset + i];

          real3<T> bmin, bmax;
          mesh.BoundingBoxAtKey(&bmin, &bmax, prim_idx, k);

          for (int c = 0; c < 3; c++) {
            dst.bmin[c] = std::min(dst.bmin[c], bmin[c]);
            dst.bmax[c] = std::max(dst.bmax[c], bmax[c]);
          }
        }
      } else {
        // Branch node: union of the child bounds at this key time.
        const BBox<T> &c0 = motion_bboxes_[size_t(node.data[0]) * num_keys + k];
        const BBox<T> &c1 = motion_bboxes_[size_t(node.data[1]) * num_keys + k];

        for (int c = 0; c < 3; c++) {
          dst.bmin[c] = std::min(c0.bmin[c], c1.bmin[c]);
          dst.bmax[c] = std::max(c0.bmax[c], c1.bmax[c]);
        }
      }
    }
  }

  motion_enabled_ = true;

  return true;
}

}  // namespace nanort

#endif  // NANORT_MOTION_H_
