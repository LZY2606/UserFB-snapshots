#ifndef TRIMESH_ALGO_H
#define TRIMESH_ALGO_H
/*
Szymon Rusinkiewicz
Princeton University

TriMesh_algo.h
Various mesh-munging algorithms using TriMeshes
*/


#include "TriMesh.h"
#include "XForm.h"


namespace trimesh {

// Optimally re-triangulate a mesh by doing edge flips
extern void edgeflip(TriMesh *mesh);

// Flip the order of vertices in each face.  Turns the mesh inside out.
extern void faceflip(TriMesh *mesh);

// One iteration of umbrella-operator smoothing
extern void umbrella(TriMesh *mesh, float stepsize, bool tangent = false);

// Taubin lambda/mu mesh smoothing
extern void lmsmooth(TriMesh *mesh, int niters);

// Umbrella operator on the normals
extern void numbrella(TriMesh *mesh, float stepsize);

// Remove the indicated vertices from the TriMesh.
extern void remove_vertices(TriMesh *mesh, const ::std::vector<bool> &toremove);

// Remove vertices that aren't referenced by any face
extern void remove_unused_vertices(TriMesh *mesh);

// Remove faces as indicated by toremove.  Should probably be
// followed by a call to remove_unused_vertices()
extern void remove_faces(TriMesh *mesh, const ::std::vector<bool> &toremove);

// Remove long, skinny faces.  Should probably be followed by a
// call to remove_unused_vertices()
extern void remove_sliver_faces(TriMesh *mesh);

// Remap vertices according to the given table
extern void remap_verts(TriMesh *mesh, const ::std::vector<int> &remap_table);

// Reorder vertices in a mesh according to the order in which
// they are referenced by the tstrips or faces.
extern void reorder_verts(TriMesh *mesh);

// Perform one iteration of subdivision on a mesh.
enum SubdivScheme { SUBDIV_PLANAR,
	SUBDIV_LOOP, SUBDIV_LOOP_ORIG, SUBDIV_LOOP_NEW,
	SUBDIV_BUTTERFLY, SUBDIV_BUTTERFLY_MODIFIED };
extern void subdiv(TriMesh *mesh, SubdivScheme scheme = SUBDIV_LOOP);

// Smooth the mesh geometry
extern void smooth_mesh(TriMesh *themesh, float sigma);

// Bilateral smoothing
extern void bilateral_smooth_mesh(TriMesh *themesh, float sigma1, float sigma2);

// Diffuse an arbitrary per-vertex vector (or scalar) field
template <class T>
extern void diffuse_vector(TriMesh *themesh, ::std::vector<T> &field, float sigma);

// Diffuse the normals across the mesh
extern void diffuse_normals(TriMesh *themesh, float sigma);

// Diffuse the curvatures across the mesh
extern void diffuse_curv(TriMesh *themesh, float sigma);

// Diffuse the curvature derivatives across the mesh
extern void diffuse_dcurv(TriMesh *themesh, float sigma);

// Given a curvature tensor, find principal directions and curvatures
extern void diagonalize_curv(const vec &old_u, const vec &old_v,
                             float ku, float kuv, float kv,
                             const vec &new_norm,
                             vec &pdir1, vec &pdir2, float &k1, float &k2);

// Reproject a curvature tensor from the basis spanned by old_u and old_v
// (which are assumed to be unit-length and perpendicular) to the
// new_u, new_v basis.
extern void proj_curv(const vec &old_u, const vec &old_v,
                      float old_ku, float old_kuv, float old_kv,
                      const vec &new_u, const vec &new_v,
                      float &new_ku, float &new_kuv, float &new_kv);

// Like the above, but for dcurv
extern void proj_dcurv(const vec &old_u, const vec &old_v,
                       const Vec<4> old_dcurv,
                       const vec &new_u, const vec &new_v,
                       Vec<4> &new_dcurv);

// Create an offset surface from a mesh
extern void inflate(TriMesh *mesh, float amount);

// Transform the mesh by the given matrix
extern void apply_xform(TriMesh *mesh, const xform &xf);

// Translate the mesh
extern void trans(TriMesh *mesh, const vec &transvec);

// Rotate the mesh by r radians
extern void rot(TriMesh *mesh, float r, const vec &axis);

// Scale the mesh - isotropic
extern void scale(TriMesh *mesh, float s);

// Scale the mesh - anisotropic in X, Y, Z
extern void scale(TriMesh *mesh, float sx, float sy, float sz);

// Scale the mesh - anisotropic in an arbitrary direction
extern void scale(TriMesh *mesh, float s, const vec &d);

// Clip mesh to the given bounding box
extern void clip(TriMesh *mesh, const box &b);

// Find center of mass of a bunch of points
extern point point_center_of_mass(const ::std::vector<point> &pts);

// Find (area-weighted) center of mass of a mesh
extern point mesh_center_of_mass(TriMesh *mesh);

// Compute covariance of a bunch of points
extern void point_covariance(const ::std::vector<point> &pts, float (&C)[3][3]);

// Compute covariance of faces (area-weighted) in a mesh
extern void mesh_covariance(TriMesh *mesh, float (&C)[3][3]);

// Scale the mesh so that mean squared distance from center of mass is 1
extern void normalize_variance(TriMesh *mesh);

// Rotate model so that first principal axis is along +X (using
// forward weighting), and the second is along +Y
extern void pca_rotate(TriMesh *mesh);

// As above, but only rotate by 90/180/etc. degrees w.r.t. original
extern void pca_snap(TriMesh *mesh);

// Flip faces so that orientation among touching faces is consistent
extern void orient(TriMesh *mesh);

// Remove boundary vertices (and faces that touch them)
extern void erode(TriMesh *mesh);

// Add a bit of noise to the mesh
extern void noisify(TriMesh *mesh, float amount);

// Find connected components.
// Considers components to be connected if they touch at a vertex if
//  conn_vert == true, else they need to touch at an edge.
// Outputs:
//  comps is a vector that gives a mapping from each face to its
//   associated connected component.
//  compsizes holds the size of each connected component.
// Connected components are sorted from largest to smallest.
extern void find_comps(TriMesh *mesh, ::std::vector<int> &comps,
	::std::vector<int> &compsizes, bool conn_vert = false);

// Select a particular connected component, and delete all other vertices from
// the mesh.
extern void select_comp(TriMesh *mesh, const ::std::vector<int> &comps,
	int whichcc);

// Select the connected components no smaller than min_size (but no more than
// total_largest components), and delete all other vertices from the mesh.
extern void select_big_comps(TriMesh *mesh, const ::std::vector<int> &comps,
	const ::std::vector<int> &compsizes, int min_size,
	int total_largest = ::std::numeric_limits<int>::max());

// Select the connected components no bigger than max_size (but no more than
// total_smallest components), and delete all other vertices from the mesh.
extern void select_small_comps(TriMesh *mesh, const ::std::vector<int> &comps,
	const ::std::vector<int> &compsizes, int max_size,
	int total_smallest = ::std::numeric_limits<int>::max());

// Find overlap area and RMS distance between mesh1 and mesh2.
// rmsdist is unchanged if area returned as zero
extern void find_overlap(TriMesh *mesh1, TriMesh *mesh2,
	float &area, float &rmsdist);

extern void find_overlap(TriMesh *mesh1, TriMesh *mesh2,
	const xform &xf1, const xform &xf2,
	float &area, float &rmsdist);

class KDtree;
extern void find_overlap(TriMesh *mesh1, TriMesh *mesh2,
	const xform &xf1, const xform &xf2,
	const KDtree *kd1, const KDtree *kd2,
	float &area, float &rmsdist);

// Return intersection over union between mesh1 and mesh2
extern float iou(TriMesh *mesh1, TriMesh *mesh2);

extern float iou(TriMesh *mesh1, TriMesh *mesh2,
	const xform &xf1, const xform &xf2);

extern float iou(TriMesh *mesh1, TriMesh *mesh2,
	const xform &xf1, const xform &xf2,
	const KDtree *kd1, const KDtree *kd2);

// Find separate mesh vertices that should be "shared": they lie on separate
// connected components, but they are within "tol" of each other.
extern void shared(TriMesh *mesh, float tol);

// Join multiple meshes together, possibly sharing vertices within tol.
// If tol < 0, don't share vertices.
extern TriMesh *join(const ::std::vector<TriMesh *> &meshes, float tol = -1.0f);

// Result of stitch_boundary_loops():
//  old_to_new has one entry per input vertex and gives the compact index
//  of that vertex in the stitched mesh.  Coincident seam vertices from the
//  two loops share one output vertex; all other vertices keep their own.
//  face_source has one entry per output face and gives the index of the
//  original face that each (sub-)triangle came from.
struct SeamStitchResult {
	::std::vector<int> old_to_new;
	::std::vector<int> face_source;
};

// Stitch two complete boundary loops of the same mesh that trace the same
// closed spatial polyline in opposite directions, building a common
// subdivision so that every seam segment becomes one real indexed edge
// shared by exactly two oppositely-oriented faces.  Unlike join()/shared(),
// long edges are split at the other side's sample points rather than only
// welding coincident corners.
//
// Geometry scope:
//  - first and second hold vertex indices of two complete boundary loops,
//    with no repeated first point, at least 3 points each, no index shared
//    between the loops, and each ordered along the boundary direction
//    induced by its incident faces (so the two loops run in opposite
//    directions in space); their start points may differ.
//  - The valid geometry is one closed, non-self-intersecting,
//    non-backtracking piecewise-linear loop.  Either side may insert extra
//    samples anywhere along a segment; every turn has a vertex on both
//    sides.  No curve fitting or approximate registration is performed.
//  - Coordinates are finite.  L is the bounding-box diagonal; coincidence
//    and collinearity use a distance tolerance of 1e-6*L.  Distinct events
//    and non-adjacent segments are separated by at least 1e-3*L.
//
// Mapping rules:
//  - Only the two designated loops are welded; coincident vertices outside
//    of them are left alone, and connectivity away from the seam is
//    unchanged.  No new interior points are introduced: only original
//    vertices and sample positions already on the seam are used.
//  - A coincident seam point takes the position of the lowest-numbered
//    contributing original vertex; all other vertex positions are
//    unchanged.  Output vertices are compacted, ordered by each one's
//    lowest contributing source index.
//  - Triangles whose edges are split are re-tessellated into sub-triangles
//    that cover the original triangle exactly without overlap, inherit its
//    winding, and have nonzero area; unaffected faces are retained once.
//
// Attribute rules (colors, confidences, flags each stay empty or keep the
// length of the vertex set):
//  - Continuous attributes at a seam point are linearly interpolated along
//    each side's original edge by edge length (an exact vertex hit on a
//    side takes that vertex's value directly), then the two side values
//    are averaged with equal weight.
//  - flags combine by bitwise OR of the contributing original endpoints:
//    a side that hits an original vertex contributes only that vertex's
//    flags; an interior hit contributes the OR of its edge endpoints.
//  - Non-seam vertices keep their attributes through the remap.
//
// On success the derived caches (normals, curvatures, third-order fields,
// point areas, adjacency, triangle strips, grid, bounding box and sphere)
// are invalidated, while flag_curr and user attributes are retained.
// Returns false (leaving mesh and result untouched, without filling any
// lazy cache) on NULL mesh, out-of-range or duplicated loop points, loops
// that are not complete boundary loops, same-direction loops, geometry
// that does not coincide, illegal attribute lengths, or a result that
// would contain duplicate or non-manifold faces.
extern bool stitch_boundary_loops(TriMesh *mesh,
	const ::std::vector<int> &first, const ::std::vector<int> &second,
	SeamStitchResult &result);

// Tessellated square (-1..1, -1..1, 0)
extern TriMesh *make_plane(int tess_x, int tess_y = -1);

// Gaussian bump of height 1 and width sigma
extern TriMesh *make_bump(int tess, float sigma = 1.0f);

// Sine wave of angular frequency omega
extern TriMesh *make_wave(int tess, float omega = M_PIf);

// Fractal landscape
extern TriMesh *make_frac(int tess);

// Tessellated cube
extern TriMesh *make_cube(int tess);

// Disc with the given tessellation in angle and radius
extern TriMesh *make_disc(int tess_th, int tess_r);

// Open cylinder of height 1 and given radius
extern TriMesh *make_cyl(int tess_th, int tess_h, float r = 1.0f);

// Cylinder capped with discs on both ends
extern TriMesh *make_ccyl(int tess_th, int tess_h, float r = 1.0f);

// Cylinder capped with hemispheres on both ends
extern TriMesh *make_scyl(int tess_th, int tess_h, float r = 1.0f);

// Open cone
extern TriMesh *make_cone(int tess_th, int tess_r, float r = 1.0f);

// Capped cone
extern TriMesh *make_ccone(int tess_th, int tess_r, float r = 1.0f);

// Torus with major radius 1, given minor radius
extern TriMesh *make_torus(int tess_th, int tess_ph, float r = 0.25f);

// Trefoil knot of the given minor radius
extern TriMesh *make_knot(int tess_th, int tess_ph, float r = 0.2f);

// Klein bottle
extern TriMesh *make_klein(int tess_th, int tess_ph);

// Helix of major radius 1, given minor radius, and number of turns
extern TriMesh *make_helix(int tess_th, int tess_ph, float turns, float r = 0.2f);

// Lat/long tessellated sphere
extern TriMesh *make_sphere_polar(int tess_ph, int tess_th);

// Sphere subdivided nsubdiv times from a Platonic solid of nfaces faces
extern TriMesh *make_sphere_subdiv(int nfaces, int nsubdiv);

enum FixedShape {
	// Platonic solids
	SHAPE_TETRAHEDRON, SHAPE_CUBE, SHAPE_OCTAHEDRON,
	SHAPE_DODECAHEDRON, SHAPE_ICOSAHEDRON,
	// Archimedean solids
	SHAPE_TRUNCATED_TETRAHEDRON, SHAPE_CUBOCTAHEDRON, SHAPE_TRUNCATED_CUBE,
	SHAPE_TRUNCATED_OCTAHEDRON, SHAPE_RHOMBICUBOCTAHEDRON,
	SHAPE_TRUNCATED_CUBOCTAHEDRON, SHAPE_ICOSIDODECAHEDRON,
	SHAPE_TRUNCATED_DODECAHEDRON, SHAPE_TRUNCATED_ICOSAHEDRON,
	SHAPE_SNUB_CUBE, SHAPE_RHOMBICOSIDODECAHEDRON,
	SHAPE_TRUNCATED_ICOSIDODECAHEDRON, SHAPE_SNUB_DODECAHEDRON,
	// Archimedean duals (Catalan solids)
	SHAPE_TRIAKIS_TETRAHEDRON, SHAPE_RHOMBIC_DODECAHEDRON,
	SHAPE_TRIAKIS_OCTAHEDRON, SHAPE_TETRAKIS_HEXAHEDRON,
	SHAPE_DELTOIDAL_ICOSITETRAHEDRON, SHAPE_DISDYAKIS_DODECAHEDRON,
	SHAPE_RHOMBIC_TRIACONTAHEDRON, SHAPE_TRIAKIS_ICOSAHEDRON,
	SHAPE_PENTAKIS_DODECAHEDRON, SHAPE_PENTAGONAL_ICOSITETRAHEDRON,
	SHAPE_DELTOIDAL_HEXECONTAHEDRON, SHAPE_DISDYAKIS_TRIACONTAHEDRON,
	SHAPE_PENTAGONAL_HEXECONTAHEDRON
};

// A pre-baked polyhedron (Platonic solid, etc.)
extern TriMesh *make_fixed_shape(FixedShape shape);

// Backwards compatibility - make a Platonic solid with the given
// number of faces
extern TriMesh *make_platonic(int nfaces);

// Surface of revolution obtained by rotating points on the given curve
// around the z axis.
extern TriMesh *make_surface_of_revolution(int tess_th,
	const ::std::vector<point> &curve_pts);

// Make a teapot.  Purists should pass in true for the last parameters.
// *Real* purists will be tempted to send me mail complaining about how
// the defaults are set, but are asked to refrain from doing so.
extern TriMesh *make_teapot(int tess, bool omit_bottom = false, bool taller = false);

} // namespace trimesh

#endif
