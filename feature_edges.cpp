#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <vector>
#include "feature_edges.h"
#include "model.h"

extern mat<4,4> ModelView, Viewport, Perspective; // transform matrices maintained by our_gl
extern std::vector<double> zbuffer;               // depth buffer maintained by our_gl

namespace {

constexpr double kAreaEps = 1e-20;
#ifndef M_PI
constexpr double M_PI = 3.14159265358979323846;
#endif

vec3 xyz(const vec4 &v) { return {v.x, v.y, v.z}; }

std::pair<int,int> edge_key(int a, int b) {
    return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
}

bool is_finite(const vec4 &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) &&
           std::isfinite(v.z) && std::isfinite(v.w);
}

// Project a world position through the whole pipeline.
bool project_vertex(const vec4 &world, vec2 &screen, double &depth) {
    vec4 clip = Perspective * (ModelView * world);
    if (!is_finite(clip) || !(clip.w > 0.)) return false; // only finite vertices with positive w
    vec4 ndc = clip / clip.w;
    screen = (Viewport * ndc).xy();
    depth  = ndc.z;
    return true;
}

bool faces_viewer(const vec3 &normal, const vec3 &point, const vec3 &eye) {
    return normal * (eye - point) > 0.;
}

} // namespace

FeatureEdgeSet classify_feature_edges(const Model &model, double crease_angle_degrees) {
    FeatureEdgeSet result;
    const int nfaces = model.nfaces();
    result.face_normal.assign(nfaces, {0., 0., 0.});
    result.valid_face.assign(nfaces, 0);

    // position-index key -> adjacency; UV and normal indices never appear here
    std::map<std::pair<int,int>, EdgeAdjacency> adjacency;

    for (int f = 0; f < nfaces; ++f) {
        const int idx[3] = { model.position_index(f, 0),
                             model.position_index(f, 1),
                             model.position_index(f, 2) };
        const vec3 p[3] = { xyz(model.vert(f, 0)),
                            xyz(model.vert(f, 1)),
                            xyz(model.vert(f, 2)) };
        vec3 normal = cross(p[1] - p[0], p[2] - p[0]);
        if (norm(normal) <= kAreaEps) continue; // zero-area faces do not participate
        result.valid_face[f]   = 1;
        result.face_normal[f]  = normalized(normal);

        const int corners[3][2] = { {idx[0], idx[1]}, {idx[1], idx[2]}, {idx[2], idx[0]} };
        for (auto &e : corners) {
            auto key = edge_key(e[0], e[1]);
            EdgeAdjacency &entry = adjacency[key];
            entry.a = key.first;
            entry.b = key.second;
            entry.faces.push_back(f);
        }
    }

    result.adjacency.reserve(adjacency.size());
    for (auto &kv : adjacency) {
        const EdgeAdjacency &entry = kv.second;
        result.adjacency.push_back(entry);
        FeatureEdge edge;
        edge.a = entry.a;
        edge.b = entry.b;
        edge.faces = entry.faces;
        if (entry.faces.size() == 1) {
            edge.kind = FEATURE_BOUNDARY;   // exactly one valid neighbor: boundary edge
            result.edges.push_back(edge);
        } else if (entry.faces.size() > 2) {
            edge.kind = FEATURE_CREASE;     // non-manifold edge: handled as a crease
            result.edges.push_back(edge);
        } else {
            const vec3 &n0 = result.face_normal[entry.faces[0]];
            const vec3 &n1 = result.face_normal[entry.faces[1]];
            double cos_angle = std::max(-1., std::min(1., n0 * n1));
            if (std::acos(cos_angle) * 180. / M_PI > crease_angle_degrees) {
                edge.kind = FEATURE_CREASE; // strictly greater opening than the threshold
                result.edges.push_back(edge);
            }
        }
    }
    return result;
}

