#ifndef KDTREE_H
#define KDTREE_H

#include <vector>

#include "vector.h"
#include "ray.h"
#include "triangle.h"

// Half space n.dot(p) >= d.
struct Plane {
    Vec n;
    double d;
    bool valid;

    Plane() : n(Vec()), d(0.0), valid(false) {}
    Plane(Vec n_, double d_, bool valid_=true) : n(n_), d(d_), valid(valid_) {}
};

// Conservative description of a convex beam segment. A planar mirror turns a
// pinhole beam into another pinhole beam: the reflected ray family is exactly
// the set of lines from the mirror image of the eye. Every straight edge of
// the convex screen region therefore sweeps a plane through that (possibly
// virtual) eye, so the beam is an intersection of inward facing half spaces.
// The optional near plane is the segment start plane (mirror departure plane
// for reflected segments); boxes and triangles before it are never accepted.
struct BeamFrustum {
    std::vector<Plane> sides;     // Inward facing side planes
    Plane near;                   // Segment start plane
    bool has_near;

    BeamFrustum() : has_near(false) {}
};

struct BeamTraversalStats {
    unsigned long long nodes_visited;
    unsigned long long candidate_faces; // Faces handed out by KD leaves
    unsigned long long clipped_faces;   // Faces run through exact clipping

    BeamTraversalStats()
        : nodes_visited(0), candidate_faces(0), clipped_faces(0) {}
};

class KDNode {
public:
    AABBox box;
    KDNode* left;
    KDNode* right;
    std::vector<Triangle*> triangles;
    bool leaf;

    KDNode(){};
    KDNode* build(std::vector<Triangle*> &tris, int depth);
    bool hit (KDNode* node, const Ray &ray, double &t, double &tmin, Vec &normal, Vec &c);

    // Nearest triangle hit at or beyond t_start. Returns NULL on a miss and
    // fills the hit distance and the geometric normal oriented against the
    // ray.
    Triangle* nearest(KDNode *node, const Ray &ray, double t_start,
                      double &tmin, Vec &normal);

    // Conservative beam traversal: every triangle whose leaf box may meet
    // the frustum on/after the near plane is appended. May over-collect,
    // never omits a contributing face. Builds no auxiliary structure and
    // never scans every triangle linearly.
    void collect_beam(KDNode *node, const BeamFrustum &frustum,
                      std::vector<Triangle*> &candidates,
                      BeamTraversalStats &stats);
};

#endif // KDTREE_H
