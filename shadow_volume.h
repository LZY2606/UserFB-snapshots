#pragma once
// Closed shadow volumes for hard directional-light shadows (z-fail algorithm).
//
// The occluder is a closed, outward-oriented, non-self-intersecting triangle
// 2-manifold. Faces are addressed by *position* indices, so UV/normal seams
// (duplicated vertices sharing the same position) do not look like cracks and
// coincident positions with distinct indices are never welded.
//
// Support range (see also shadow_frame_pipeline in our_gl.h):
//   * finite extrusion distance D chosen by the caller; the caller guarantees
//     that every receiver-to-occluder intersection along normalize(L) is closer
//     than D, so the far caps lie behind the receivers. No infinite-far cap.
//   * all generated vertices are assumed to satisfy z >= -w after projection;
//     the far plane therefore never cuts the volume and no far-plane capping is
//     performed. Near-plane and side-plane clipping are handled, including the
//     case where the camera is inside the volume (z-fail, never z-pass).
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "geometry.h"

class ShadowVolumeError : public std::runtime_error {
public:
    explicit ShadowVolumeError(const std::string &what) : std::runtime_error(what) {}
};

class ShadowVolume {
public:
    // Build the closed volume for one occluder.
    //   positions : finite world-space positions, indexed by faces
    //   faces     : triples of position indices
    //   L         : non-zero finite direction pointing TOWARD the light
    //   D         : positive finite extrusion distance along -normalize(L)
    // Throws ShadowVolumeError (leaving no half-built object) on out-of-range
    // indices, zero-area faces, open edges, consistent-winding shared edges, or
    // edges incident to more than two faces.
    ShadowVolume(const std::vector<vec3> &positions,
                 const std::vector<int> &faces,
                 const vec3 &L, double D);

    int nverts()  const { return static_cast<int>(verts.size()); }
    int ntris()   const { return static_cast<int>(tris.size()) / 3; }
    int nedges()  const { return static_cast<int>(outline.size()); }

    const std::vector<vec3>& vertices()      const { return verts; }
    const std::vector<int>&  triangles()     const { return tris; }       // size ntris()*3, outward oriented
    const std::vector<std::pair<int,int>>& outline_edges() const { return outline; }

    // True when the triangle's outward normal faces the light
    // (dot(n,L) > 0; an exactly zero dot product counts as facing away).
    static bool faces_light(const vec3 &n, const vec3 &L) { return n * L > 0.0; }

private:
    std::vector<vec3> verts;
    std::vector<int> tris;
    std::vector<std::pair<int,int>> outline;
};
