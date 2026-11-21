#include <vector>
#include <cmath>

#include "vector.h"
#include "ray.h"
#include "triangle.h"
#include "kdtree.h"

// Build KD tree for tris
KDNode* KDNode::build(std::vector<Triangle*> &tris, int depth){
    KDNode* node = new KDNode();
    node->leaf = false;
    node->triangles = std::vector<Triangle*>();
    node->left = NULL;
    node->right = NULL;
    node->box = AABBox();

    if (tris.size() == 0) return node;

    if (depth > 25 || tris.size() <= 6) {
        node->triangles = tris;
        node->leaf = true;
        node->box = tris[0]->get_bounding_box();

        for (long i=1; i<tris.size(); i++) {
            node->box.expand(tris[i]->get_bounding_box());
        }

        node->left = new KDNode();
        node->right = new KDNode();
        node->left->triangles = std::vector<Triangle*>();
        node->right->triangles = std::vector<Triangle*>();

        return node;
    }

    node->box = tris[0]->get_bounding_box();
    Vec midpt = Vec();
    double tris_recp = 1.0/tris.size();

    for (long i=1; i<tris.size(); i++) {
        node->box.expand(tris[i]->get_bounding_box());
        midpt = midpt + (tris[i]->get_midpoint() * tris_recp);
    }

    std::vector<Triangle*> left_tris;
    std::vector<Triangle*> right_tris;
    int axis = node->box.get_longest_axis();

    for (long i=0; i<tris.size(); i++) {
        switch (axis) {
            case 0:
                midpt.x >= tris[i]->get_midpoint().x ? right_tris.push_back(tris[i]) : left_tris.push_back(tris[i]);
                break;
            case 1:
                midpt.y >= tris[i]->get_midpoint().y ? right_tris.push_back(tris[i]) : left_tris.push_back(tris[i]);
                break;
            case 2:
                midpt.z >= tris[i]->get_midpoint().z ? right_tris.push_back(tris[i]) : left_tris.push_back(tris[i]);
                break;
        }
    }

    if (tris.size() == left_tris.size() || tris.size() == right_tris.size()) {
        node->triangles = tris;
        node->leaf = true;
        node->box = tris[0]->get_bounding_box();

        for (long i=1; i<tris.size(); i++) {
            node->box.expand(tris[i]->get_bounding_box());
        }

        node->left = new KDNode();
        node->right = new KDNode();
        node->left->triangles = std::vector<Triangle*>();
        node->right->triangles = std::vector<Triangle*>();

        return node;
    }

    node->left = build(left_tris, depth+1);
    node->right = build(right_tris, depth+1);

    return node;
}

// Finds nearest triangle in kd tree that intersects with ray.
bool KDNode::hit(KDNode *node, const Ray &ray, double &t, double &tmin, Vec &normal, Vec &c) {
    double dist;
    if (node->box.intersection(ray, dist)){
        if (dist > tmin) return false;

        bool hit_tri = false;
        bool hit_left = false;
        bool hit_right = false;
        long tri_idx;

        if (!node->leaf) {
            //if ( node->left->triangles.size() > 0 )
                hit_left = hit(node->left, ray, t, tmin, normal, c);

            //if ( node->right->triangles.size() > 0 )
                hit_right = hit(node->right, ray, t, tmin, normal, c);

            return hit_left || hit_right;
        }
        else {
            long triangles_size = node->triangles.size();
            for (long i=0; i<triangles_size; i++) {
                if (node->triangles[i]->intersect(ray, t, tmin, normal)){
                    hit_tri = true;
                    tmin = t;
                    tri_idx = i;
                }
            }
            if (hit_tri) {
                Vec p = ray.origin + ray.direction * tmin;
                c = node->triangles[tri_idx]->get_colour_at(p);
                return true;
            }
        }
    }
    return false;
}

