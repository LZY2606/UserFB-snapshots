#include <algorithm>
#include <cmath>
#include <map>
#include <sstream>
#include "shadow_volume.h"

namespace {

bool finite3(const vec3 &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

struct EdgeRef {
    int face;
    int a, b; // directed edge a -> b as stored in the face winding
};

} // namespace

ShadowVolume::ShadowVolume(const std::vector<vec3> &positions,
                           const std::vector<int> &faces,
                           const vec3 &L, double D) {
    if (positions.empty())
        throw ShadowVolumeError("shadow volume: occluder has no vertices");
    if (faces.size() % 3 != 0 || faces.empty())
        throw ShadowVolumeError("shadow volume: face index array must be a non-empty multiple of 3");
    if (!finite3(L) || (L.x == 0.0 && L.y == 0.0 && L.z == 0.0))
        throw ShadowVolumeError("shadow volume: light direction must be non-zero and finite");
    if (!std::isfinite(D) || D <= 0.0)
        throw ShadowVolumeError("shadow volume: extrusion distance must be positive and finite");
    for (const vec3 &p : positions)
        if (!finite3(p))
            throw ShadowVolumeError("shadow volume: occluder positions must be finite");

    const int nfaces = static_cast<int>(faces.size()) / 3;
    const int npos   = static_cast<int>(positions.size());

    for (int i = 0; i < static_cast<int>(faces.size()); ++i) {
        if (faces[i] < 0 || faces[i] >= npos) {
            std::ostringstream os;
            os << "shadow volume: face " << i / 3 << " corner " << i % 3
               << " has out-of-range position index " << faces[i];
            throw ShadowVolumeError(os.str());
        }
    }

    // Half-edge table keyed by the undirected position-index pair. Position
    // indices (never UV/normal indices) define adjacency, so seams do not
    // split edges and distinct coincident vertices are never welded.
    std::map<std::pair<int,int>, std::vector<EdgeRef>> edges;
    for (int f = 0; f < nfaces; ++f) {
        const int i0 = faces[3*f + 0];
        const int i1 = faces[3*f + 1];
        const int i2 = faces[3*f + 2];
        if (i0 == i1 || i1 == i2 || i2 == i0) {
            std::ostringstream os;
            os << "shadow volume: face " << f << " has repeated indices (zero area)";
            throw ShadowVolumeError(os.str());
        }
        const vec3 n = cross(positions[i1] - positions[i0], positions[i2] - positions[i0]);
        if (n * n == 0.0 || !std::isfinite(n.x)) {
            std::ostringstream os;
            os << "shadow volume: face " << f << " has zero area";
            throw ShadowVolumeError(os.str());
        }
        const int idx[3] = {i0, i1, i2};
        for (int k = 0; k < 3; ++k) {
            int a = idx[k], b = idx[(k + 1) % 3];
            auto key = a < b ? std::make_pair(a, b) : std::make_pair(b, a);
            edges[key].push_back({f, a, b});
        }
    }

    for (auto &kv : edges) {
        const auto &refs = kv.second;
        if (refs.size() == 1) {
            std::ostringstream os;
            os << "shadow volume: open boundary edge between positions "
               << kv.first.first << " and " << kv.first.second
               << " (only used by face " << refs[0].face << ")";
            throw ShadowVolumeError(os.str());
        }
        if (refs.size() > 2) {
            std::ostringstream os;
            os << "shadow volume: edge between positions " << kv.first.first
               << " and " << kv.first.second << " is used by " << refs.size()
               << " faces (manifold edges have exactly two)";
            throw ShadowVolumeError(os.str());
        }
        // A closed oriented manifold traverses every shared edge once in each
        // direction; same-direction references mean flipped/inconsistent winding.
        if (refs[0].a == refs[1].a && refs[0].b == refs[1].b) {
            std::ostringstream os;
            os << "shadow volume: edge between positions " << kv.first.first
               << " and " << kv.first.second << " shared by faces "
               << refs[0].face << " and " << refs[1].face
               << " with the same winding direction";
            throw ShadowVolumeError(os.str());
        }
    }

    const vec3 dir = normalized(L); // points toward the light
    const vec3 shift = dir * (-D); // extrusion goes away from the light

    // One near (original) and one far (extruded) vertex per position index.
    verts.resize(2 * npos);
    for (int i = 0; i < npos; ++i) {
        verts[i]        = positions[i];
        verts[npos + i] = positions[i] + shift;
    }

    std::vector<char> lit(static_cast<size_t>(nfaces), 0);
    for (int f = 0; f < nfaces; ++f) {
        const int i0 = faces[3*f + 0];
        const int i1 = faces[3*f + 1];
        const int i2 = faces[3*f + 2];
        const vec3 n = cross(positions[i1] - positions[i0], positions[i2] - positions[i0]);
        lit[f] = faces_light(n, L) ? 1 : 0;
    }

    // Near cap: lit triangles keep their outward winding.
    // Far cap: extruded copies of the lit triangles, reversed so their outward
    // normals point away from the light.
    for (int f = 0; f < nfaces; ++f) {
        if (!lit[f]) continue;
        const int i0 = faces[3*f + 0];
        const int i1 = faces[3*f + 1];
        const int i2 = faces[3*f + 2];
        tris.insert(tris.end(), {i0, i1, i2});
        tris.insert(tris.end(), {npos + i0, npos + i2, npos + i1});
    }

    // Side walls on silhouette edges: a lit face adjacent to a dark face.
    // Interior edges shared by two lit or two dark faces generate no wall; in
    // particular coplanar triangulation edges never become silhouettes.
    for (auto &kv : edges) {
        const auto &refs = kv.second;
        const EdgeRef &r0 = refs[0];
        const EdgeRef &r1 = refs[1];
        if (lit[r0.face] == lit[r1.face]) continue;
        const EdgeRef &lr = lit[r0.face] ? r0 : r1; // directed edge of the lit face
        const int a = lr.a, b = lr.b;
        outline.emplace_back(a, b);
        // The lit face lies to the left of its directed edge a -> b, so the
        // adjacent dark face lies to the right; that is the outward side of
        // the wall. With a' = a + shift, both triangles below have normal
        // cross(shift, b-a), pointing toward the dark side.
        const int ap = npos + a;
        const int bp = npos + b;
        tris.insert(tris.end(), {a, ap, bp});
        tris.insert(tris.end(), {a, bp, b});
    }

    if (tris.empty())
        throw ShadowVolumeError("shadow volume: no light-facing faces; cannot form a closed volume");
}
