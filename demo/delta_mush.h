#ifndef DEMO_DELTA_MUSH_H
#define DEMO_DELTA_MUSH_H

// Detail-preserving skinning for IQM meshes.
//
// At bind time the mesh neighbourhood diffusion S = (I + lambda*L)^(-p)
// (combinatorial Laplacian L = D - A over the deduplicated undirected
// triangle-edge graph, keyed by vertex index) is folded, together with the
// blend weights, into per-vertex/per-joint moments
//     Omega_ij = sum_k S_ik * w_kj * u_k * u_k^T,  u_k = (x_k, 1).
// At playback only these moments are accumulated against the joint palette
// M_j (model-space joint transform times inverse-bind, any finite affine
// 3x4), yielding the weighted centres and the centred cross-covariance of
// the rest neighbourhood and its linear-blend-skinned image. A locally
// optimal rigid transform (R_i, t_i) with det(R_i) = +1 is fitted per
// vertex and applied to the original rest attributes, so bind pose and
// uniform rigid motion are reproduced exactly. No diffusion system is
// solved and no neighbourhood is traversed at evaluation time.
//
// Degeneracy rule: if the smallest singular value of the rest covariance
// (at bind) or of the current-frame cross-covariance (at evaluate) is not
// greater than 1e-9 times the largest one - including the all-zero case -
// that vertex falls back to plain CPU linear blend skinning (positions via
// the blended matrix, directions via the adjoint-transpose rule). A
// degenerate vertex never affects any other vertex.
//
// All objects own their data: the binding copies everything it needs at
// bind time, and evaluation results belong to the output object. Any
// failure (invalid indices/weights/parameters, non-finite data, failed
// precomputation) is reported by a null/false return and leaves existing
// bindings and outputs untouched.

#include <cmath>
#include <cstddef>
#include <vector>

#include "util.h"
#include "geom.h"

struct DeltaMushBinding;

// Evaluation result. Each vector, when filled, holds 3*numverts floats.
struct DeltaMushOutput
{
    std::vector<float> position;
    std::vector<float> normal;
    std::vector<float> tangent;
    std::vector<float> bitangent;
};

// Builds a binding from rest-pose mesh data. positions/normals are 3 floats
// per vertex, tangents 4 (xyz + bitangent sign), triangles 3 vertex indices
// per triangle, blendindices/blendweights 4 bytes per vertex following the
// IQM convention (weights sum to 255, used influences first, zeros trail).
// lambda must be > 0 and power >= 1. Returns null on any invalid input or
// failed precomputation.
DeltaMushBinding *deltamush_bind(const float *positions, const float *normals, const float *tangents,
                                 const uint *triangles, int numtriangles,
                                 const uchar *blendindices, const uchar *blendweights,
                                 int numverts, int numjoints, double lambda, int power);
void deltamush_free(DeltaMushBinding *binding);

int deltamush_numverts(const DeltaMushBinding *binding);
int deltamush_numjoints(const DeltaMushBinding *binding);
// Number of stored nonzero moments Omega_ij.
size_t deltamush_momentcount(const DeltaMushBinding *binding);
// Number of vertices flagged degenerate at bind time (rest covariance).
int deltamush_degeneratecount(const DeltaMushBinding *binding);
// Inspection: writes row `row` of the diffusion matrix S (numverts entries).
bool deltamush_diffusionrow(const DeltaMushBinding *binding, int row, double *out);

// Evaluates the binding against a palette of numjoints finite affine 3x4
// matrices (model-space joint transform times inverse-bind). On success
// fills output and returns true; accumulations, if given, receives how many
// moment accumulations this evaluation performed (each stored nonzero
// moment is visited exactly once), and fallbacks receives how many vertices
// fell back to plain linear blend skinning this frame. On failure returns
// false and leaves output unchanged.
bool deltamush_evaluate(const DeltaMushBinding *binding, const Matrix3x4 *palette, DeltaMushOutput *output,
                        size_t *accumulations = 0, int *fallbacks = 0);

// Switch hooking the delta-mush path into animateiqm (implemented in
// demo.cpp). While enabled with a binding whose vertex count matches the
// loaded model, animateiqm fills outposition/outnormal/outtangent/
// outbitangent from deltamush_evaluate instead of the classic algorithm.
// The caller keeps owning the binding. Pass null or enabled=false to
// restore the classic path.
void deltamush_switch(DeltaMushBinding *binding, bool enabled);
bool deltamush_switched();

#endif
