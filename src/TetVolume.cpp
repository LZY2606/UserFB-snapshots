#include "TetVolume.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

namespace tetvol {

namespace {

bool finiteVec(const Vector& v) {
   return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool finiteRGB(const RGB& c) {
   return std::isfinite(c.r) && std::isfinite(c.g) && std::isfinite(c.b);
}

// Six times the signed volume of a tetrahedron; positive for a positively
// oriented cell.
double signedDet(const Vector& p0, const Vector& p1,
                 const Vector& p2, const Vector& p3) {
   Vector e1 = p1 - p0;
   Vector e2 = p2 - p0;
   Vector e3 = p3 - p0;
   return e1.dot(e2.cross(e3));
}

AABB boundsOf(const Vector* const pts[4]) {
   AABB b;
   b.valid = true;
   b.mn = Vector(std::numeric_limits<double>::infinity(),
                  std::numeric_limits<double>::infinity(),
                  std::numeric_limits<double>::infinity());
   b.mx = Vector(-std::numeric_limits<double>::infinity(),
                  -std::numeric_limits<double>::infinity(),
                  -std::numeric_limits<double>::infinity());
   for (int k = 0; k < 4; ++k) {
      b.mn.x = std::min(b.mn.x, pts[k]->x);
      b.mn.y = std::min(b.mn.y, pts[k]->y);
      b.mn.z = std::min(b.mn.z, pts[k]->z);
      b.mx.x = std::max(b.mx.x, pts[k]->x);
      b.mx.y = std::max(b.mx.y, pts[k]->y);
      b.mx.z = std::max(b.mx.z, pts[k]->z);
   }
   return b;
}

void unionInto(AABB& dst, const AABB& src) {
   if (!src.valid) return;
   if (!dst.valid) { dst = src; return; }
   dst.mn.x = std::min(dst.mn.x, src.mn.x);
   dst.mn.y = std::min(dst.mn.y, src.mn.y);
   dst.mn.z = std::min(dst.mn.z, src.mn.z);
   dst.mx.x = std::max(dst.mx.x, src.mx.x);
   dst.mx.y = std::max(dst.mx.y, src.mx.y);
   dst.mx.z = std::max(dst.mx.z, src.mx.z);
}

} // namespace

TetVolume::TetVolume(const std::vector<Vector>& restVertices,
                     const std::vector<TetCell>& cells,
                     const std::vector<double>& density) {
   data_ = std::make_shared<TetVolumeData>();
   if (restVertices.empty() || cells.empty()) {
      throw std::invalid_argument("TetVolume: vertices and cells must be non-empty");
   }
   if (density.size() != restVertices.size()) {
      throw std::invalid_argument("TetVolume: density count must match vertex count");
   }
   for (const Vector& v : restVertices) {
      if (!finiteVec(v)) throw std::invalid_argument("TetVolume: non-finite rest vertex");
   }
   for (double d : density) {
      if (!std::isfinite(d) || d < 0.0) {
         throw std::invalid_argument("TetVolume: density must be finite and non-negative");
      }
   }

   const int nv = static_cast<int>(restVertices.size());
   for (std::size_t ci = 0; ci < cells.size(); ++ci) {
      const TetCell& cell = cells[ci];
      for (int k = 0; k < 4; ++k) {
         if (cell.v[k] < 0 || cell.v[k] >= nv) {
            throw std::invalid_argument("TetVolume: cell index out of range");
         }
         for (int j = 0; j < k; ++j) {
            if (cell.v[j] == cell.v[k]) {
               throw std::invalid_argument("TetVolume: repeated index in cell");
            }
         }
      }
      if (!finiteRGB(cell.sigma) || cell.sigma.r < 0.0 || cell.sigma.g < 0.0 ||
          cell.sigma.b < 0.0) {
         throw std::invalid_argument("TetVolume: sigma must be finite and non-negative");
      }
      if (!finiteRGB(cell.source) || cell.source.r < 0.0 || cell.source.g < 0.0 ||
          cell.source.b < 0.0) {
         throw std::invalid_argument("TetVolume: source S must be finite and non-negative");
      }
      double det = signedDet(restVertices[cell.v[0]], restVertices[cell.v[1]],
                             restVertices[cell.v[2]], restVertices[cell.v[3]]);
      if (!std::isfinite(det) || det <= 0.0) {
         throw std::invalid_argument(
            "TetVolume: rest cell is degenerate or not positively oriented");
      }

      // Faces: each omission of one vertex; edges: all six pairs.  Cells are
      // visited in order, so the first (smallest index) cell wins ownership.
      static const int faceK[4][3] = {{1, 2, 3}, {0, 2, 3}, {0, 1, 3}, {0, 1, 2}};
      for (int f = 0; f < 4; ++f) {
         int a[3] = {cell.v[faceK[f][0]], cell.v[faceK[f][1]], cell.v[faceK[f][2]]};
         std::sort(a, a + 3);
         data_->faceOwner.emplace(std::make_tuple(a[0], a[1], a[2]),
                                  static_cast<int>(ci));
      }
      for (int i = 0; i < 4; ++i) {
         for (int j = i + 1; j < 4; ++j) {
            int a = cell.v[i];
            int b = cell.v[j];
            if (a > b) std::swap(a, b);
            data_->edgeOwner.emplace(std::make_pair(a, b),
                                     static_cast<int>(ci));
         }
      }
   }

   // Copy the validated inputs: later external mutation cannot reach the
   // resource.
   data_->rest = restVertices;
   data_->cells = cells;
   data_->density = density;

   // Build the vertex -> incident cell adjacency once at construction.
   data_->vertexCells.assign(static_cast<std::size_t>(nv),
                             std::vector<int>());
   for (std::size_t ci = 0; ci < cells.size(); ++ci) {
      for (int k = 0; k < 4; ++k) {
         data_->vertexCells[static_cast<std::size_t>(cells[ci].v[k])]
            .push_back(static_cast<int>(ci));
      }
   }
}

int TetVolume::faceOwner(int a, int b, int c) const {
   auto it = data_->faceOwner.find(std::make_tuple(a, b, c));
   return it == data_->faceOwner.end() ? -1 : it->second;
}

int TetVolume::edgeOwner(int a, int b) const {
   auto it = data_->edgeOwner.find(std::make_pair(a, b));
   return it == data_->edgeOwner.end() ? -1 : it->second;
}

std::shared_ptr<TetPose> TetVolume::createPose() const {
   return std::shared_ptr<TetPose>(new TetPose(*this));
}

TetPose::TetPose(const TetVolume& volume)
   : volume_(volume), root_(-1), generation_(1), totalBuilds_(1),
     inverseRecomputations_(0), boundsRefits_(0) {
   positions_ = volume_.data_->rest;
   buildAll();
}

void TetPose::buildAll() {
   const std::size_t nc = volume_.cellCount();
   inv_.resize(nc);
   cellBounds_.resize(nc);
   leafNode_.assign(nc, -1);
   for (std::size_t c = 0; c < nc; ++c) {
      recomputeCell(c);
   }

   leafOrder_.resize(nc);
   for (std::size_t c = 0; c < nc; ++c) {
      leafOrder_[c] = static_cast<int>(c);
   }
   nodes_.clear();
   root_ = buildRecursive(0, static_cast<int>(nc), 0);
}

void TetPose::recomputeCell(std::size_t c) {
   const TetCell& cell = volume_.cell(c);
   const Vector& p0 = positions_[cell.v[0]];
   const Vector& p1 = positions_[cell.v[1]];
   const Vector& p2 = positions_[cell.v[2]];
   const Vector& p3 = positions_[cell.v[3]];

   Vector e1 = p1 - p0;
   Vector e2 = p2 - p0;
   Vector e3 = p3 - p0;
   double det = e1.dot(e2.cross(e3));
   // Callers validate orientation before committing; this also guards the
   // initial build and keeps the cache finite.
   if (!std::isfinite(det) || det <= 0.0) {
      throw std::invalid_argument("TetPose: degenerate or flipped cell");
   }
   double invDet = 1.0 / det;

   InvMap& m = inv_[c];
   m.p0 = p0;
   m.row1 = e2.cross(e3) * invDet;
   m.row2 = e3.cross(e1) * invDet;
   m.row3 = e1.cross(e2) * invDet;

   const Vector* pts[4] = {&p0, &p1, &p2, &p3};
   cellBounds_[c] = boundsOf(pts);
}

TetPose::NodeView TetPose::nodeView(int index) const {
   const BVHNode& n = nodes_[static_cast<std::size_t>(index)];
   return NodeView{n.bounds, n.left == -1, n.left, n.right, n.first, n.last};
}

int TetPose::buildRecursive(int first, int last, int depth) {
   BVHNode node;
   node.left = node.right = -1;
   node.parent = -1;
   node.first = first;
   node.last = last;

   AABB bound;
   bound.valid = false;
   Vector centroidSum(0.0, 0.0, 0.0);
   for (int i = first; i < last; ++i) {
      const AABB& cb = cellBounds_[leafOrder_[i]];
      unionInto(bound, cb);
      centroidSum += (cb.mn + cb.mx) * 0.5;
   }
   node.bounds = bound;

   const int leafLimit = 4;
   if (last - first <= leafLimit || depth > 64) {
      int idx = static_cast<int>(nodes_.size());
      nodes_.push_back(node);
      for (int i = first; i < last; ++i) {
         leafNode_[leafOrder_[i]] = idx;
      }
      return idx;
   }

   // Split along the longest bounds axis at the mean centroid.
   Vector extent = bound.mx - bound.mn;
   int axis = 0;
   if (extent.y > extent.x && extent.y > extent.z) axis = 1;
   if (extent.z > extent.x && extent.z > extent.y) axis = 2;
   double center = (&centroidSum.x)[axis] / (last - first);
   int split = first;
   for (int i = first; i < last; ++i) {
      const AABB& cb = cellBounds_[leafOrder_[i]];
      double c = (&cb.mn.x)[axis] * 0.5 + (&cb.mx.x)[axis] * 0.5;
      if (c < center) {
         std::swap(leafOrder_[i], leafOrder_[split]);
         ++split;
      }
   }
   if (split == first || split == last) {
      split = first + (last - first) / 2;
   }

   int idx = static_cast<int>(nodes_.size());
   nodes_.push_back(node); // placeholder so children get larger indices
   int left = buildRecursive(first, split, depth + 1);
   int right = buildRecursive(split, last, depth + 1);
   nodes_[idx].left = left;
   nodes_[idx].right = right;
   nodes_[left].parent = idx;
   nodes_[right].parent = idx;
   return idx;
}

void TetPose::update(const std::vector<int>& ids,
                     const std::vector<Vector>& positions) {
   // Empty update preserves geometry, cached query results and generation.
   if (ids.empty()) {
      if (!positions.empty()) {
         throw std::invalid_argument("TetPose::update: ids/positions size mismatch");
      }
      return;
   }
   if (ids.size() != positions.size()) {
      throw std::invalid_argument("TetPose::update: ids/positions size mismatch");
   }
   const int nv = static_cast<int>(volume_.vertexCount());
   std::vector<char> seen(static_cast<std::size_t>(nv), 0);
   for (std::size_t i = 0; i < ids.size(); ++i) {
      int id = ids[i];
      if (id < 0 || id >= nv) {
         throw std::invalid_argument("TetPose::update: vertex id out of range");
      }
      if (seen[id]) {
         throw std::invalid_argument("TetPose::update: duplicate vertex id");
      }
      seen[id] = 1;
      if (!finiteVec(positions[i])) {
         throw std::invalid_argument("TetPose::update: non-finite position");
      }
   }

   // Find the unique set of incident cells through the resource adjacency.
   // The topology itself is never rebuilt, and unrelated cells are not
   // visited.
   std::vector<int> dirtyCells;
   for (std::size_t i = 0; i < ids.size(); ++i) {
      for (int c : volume_.incidentCells(ids[i])) {
         dirtyCells.push_back(c);
      }
   }
   std::sort(dirtyCells.begin(), dirtyCells.end());
   dirtyCells.erase(std::unique(dirtyCells.begin(), dirtyCells.end()),
                    dirtyCells.end());

   // Validate the tentative geometry before touching any cached state, so a
   // rejection leaves every old query result usable.
   for (int ci : dirtyCells) {
      std::size_t c = static_cast<std::size_t>(ci);
      const TetCell& cell = volume_.cell(c);
      Vector pts[4];
      for (int k = 0; k < 4; ++k) {
         int vid = cell.v[k];
         pts[k] = positions_[static_cast<std::size_t>(vid)];
         for (std::size_t j = 0; j < ids.size(); ++j) {
            if (ids[j] == vid) { pts[k] = positions[j]; break; }
         }
      }
      double det = signedDet(pts[0], pts[1], pts[2], pts[3]);
      if (!std::isfinite(det) || det <= 0.0) {
         throw std::invalid_argument(
            "TetPose::update: update would degenerate or flip a cell");
      }
   }

   // Commit positions.
   for (std::size_t i = 0; i < ids.size(); ++i) {
      positions_[static_cast<std::size_t>(ids[i])] = positions[i];
   }

   // Recompute only the inverse barycentric maps and bounds of incident cells.
   for (int ci : dirtyCells) {
      recomputeCell(static_cast<std::size_t>(ci));
   }
   inverseRecomputations_ += static_cast<std::uint64_t>(dirtyCells.size());

   // Refit the existing BVH once, walking each affected leaf's parent chain.
   std::unordered_set<int> touched;
   for (int ci : dirtyCells) {
      touched.insert(leafNode_[static_cast<std::size_t>(ci)]);
   }
   for (int n : std::vector<int>(touched.begin(), touched.end())) {
      int p = nodes_[static_cast<std::size_t>(n)].parent;
      while (p != -1) {
         touched.insert(p);
         p = nodes_[static_cast<std::size_t>(p)].parent;
      }
   }
   for (int n : touched) {
      BVHNode& node = nodes_[static_cast<std::size_t>(n)];
      AABB b;
      b.valid = false;
      if (node.left == -1) {
         for (int i = node.first; i < node.last; ++i) {
            unionInto(b, cellBounds_[static_cast<std::size_t>(
                             leafOrder_[static_cast<std::size_t>(i)])]);
         }
      } else {
         unionInto(b, nodes_[static_cast<std::size_t>(node.left)].bounds);
         unionInto(b, nodes_[static_cast<std::size_t>(node.right)].bounds);
      }
      node.bounds = b;
   }
   boundsRefits_ += static_cast<std::uint64_t>(touched.size());

   ++generation_;
   ++totalBuilds_;
}

} // namespace tetvol