std::vector<FeatureEdge> classify_view_edges(const FeatureEdgeSet &features,
                                             const Model &model, const vec3 &eye) {
    std::map<std::pair<int,int>, int> static_kinds;
    for (const FeatureEdge &e : features.edges)
        static_kinds[edge_key(e.a, e.b)] = e.kind;

    std::vector<FeatureEdge> view_edges;
    for (const EdgeAdjacency &entry : features.adjacency) {
        if (entry.faces.size() != 2) continue; // silhouette only makes sense for manifold edges
        if (static_kinds.count(edge_key(entry.a, entry.b))) continue; // already drawn as boundary/crease
        const int f0 = entry.faces[0], f1 = entry.faces[1];
        if (!features.valid_face[f0] || !features.valid_face[f1]) continue;
        const vec3 p0 = xyz(model.vert(f0, 0));
        const vec3 p1 = xyz(model.vert(f1, 0));
        const bool front0 = faces_viewer(features.face_normal[f0], p0, eye);
        const bool front1 = faces_viewer(features.face_normal[f1], p1, eye);
        if (front0 == front1) continue; // same facing: the edge is not on the silhouette
        FeatureEdge edge;
        edge.a = entry.a;
        edge.b = entry.b;
        edge.faces = entry.faces;
        edge.kind = FEATURE_SILHOUETTE;
        view_edges.push_back(edge);
    }
    return view_edges;
}

void overlay_feature_lines(const FeatureEdgeSet &features, const Model &model,
                           int object_id, const vec3 &eye, double width_pixels,
                           const TGAColor &color, TGAImage &framebuffer,
                           const FragmentOwnership &owner) {
    std::vector<FeatureEdge> draw = features.edges;
    const std::vector<FeatureEdge> view_edges = classify_view_edges(features, model, eye);
    draw.insert(draw.end(), view_edges.begin(), view_edges.end());

    const int fb_width  = framebuffer.width();
    const int fb_height = framebuffer.height();
    const bool ownership_valid =
        owner.object.size() == zbuffer.size() && owner.face.size() == zbuffer.size();
    const double half_width = width_pixels / 2.;

    for (const FeatureEdge &edge : draw) {
        vec2 s0, s1;
        double z0 = 0., z1 = 0.;
        if (!project_vertex(model.vert(edge.a), s0, z0)) continue;
        if (!project_vertex(model.vert(edge.b), s1, z1)) continue;

        const vec2 d = {s1.x - s0.x, s1.y - s0.y};
        const double len2 = d.x*d.x + d.y*d.y;

        int xmin = std::max<int>(0, std::floor(std::min(s0.x, s1.x) - half_width));
        int xmax = std::min<int>(fb_width  - 1, std::ceil(std::max(s0.x, s1.x) + half_width));
        int ymin = std::max<int>(0, std::floor(std::min(s0.y, s1.y) - half_width));
        int ymax = std::min<int>(fb_height - 1, std::ceil(std::max(s0.y, s1.y) + half_width));

#pragma omp parallel for
        for (int x = xmin; x <= xmax; ++x) {
            for (int y = ymin; y <= ymax; ++y) {
                const vec2 p = {static_cast<double>(x), static_cast<double>(y)};
                double t;
                double dist;
                if (len2 <= 0.) { // degenerate projection: the round cap around the single point
                    t = 0.;
                    const vec2 q = {p.x - s0.x, p.y - s0.y};
                    dist = std::sqrt(q.x*q.x + q.y*q.y);
                } else {
                    t = ((p.x - s0.x)*d.x + (p.y - s0.y)*d.y) / len2;
                    const double tc = std::max(0., std::min(1., t)); // nearest parameter on the segment, round caps
                    const vec2 q = {p.x - (s0.x + d.x*tc), p.y - (s0.y + d.y*tc)};
                    dist = std::sqrt(q.x*q.x + q.y*q.y);
                    t = tc;
                }
                if (dist > half_width) continue; // integer sample point farther than half the line width

                const double z = z0 + (z1 - z0) * t; // screen parameter interpolation of z/w
                const int idx = x + y*fb_width;
                if (z <= zbuffer[idx]) { // greater depth wins; equal depth blocks the line
                    bool owned_by_neighbor = false;
                    if (ownership_valid && owner.object[idx] == object_id) {
                        for (int f : edge.faces) {
                            if (owner.face[idx] == f) { owned_by_neighbor = true; break; }
                        }
                    }
                    if (!owned_by_neighbor) continue; // other objects / non-adjacent faces keep occluding
                }
                framebuffer.set(x, y, color); // color only: surface depth and ownership are preserved
            }
        }
    }
}
