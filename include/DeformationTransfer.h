#ifndef DEFORMATIONTRANSFER_H
#define DEFORMATIONTRANSFER_H
/*
DeformationTransfer.h

Face-correspondence-based deformation transfer between two triangle meshes
of different topology, solved as a gradient-domain least-squares problem
with exact anchor constraints.

Given rest poses of a source and a target mesh, a list of face
correspondences, and fixed target anchor vertices, the object precomputes
and factorizes the (constant) sparse normal equations once.  Each call to
solve() only reassembles the right-hand side from a new source pose and new
anchor positions, so per-frame work is linear in the mesh size.

The object owns every piece of rest data it needs: the source and target
meshes passed to the constructor may be modified or deleted afterwards
without affecting it.

Gradient convention (see README for details):
  For a triangle with vertices p0, p1, p2:
    e1 = p1 - p0, e2 = p2 - p0, a = e1 x e2
    F = [ e1  e2  a / sqrt(|a|) ]
  The source deformation gradient of a face is S = F_current F_rest^-1.
  The third frame column uses the sqrt-area-scaled normal (its length is
  sqrt(|a|) = sqrt(2 * area)), not a unit normal.

Minimized energy:
  E = sum_correspondences A_target * weight *
      || sum_i x_i g_i^T - S_source P_target ||_F^2
  where g_i are the in-plane barycentric coordinate gradients of the
  target rest face, P_target projects onto its tangent plane, A_target is
  its area, and x_i are the unknown target positions.  Anchor vertices are
  imposed as exact constraints.
*/

#include "TriMesh.h"
#include <cstddef>
#include <vector>


namespace trimesh {

// One face correspondence: the deformation gradient of source_face is
// transferred onto target_face with the given positive weight.
// Several source faces may map to the same target face; every target face
// must be covered by at least one correspondence.
struct FaceCorrespondence {
	int source_face;
	int target_face;
	double weight;

	FaceCorrespondence() : source_face(-1), target_face(-1), weight(0.0)
		{}
	FaceCorrespondence(int sf, int tf, double w)
		: source_face(sf), target_face(tf), weight(w)
		{}
};

// Output of a solve().  mesh is a newly allocated TriMesh owned by the
// caller (delete it when done) and is NULL on failure.  energy and
// residual are quiet NaNs on failure; residual is the free-variable
// relative infinity-norm residual
//   || H x - b ||_inf / max(1, ||b||_inf).
struct DeformationTransferResult {
	TriMesh *mesh;
	double energy;
	double residual;

	DeformationTransferResult()
		: mesh(0), energy(0.0), residual(0.0)
		{}
	bool ok() const
		{ return mesh != 0; }
};


class DeformationTransfer {
public:
	// Builds and preprocesses the constant sparse system.  Both meshes must
	// be finite, non-degenerate, consistently oriented 2-manifold triangle
	// meshes without isolated or unused vertices; open boundaries and
	// multiple connected components are allowed, but every component needs
	// at least one anchor.  Throws std::invalid_argument for bad face or
	// anchor indices, non-positive/non-finite weights, uncovered target
	// faces, duplicate anchors, components without an anchor, or meshes
	// that violate the assumptions above.
	DeformationTransfer(const TriMesh &source_rest,
	                    const TriMesh &target_rest,
	                    const ::std::vector<FaceCorrespondence> &correspondences,
	                    const ::std::vector<int> &target_anchors);

	~DeformationTransfer();

	// Solve for a new frame.  source_positions are the current source
	// vertices in their original (rest) vertex ordering; anchor_positions
	// give the exact target positions of the constructor's anchors, in the
	// same order.  A failed solve (degenerate current source triangle,
	// non-finite coordinates, or solver non-convergence) leaves the object
	// and every previously returned mesh untouched, and the next frame can
	// be solved normally.  Size mismatches throw std::invalid_argument.
	DeformationTransferResult solve(
		const ::std::vector<point> &source_positions,
		const ::std::vector<dvec3> &anchor_positions) const;
	DeformationTransferResult solve(
		const ::std::vector<point> &source_positions,
		const ::std::vector<point> &anchor_positions) const;

