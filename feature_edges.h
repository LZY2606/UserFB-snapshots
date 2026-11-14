#ifndef FEATURE_EDGES_H
#define FEATURE_EDGES_H
#include <vector>
#include "geometry.h"
#include "tgaimage.h"
#include "our_gl.h"

class Model;

enum FeatureEdgeKind {
    FEATURE_BOUNDARY   = 0, // exactly one valid adjacent face
    FEATURE_CREASE     = 1, // adjacent face normals differ by more than the threshold, or more than two valid neighbors
    FEATURE_SILHOUETTE = 2  // view dependent: the two adjacent faces do not face the viewer the same way
};

struct FeatureEdge {
    int a = -1;                 // original OBJ position index of the first endpoint
    int b = -1;                 // original OBJ position index of the second endpoint
    std::vector<int> faces = {}; // numbers of the valid adjacent faces
    int kind = FEATURE_BOUNDARY;
};

// One undirected edge keyed by the pair of OBJ position indices, together with
// every valid face that references it. Distinct position indices are kept apart.
struct EdgeAdjacency {
    int a = -1;
    int b = -1;
    std::vector<int> faces = {};
};

struct FeatureEdgeSet {
    std::vector<FeatureEdge> edges;        // boundary and crease edges, topology based
    std::vector<EdgeAdjacency> adjacency;  // every edge with its valid adjacent faces
    std::vector<vec3> face_normal;         // geometric normal of every face, zero for zero-area faces
    std::vector<char> valid_face;          // zero-area faces do not participate in the classification
};

// Build the position-index adjacency. UV and normal seams are ignored: only the
// OBJ position indices define an edge, distinct indices are never welded even
// when their coordinates coincide. Zero-area faces are excluded.
FeatureEdgeSet classify_feature_edges(const Model &model, double crease_angle_degrees);

// Add the view dependent silhouette edges to the static boundary/crease set.
// The geometric face normals and the current observation point decide the
// facing, never texture or interpolated normals.
std::vector<FeatureEdge> classify_view_edges(const FeatureEdgeSet &features,
                                             const Model &model, const vec3 &eye);

// Rasterize the feature lines on top of an already shaded framebuffer, reusing
// its depth buffer and per-fragment ownership. Only colors are written; the
// depth buffer and the ownership tags are untouched.
void overlay_feature_lines(const FeatureEdgeSet &features, const Model &model,
                           int object_id, const vec3 &eye, double width_pixels,
                           const TGAColor &color, TGAImage &framebuffer,
                           const FragmentOwnership &owner);

#endif // FEATURE_EDGES_H
