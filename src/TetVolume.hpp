#ifndef TET_VOLUME_HPP
#define TET_VOLUME_HPP

// Tetrahedral volume resource and deformable pose.
//
// A TetVolume owns the shared, read-only resource data: rest vertices,
// four-index tetrahedral cells, per-vertex non-negative density, and
// per-cell non-negative RGB extinction sigma and source S.  One or more
// TetPose objects can be created from the same resource; every pose carries
// its own independently updatable vertex positions and its own mutable
// caches (inverse barycentric maps, per-cell bounds and a refittable BVH).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include "Vector.h"

namespace tetvol {

struct RGB {
   double r, g, b;
   RGB() : r(0.0), g(0.0), b(0.0) {}
   RGB(double r_, double g_, double b_) : r(r_), g(g_), b(b_) {}
};

struct TetCell {
   // Vertex indices into the resource vertex array; all four distinct.
   std::array<int, 4> v;
   RGB sigma;  // non-negative RGB extinction coefficient
   RGB source; // non-negative RGB source function S
};

struct AABB {
   Vector mn;
   Vector mx;
   bool valid;
   AABB() : mn(Vector(0.0, 0.0, 0.0)), mx(Vector(0.0, 0.0, 0.0)),
            valid(false) {}
};

class TetPose;

// Finite-ray slab test of an AABB against [t0, t1]; returns false when the
// overlap has no positive length within the window.  Handles axis-parallel
// directions (zero direction components).  Defined inline so translation
// units that only optionally use a volume do not gain a link dependency.
inline bool rayAABB(const Vector& origin, const Vector& dir, double t0,
                    double t1, const AABB& box, double& hitLo,
                    double& hitHi) {
   double lo = t0;
   double hi = t1;
   const double* o = &origin.x;
   const double* d = &dir.x;
   const double* mn = &box.mn.x;
   const double* mx = &box.mx.x;
   for (int axis = 0; axis < 3; ++axis) {
      if (d[axis] == 0.0) {
         if (o[axis] < mn[axis] || o[axis] > mx[axis]) return false;
      } else {
         double ta = (mn[axis] - o[axis]) / d[axis];
         double tb = (mx[axis] - o[axis]) / d[axis];
         if (ta > tb) {
            double tmp = ta;
            ta = tb;
            tb = tmp;
         }
         lo = std::max(lo, ta);
         hi = std::min(hi, tb);
         if (lo > hi) return false;
      }
   }
   hitLo = lo;
   hitHi = hi;
   return true;
}

// Immutable resource contents, shared by every pose of one volume.
struct TetVolumeData {
   std::vector<Vector> rest;
   std::vector<TetCell> cells;
   std::vector<double> density;
   std::map<std::tuple<int, int, int>, int> faceOwner;
   std::map<std::pair<int, int>, int> edgeOwner;
   // Incident cell ids per vertex, so a pose update touches only associated
   // cells without scanning the whole cell array.
   std::vector<std::vector<int>> vertexCells;
};

// Shared, immutable tetrahedral resource.  A TetVolume is a lightweight,
// reference-semantic handle: copies share the same underlying data, and poses
// created from it keep that data alive for their own lifetime.
class TetVolume {
public:
   // Throws std::invalid_argument for non-finite values, illegal or repeated
   // indices, negative density / sigma / source, or a degenerate or flipped
   // rest cell (rest cells are required to be positively oriented).
   TetVolume(const std::vector<Vector>& restVertices,
             const std::vector<TetCell>& cells,
             const std::vector<double>& density);

   std::size_t vertexCount() const { return data_->rest.size(); }
   std::size_t cellCount() const { return data_->cells.size(); }

   const Vector& restVertex(int i) const {
      return data_->rest[static_cast<std::size_t>(i)];
   }
   const TetCell& cell(std::size_t c) const { return data_->cells[c]; }
   double vertexDensity(int i) const {
      return data_->density[static_cast<std::size_t>(i)];
   }