	// Number of times the sparse operator has been built: always 1, since
	// assembly and preprocessing happen only in the constructor.
	::std::size_t operator_build_count() const
		{ return operator_builds; }

	// Number of successful solve() calls over the object's lifetime.
	::std::size_t successful_solve_count() const
		{ return successful_solves; }

	// Number of unconstrained scalar degrees of freedom.
	::std::size_t free_variable_count() const
		{ return nfree; }

	// Number of stored sparse matrix entries (diagonal plus upper triangle
	// of the reduced system).  Grows linearly with mesh size.
	::std::size_t stored_nonzeros() const;

private:
	// No reason to copy these objects so far; disallow until needed.
	DeformationTransfer(const DeformationTransfer &);
	DeformationTransfer &operator = (const DeformationTransfer &);

protected:
	// Rest geometry, owned here (stored in double precision).
	int nsource_verts, ntarget_verts, ntarget_faces;
	::std::vector<dvec3> source_rest;
	::std::vector<dvec3> target_rest;
	::std::vector<TriMesh::Face> target_faces;

	// Per-vertex target attributes preserved on output.
	::std::vector<Color> target_colors;
	::std::vector<float> target_confidences;
	::std::vector<unsigned> target_flags;
	unsigned target_flag_curr;

	// Purely topological target caches, valid for the same connectivity
	// and therefore retained on the output meshes.
	::std::vector<int> target_tstrips;
	::std::vector<int> target_grid;
	int target_grid_width, target_grid_height;
	::std::vector< ::std::vector<int> > target_neighbors;
	::std::vector< ::std::vector<int> > target_adjacentfaces;
	::std::vector<TriMesh::Face> target_across_edge;

	// Source rest frames, indexed by source face.
	struct SourceFace {
		int v[3];
		double inv_frame[3][3]; // inverse of the rest frame F_rest
	};
	::std::vector<SourceFace> source_faces;

	// Per-target-face static data.
	struct TargetFace {
		int v[3];
		double area;            // A
		double grad[3][3];      // g_i as rows, in the rest tangent plane
		double proj[3][3];      // P, tangent-plane projection
	};
	::std::vector<TargetFace> target_face_data;

	// Correspondences grouped by target face.
	struct Correspondence {
		int source_face;
		double weight;
		Correspondence() : source_face(-1), weight(0.0)
			{}
		Correspondence(int sf, double w) : source_face(sf), weight(w)
			{}
	};
	::std::vector< ::std::vector<Correspondence> > correspondences;

	// Anchor bookkeeping.
	::std::vector<int> anchors;          // target vertex ids, given order
	::std::vector<int> anchor_lookup;    // target vertex id -> anchor slot
	::std::vector<int> free_index;       // vertex id -> free vertex index
	int nfree;                           // number of free vertices

	// Reduced sparse normal equations, symmetric: diagonal plus upper
	// triangle stored per free scalar dof (3 per free vertex).
	::std::vector<double> h_diag;       // original operator diagonal
	::std::vector< ::std::vector< ::std::pair<int, double> > > h_upper;
	// diag holds the D factor and l_upper holds the L factor of the
	// one-time LDL^T factorization (l_upper stores only off-diagonal
	// entries, in the same row order as h_upper plus symbolic fill-in).
	::std::vector<double> diag;
	::std::vector< ::std::vector< ::std::pair<int, double> > > l_upper;

	// Coupling between free dofs and anchored dofs in the full system,
	// used to update the right-hand side when anchors move.
	::std::vector< ::std::vector< ::std::pair<int, double> > > anchor_terms;

	::std::size_t operator_builds;
	mutable ::std::size_t successful_solves;

	DeformationTransferResult solve_impl(
		const ::std::vector<point> &source_positions,
		const ::std::vector<dvec3> &anchor_positions) const;
};

} // namespace trimesh

#endif