// Robust ray/AABB slab test that also handles ray components of zero
// (the original intersection() multiplies by infinite inverses). Fills the
// entry and exit parameters and reports whether the box is met at or after
// t_start.
static bool box_segment(const AABBox &box, const Ray &ray, double t_start,
                        double &t_enter, double &t_leave) {
    double lo = -INFINITY;
    double hi = INFINITY;
    const Vec bl = box.bl;
    const Vec tr = box.tr;
    const Vec o = ray.origin;
    const Vec d = ray.direction;
    double b[3] = {bl.x, bl.y, bl.z};
    double e[3] = {tr.x, tr.y, tr.z};
    double od[3] = {o.x, o.y, o.z};
    double dd[3] = {d.x, d.y, d.z};

    for (int a = 0; a < 3; ++a) {
        if (fabs(dd[a]) < 1e-300) {
            if (od[a] < b[a] || od[a] > e[a]) return false;
        } else {
            double t1 = (b[a] - od[a]) / dd[a];
            double t2 = (e[a] - od[a]) / dd[a];
            if (t1 > t2) { double tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > lo) lo = t1;
            if (t2 < hi) hi = t2;
            if (lo > hi) return false;
        }
    }
    if (hi < t_start) return false;
    t_enter = lo;
    t_leave = hi;
    return true;
}

Triangle* KDNode::nearest(KDNode *node, const Ray &ray, double t_start,
                          double &tmin, Vec &normal) {
    if (node == NULL) return NULL;

    double t_enter, t_leave;
    if (!box_segment(node->box, ray, t_start, t_enter, t_leave)) return NULL;
    if (t_enter > tmin) return NULL;

    Triangle *best = NULL;

    if (!node->leaf) {
        Triangle *l = nearest(node->left, ray, t_start, tmin, normal);
        if (l != NULL) best = l;
        Triangle *r = nearest(node->right, ray, t_start, tmin, normal);
        if (r != NULL) best = r;
        return best;
    }

    Vec norm;
    long triangles_size = (long)node->triangles.size();
    for (long i = 0; i < triangles_size; ++i) {
        double t;
        if (node->triangles[i]->hit_point(ray, t, norm) && t >= t_start && t < tmin) {
            tmin = t;
            normal = norm;
            best = node->triangles[i];
        }
    }
    return best;
}

// Largest value of n.p over the axis aligned box, used for conservative
// half-space rejection (a box is outside n.p >= d when even this is < d).
static double box_max_dot(const AABBox &box, const Vec &n) {
    const Vec bl = box.bl;
    const Vec tr = box.tr;
    double px = n.x >= 0.0 ? n.x * tr.x : n.x * bl.x;
    double py = n.y >= 0.0 ? n.y * tr.y : n.y * bl.y;
    double pz = n.z >= 0.0 ? n.z * tr.z : n.z * bl.z;
    return px + py + pz;
}

void KDNode::collect_beam(KDNode *node, const BeamFrustum &frustum,
                          std::vector<Triangle*> &candidates,
                          BeamTraversalStats &stats) {
    if (node == NULL) return;
    ++stats.nodes_visited;

    // Side planes: reject only if the whole box lies outside one half-space.
    for (size_t k = 0; k < frustum.sides.size(); ++k) {
        const Plane &pl = frustum.sides[k];
        if (!pl.valid) continue;
        if (box_max_dot(node->box, pl.n) < pl.d) return;
    }
    // Segment start plane: boxes entirely before the departure plane cannot
    // hold an accepted intersection. A box straddling the plane (e.g. the
    // camera sits inside it) is kept, as required.
    if (frustum.has_near && frustum.near.valid) {
        if (box_max_dot(node->box, frustum.near.n) < frustum.near.d) return;
    }

    if (node->leaf) {
        long n = (long)node->triangles.size();
        for (long i = 0; i < n; ++i) {
            candidates.push_back(node->triangles[i]);
            ++stats.candidate_faces;
        }
        return;
    }

    collect_beam(node->left, frustum, candidates, stats);
    collect_beam(node->right, frustum, candidates, stats);
}