   // Creates a new pose whose positions initially equal the rest positions.
   // The pose shares this resource but keeps its own mutable caches.
   std::shared_ptr<TetPose> createPose() const;

   // Smallest cell index incident on the face/edge named by the sorted vertex
   // ids.  Used to uniquely attribute a ray propagating exactly on a shared
   // boundary.  Every named face/edge of a valid cell has an owner.
   int faceOwner(int a, int b, int c) const;
   int edgeOwner(int a, int b) const;

   // Ids of the cells incident on a vertex.
   const std::vector<int>& incidentCells(int vertex) const {
      return data_->vertexCells[static_cast<std::size_t>(vertex)];
   }

private:
   std::shared_ptr<TetVolumeData> data_;

   friend class TetPose;
};

// A deformable pose of a TetVolume.
class TetPose {
public:
   // Copies the lightweight resource handle, so the pose keeps the shared
   // resource data alive independently of the original TetVolume object.
   explicit TetPose(const TetVolume& volume);

   // Batch absolute-position update keyed by vertex id.  Ids must be unique
   // and in range, positions must be finite.  Any cell incident on a moved
   // vertex must stay non-degenerate and positively oriented; otherwise the
   // whole update is rejected (old geometry, query results and generation are
   // preserved) and std::invalid_argument is thrown.  An empty update changes
   // nothing, including the generation.
   void update(const std::vector<int>& ids, const std::vector<Vector>& positions);

   std::uint64_t generation() const { return generation_; }
   std::uint64_t totalBuilds() const { return totalBuilds_; }
   std::uint64_t inverseRecomputations() const { return inverseRecomputations_; }
   std::uint64_t boundsRefits() const { return boundsRefits_; }

   const Vector& position(int i) const { return positions_[static_cast<std::size_t>(i)]; }
   const TetVolume& volume() const { return volume_; }
   const AABB& rootBounds() const { return nodes_[static_cast<std::size_t>(root_)].bounds; }

   // Inverse barycentric map of a cell in the current pose.  For a ray
   // p(t) = origin + t * dir, barycentric coordinate k (k = 1..3) equals
   // rowK . (p(t) - p0); lambda0 = 1 - lambda1 - lambda2 - lambda3.
   struct InvMap {
      Vector p0;
      Vector row1;
      Vector row2;
      Vector row3;
   };
   const InvMap& inverseMap(std::size_t cell) const { return inv_[cell]; }
   const AABB& cellBounds(std::size_t cell) const { return cellBounds_[cell]; }

   // Read-only BVH accessors used by TetCursor's lazy traversal.
   struct NodeView {
      const AABB& bounds;
      bool leaf;
      int left;
      int right;
      int first;
      int last;
   };
   int rootIndex() const { return root_; }
   NodeView nodeView(int index) const;
   int leafCell(int orderIndex) const {
      return leafOrder_[static_cast<std::size_t>(orderIndex)];
   }

private:
   struct BVHNode {
      AABB bounds;
      int parent; // parent node index, or -1 for the root
      int left;  // child node index, or -1 for a leaf
      int right;
      int first; // leaf range [first, last) into leafOrder_
      int last;
   };

   void buildAll();
   void recomputeCell(std::size_t c);
   int buildRecursive(int first, int last, int depth);

   // Resource handle; holds a reference to the shared data for the pose's
   // whole lifetime, independent of any other TetVolume handle.
   TetVolume volume_;
   std::vector<Vector> positions_;
   std::vector<InvMap> inv_;
   std::vector<AABB> cellBounds_;

   std::vector<BVHNode> nodes_;
   std::vector<int> leafOrder_; // cell indices
   std::vector<int> leafNode_;  // leaf node index per cell
   int root_;

   std::uint64_t generation_;
   std::uint64_t totalBuilds_;
   std::uint64_t inverseRecomputations_;
   std::uint64_t boundsRefits_;
};

} // namespace tetvol

#endif
