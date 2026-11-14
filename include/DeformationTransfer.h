#ifndef DEFORMATION_TRANSFER_H
#define DEFORMATION_TRANSFER_H
/*
DeformationTransfer.h

Face-correspondence gradient-domain deformation transfer.

Transfers the per-frame deformation of a source triangle mesh onto a
target triangle mesh of different topology.  Instead of copying vertex
positions or transforming each target triangle independently (which
cracks the surface), the target deformation is computed as the global
minimizer of a gradient-domain energy with exact positional anchor
constraints.

Fixed conventions
-----------------
Triangle frame (columns):
    e1 = p1 - p0, e2 = p2 - p0, a = e1 x e2,
    F = [ e1, e2, a / sqrt(|a|) ]
The third (normal) column uses the sqrt-area scale a/sqrt(|a|),
*not* a unit normal.

Source face deformation gradient:
    S = F_current * F_rest^-1          (3x3)

Target rest face: g_i are the 3D in-plane gradients of the linear
barycentric basis functions, P is the projection onto the face's
tangent plane, and A is the face area.

The transferred target positions x are the global minimizer of
    E = sum_correspondences  A_target * weight *
        || sum_i x_i g_i^T - S_source * P_target ||_F^2
subject to exact anchor constraints x_anchor = given positions.

A global translation of the source never enters the gradient; global
rigid motion and uniform scaling of the source are recovered on the
target only through the anchors.  No per-face transform averaging,
rotation re-estimation, or plain smoothing is performed: changing any
of those would change the objective above.

Usage
-----
Both rest meshes must be finite, non-degenerate, consistently oriented
2-manifold triangle meshes without isolated or unused vertices.  Open
boundaries and multiple connected components are allowed; every
connected component of the target needs at least one anchor.  The
constructor validates the rest meshes, the correspondences, and the
anchors, and throws std::invalid_argument on any violation.  It takes
ownership of nothing: it deep-copies everything it needs, so the
inputs may be modified or destroyed afterwards.

Each correspondence { source_face, target_face, weight } requires a
positive, finite double weight.  A target face may be driven by
several source faces (its contributions accumulate), and every target
face must be covered by at least one correspondence.  Correspondences
and anchor vertex indices are fixed for the lifetime of the object;
only the source pose and the anchor positions change per frame.

solve() expects the current source positions in the original source
vertex numbering, and the current anchor positions in anchor order.
On success it writes a fresh, independent TriMesh (target topology,
target faces/colors/confidences/flags, target adjacency caches; all
position-dependent caches invalidated), returns the energy E at the
solution and the free-row residual
    ||Hx - b||_inf / max(1, ||b||_inf)   (<= 1e-8 on success),
and returns true.  Anchor constraints are satisfied exactly (in
double, before any conversion back to float).

solve() fails explicitly (returns false, output and internal state
left exactly as after the last successful solve, so the next frame
can still be solved) when a current source triangle is degenerate,
when any input coordinate is non-finite, or when the linear solve
does not converge.  Passing wrongly-sized input vectors throws
std::invalid_argument.

All storage is in double with sparse (CSR) structures; no matrix
quadratic in the vertex count is ever allocated.  Operator assembly,
anchor elimination, and the preconditioner happen once, in the
constructor; a new source pose or new anchor positions only rebuild
the right-hand side.  The number of times the operator was actually
built and the number of successful solves are recorded.

C++11, standard library only.
*/

#include "TriMesh.h"
#include <cstddef>
#include <vector>


namespace trimesh {

class DeformationTransfer {
public:
	// One face correspondence: target_face is driven by the
	// deformation gradient of source_face, with the given
	// positive weight.
	struct Correspondence {
		int source_face;
		int target_face;
		double weight;
		Correspondence() : source_face(-1), target_face(-1), weight(0.0)
			{}
		Correspondence(int s, int t, double w)
			: source_face(s), target_face(t), weight(w)
			{}
	};

	// Validating constructor.  Deep-copies the rest meshes and
	// builds the (sparse) system operator, the anchor
	// elimination, and the preconditioner.  Throws
	// std::invalid_argument on invalid meshes, indices,
	// coverage, weights, or anchors.
	DeformationTransfer(const TriMesh &source_rest,
	                    const TriMesh &target_rest,
	                    const ::std::vector<Correspondence> &correspondences,
	                    const ::std::vector<int> &anchors);

	// Non-copyable, non-assignable (owning).
	DeformationTransfer(const DeformationTransfer &) = delete;
	DeformationTransfer &operator = (const DeformationTransfer &) = delete;

	// Transfer one frame.  See file comment above for the exact
	// contract.  source_positions must have one entry per source
	// rest vertex; anchor_positions must have one entry per
	// anchor, in the order the anchors were given to the
	// constructor.
	bool solve(const ::std::vector<point> &source_positions,
	           const ::std::vector<point> &anchor_positions,
	           TriMesh &output, double &energy, double &residual);

	// Statistics.
	int operator_builds() const { return builds; }
	long successful_solves() const { return solves_ok; }
	::std::size_t operator_nonzeros() const { return ff_val.size(); }
	::std::size_t num_free_vertices() const { return ff_diag.size(); }
	::std::size_t num_anchors() const { return anchor_vertices.size(); }

private:
	// Rest geometry, owned deep copies (double precision)
	::std::vector<dvec3> src_rest;          // source rest positions
	::std::vector<TriMesh::Face> src_faces; // source rest topology
	::std::vector<dvec3> tgt_rest;          // target rest positions

	// Per-source-face rest data: inverse of the rest frame F_rest,
	// 9 row-major doubles per face
	::std::vector<double> src_inv_frest;

	// Per-target-face rest data
	::std::vector<dvec3> tgt_grad;   // 3 per face: g_0, g_1, g_2
	::std::vector<dvec3> tgt_normal; // unit rest normal (for P)
	::std::vector<double> tgt_area;  // face area A

	// Validated correspondences
	::std::vector<Correspondence> corr;

	// Anchor / free-variable structure of the target
	::std::vector<int> anchor_vertices;   // anchor vertex indices
	::std::vector<int> anchor_of_vertex;  // vertex -> anchor slot, or -1
	::std::vector<int> free_of_vertex;    // vertex -> free index, or -1

	// Sparse system operator over free vertices (scalar CSR, the
	// same matrix applies to the x, y, and z components), plus
	// the free x anchor coupling used to eliminate anchors into
	// the right-hand side, plus the Jacobi preconditioner.
	::std::vector< ::std::size_t > ff_row;
	::std::vector<int> ff_col;
	::std::vector<double> ff_val;
	::std::vector< ::std::size_t > fa_row;
	::std::vector<int> fa_col;
	::std::vector<double> fa_val;
	::std::vector<double> ff_diag;

	// Template for the output mesh: target topology and
	// attributes, including adjacency caches (position-dependent
	// caches are invalidated on every output).
	TriMesh target_template;

	// Warm-start state from the last successful solve
	::std::vector<double> last_x;
	bool have_last_x;

	// Statistics
	int builds;
	long solves_ok;
};

} // namespace trimesh

#endif
