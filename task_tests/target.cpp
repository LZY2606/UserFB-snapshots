// Shadow-volume task tests. No main(): task_support/main.cpp drives the
// registry. Ground-truth occlusion is computed with the test's own ray tracer
// against the original occluder, never against the generated shadow volume.
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "../task_support/registry.hpp"
#include "geometry.h"
#include "our_gl.h"
#include "shadow_volume.h"
#include "model.h"

namespace {

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------

struct Mesh { std::vector<vec3> v; std::vector<int> f; };

// Axis-aligned box centered at c with half size h, outward winding.
Mesh make_box(const vec3 &c, double h) {
    Mesh m;
    const vec3 p[8] = {
        {c.x-h,c.y-h,c.z-h}, {c.x+h,c.y-h,c.z-h},
        {c.x+h,c.y+h,c.z-h}, {c.x-h,c.y+h,c.z-h},
        {c.x-h,c.y-h,c.z+h}, {c.x+h,c.y-h,c.z+h},
        {c.x+h,c.y+h,c.z+h}, {c.x-h,c.y+h,c.z+h},
    };
    for (const vec3 &q : p) m.v.push_back(q);
    const int faces[12][3] = {
        {0,3,2},{0,2,1},       // -z
        {4,5,6},{4,6,7},       // +z
        {0,1,5},{0,5,4},       // -y
        {3,7,6},{3,6,2},       // +y
        {0,4,7},{0,7,3},       // -x
        {1,2,6},{1,6,5},       // +x
    };
    for (auto &t : faces) m.f.insert(m.f.end(), {t[0], t[1], t[2]});
    return m;
}

// Open receiver grid at constant z, +z winding, two triangles.
Mesh make_plane(double half, double z) {
    Mesh m;
    m.v = {{-half,-half,z}, {half,-half,z}, {half,half,z}, {-half,half,z}};
    m.f = {0,1,2, 0,2,3};
    return m;
}

// Moller-Trumbore ray/triangle intersection, test-owned ground truth.
bool ray_triangle(const vec3 &o, const vec3 &d, const vec3 &a,
                  const vec3 &b, const vec3 &c, double &t) {
    const double eps = 1e-12;
    vec3 e1 = b - a, e2 = c - a;
    vec3 p = cross(d, e2);
    double det = e1 * p;
    if (std::fabs(det) < eps) return false;
    vec3 s = o - a;
    double u = (s * p) / det;
    if (u < 0.0 || u > 1.0) return false;
    vec3 q = cross(s, e1);
    double v = (d * q) / det;
    if (v < 0.0 || u + v > 1.0) return false;
    t = (e2 * q) / det;
    return t > 1e-9;
}

// True when a ray from p along dir hits any occluder triangle.
bool ray_blocked(const vec3 &p, const vec3 &dir, const std::vector<Mesh> &occ) {
    double t;
    for (const Mesh &m : occ)
        for (int fi = 0; fi < static_cast<int>(m.f.size()) / 3; ++fi)
            if (ray_triangle(p, dir, m.v[m.f[3*fi]], m.v[m.f[3*fi+1]],
                             m.v[m.f[3*fi+2]], t))
                return true;
    return false;
}

// Orthographic world->clip: ndc = ((x-cx)/sx, (y-cy)/sy, (z-cz)/sz), w = 1.
mat<4,4> make_ortho(double cx, double cy, double cz,
                    double sx, double sy, double sz) {
    return mat<4,4>{{{1/sx,0,0,-cx/sx},
                     {0,1/sy,0,-cy/sy},
                     {0,0,1/sz,-cz/sz},
                     {0,0,0,1}}};
}

// Perspective world->clip for an eye on the +z axis looking toward -z.
// Eye-space depth ze = eye.z - z; near/far eye depths map to ndc z +/-1.
mat<4,4> make_perspective(double eyez, double near, double far) {
    double B = 2.0 / (1.0/near - 1.0/far);
    double A = 1.0 - B/near;
    return mat<4,4>{{{1,0,0,0},
                     {0,1,0,0},
                     {0,0,-A, A*eyez+B},
                     {0,0,-1,eyez}}};
}

mat<4,4> screen_viewport(int w, int h) {
    return mat<4,4>{{{w/2.,0,0,w/2.}, {0,h/2.,0,h/2.}, {0,0,1,0}, {0,0,0,1}}};
}

vec2 project(const mat<4,4> &clip_m, const mat<4,4> &vp, const vec3 &p) {
    vec4 c = clip_m * vec4{p.x,p.y,p.z,1};
    vec4 ndc = c / c.w;
    return (vp * ndc).xy();
}

// Every undirected edge of a closed outward triangle soup appears exactly
// twice, once in each direction; every triangle normal points outward.
void check_closed(Case &t, const ShadowVolume &vol, const vec3 &center) {
    std::map<std::pair<int,int>, int> dirs;
    const auto &tr = vol.triangles();
    for (int i = 0; i < vol.ntris(); ++i) {
        int a = tr[3*i], b = tr[3*i+1], c = tr[3*i+2];
        int e[3][2] = {{a,b},{b,c},{c,a}};
        for (auto &ed : e) {
            auto key = ed[0] < ed[1] ? std::make_pair(ed[0],ed[1])
                                     : std::make_pair(ed[1],ed[0]);
            dirs[key] += (ed[0] == key.first && ed[1] == key.second) ? 1 : -1;
        }
        vec3 pa = vol.vertices()[a], pb = vol.vertices()[b], pc = vol.vertices()[c];
        vec3 n = cross(pb-pa, pc-pa);
        vec3 mid = (pa+pb+pc)/3.0 - center;
        t.check(n*mid > 0.0, "volume triangle faces outward");
    }
    t.check(!dirs.empty(), "volume has edges");
    for (auto &kv : dirs)
        t.check(kv.second == 0, "every volume edge is shared once in each direction");
}

// ---------------------------------------------------------------------------
// Test surface shader
// ---------------------------------------------------------------------------

struct SolidSurfaceShader : ISurfaceShader {
    const Mesh &mesh;
    const mat<4,4> &clip_m;
    vec3 color{200,100,50};
    double ambient = .4, direct = .6;
    std::vector<char> discard_face;
    int face = -1;

    SolidSurfaceShader(const Mesh &m, const mat<4,4> &mv) : mesh(m), clip_m(mv) {
        discard_face.assign(m.f.size()/3, 0);
    }
    SurfaceVaryings vertex(int f, int k, vec4 &clip) override {
        face = f;
        vec3 p = mesh.v[mesh.f[3*f+k]];
        clip = clip_m * vec4{p.x,p.y,p.z,1};
        SurfaceVaryings v{};
        v.view = clip;
        v.nrm = vec4{0,0,1,0};
        return v;
    }
    SurfaceFragment fragment(const SurfaceVaryings [3], const vec3 &) const override {
        SurfaceFragment s;
        s.discard = discard_face[face] != 0;
        s.albedo = color;
        s.ambient = ambient;
        s.direct = direct;
        return s;
    }
};

constexpr int W = 64, H = 64;
const vec3 LIGHT{0,0,1};

// ---------------------------------------------------------------------------
// topology group
// ---------------------------------------------------------------------------

void topo_basic_box(Case &t) {
    Mesh box = make_box({0,0,2}, 1.0);
    ShadowVolume vol(box.v, box.f, LIGHT, 5.0);
    t.check(vol.nverts() == 16, "one near and one far vertex per position");
    t.check(vol.nedges() == 4, "box lit cap has four silhouette edges");
    // 2 lit near tris + 2 reversed far tris + 4 walls * 2 tris
    t.check(vol.ntris() == 12, "cap and wall triangle count");
    check_closed(t, vol, {0,0,2});
    const auto &vv = vol.vertices();
    vec3 d = vv[8] - vv[0];
    t.check(std::fabs(d.z + 5.0) < 1e-9 && std::fabs(d.x) < 1e-9 && std::fabs(d.y) < 1e-9,
            "extrusion follows -normalize(L)*D");
}

void topo_seamed_box(Case &t) {
    // Two coincident closed shells with separate index ranges: every position
    // of the second shell equals one of the first but carries a different
    // index. Coordinate welding would give every edge four incident faces (an
    // error); index-based adjacency keeps both shells closed and unwelded.
    Mesh a = make_box({0,0,2}, 1.0);
    Mesh both;
    both.v = a.v;
    int dup = static_cast<int>(both.v.size());
    for (const vec3 &q : a.v) both.v.push_back(q); // same coordinates, new indices
    for (int fi = 0; fi < 12; ++fi) {
        both.f.insert(both.f.end(), {a.f[3*fi], a.f[3*fi+1], a.f[3*fi+2]});
        both.f.insert(both.f.end(),
                      {dup+a.f[3*fi], dup+a.f[3*fi+1], dup+a.f[3*fi+2]});
    }
    t.check(both.v.size() == 16 && both.v[0].x == both.v[dup].x,
            "two distinct indices share one coordinate");
    ShadowVolume vol(both.v, both.f, LIGHT, 5.0);
    t.check(vol.ntris() == 24, "two unwelded closed shells each cap and wall");
    t.check(vol.nedges() == 8, "each shell contributes its own four silhouette edges");
    check_closed(t, vol, {0,0,2});
}

void topo_model_seam(Case &t) {
    // Real Model path: an OBJ with 8 shared position entries but per-corner UV
    // entries (UV seams). Building adjacency from face corners would see
    // cracks; using the read-only position_index() sees a closed manifold.
    const char *tmp = std::getenv("TMPDIR");
    std::string dir = tmp ? tmp : "/tmp";
    std::string path = dir + "/task_seam_box.obj";
    Mesh solid = make_box({0,0,2}, 1.0);
    {
        std::ofstream o(path);
        for (const vec3 &q : solid.v)
            o << "v " << q.x << ' ' << q.y << ' ' << q.z << '\n';
        o << "vn 0 0 1\n";
        int nvt = 0;
        for (int fi = 0; fi < 12; ++fi) {
            int uv[3];
            for (int k = 0; k < 3; ++k) {
                double u = (fi*7 + k*3) % 5 / 4.0;     // arbitrary split UVs
                double vv = (fi*3 + k*5) % 4 / 3.0;
                o << "vt " << u << ' ' << vv << '\n';
                uv[k] = nvt++;
            }
            o << "f " << solid.f[3*fi]+1   << '/' << uv[0]+1 << "/1 "
                      << solid.f[3*fi+1]+1 << '/' << uv[1]+1 << "/1 "
                      << solid.f[3*fi+2]+1 << '/' << uv[2]+1 << "/1\n";
        }
        t.check(nvt == 36, "obj emits 36 split UV entries over 8 positions");
    }
    Model m(path);
    t.check(m.nfaces() == 12 && m.nverts() == 8, "seamed obj keeps shared positions");
    std::vector<vec3> pos(m.nverts());
    std::vector<int> faces;
    for (int i = 0; i < m.nverts(); ++i) pos[i] = m.vert(i).xyz();
    for (int f = 0; f < m.nfaces(); ++f)
        for (int k = 0; k < 3; ++k) {
            int idx = m.position_index(f, k);
            t.check(idx >= 0 && idx < m.nverts(), "position index in range");
            faces.push_back(idx);
        }
    bool caught = false;
    try { ShadowVolume v(pos, faces, LIGHT, 4.0); }
    catch (const ShadowVolumeError &e) { caught = true; (void)e; }
    t.check(!caught, "UV seams are not geometric cracks under position adjacency");
}

void topo_errors(Case &t) {
    Mesh box = make_box({0,0,0}, 1.0);
    auto expect = [&](std::vector<int> faces, const char *needle, const char *label) {
        bool caught = false;
        try { ShadowVolume v(box.v, faces, LIGHT, 3.0); }
        catch (const ShadowVolumeError &e) {
            caught = true;
            t.check(std::string(e.what()).find(needle) != std::string::npos, label);
        }
        t.check(caught, label);
    };
    {
        std::vector<int> bad = box.f; bad[0] = 99;
        expect(bad, "face 0", "out-of-range index names the face");
    }
    {
        std::vector<int> bad = box.f; bad[3*5+1] = bad[3*5];
        expect(bad, "face 5", "zero-area face names the face");
    }
    {
        std::vector<int> bad(box.f.begin(), box.f.end() - 3);
        expect(bad, "positions", "open edge names the position pair");
    }
    {
        std::vector<int> bad = box.f;
        std::swap(bad[0], bad[1]);
        expect(bad, "winding", "same-direction shared edge is rejected");
    }
    {
        std::vector<int> bad = box.f;
        bad.insert(bad.end(), {box.f[0], box.f[1], box.f[2]});
        expect(bad, "faces", "edge used by three faces is rejected");
    }
    {
        bool caught = false;
        try { ShadowVolume v(box.v, box.f, vec3{0,0,0}, 3.0); }
        catch (const ShadowVolumeError &) { caught = true; }
        t.check(caught, "zero light direction rejected");
        caught = false;
        try { ShadowVolume v(box.v, box.f, LIGHT, 0.0); }
        catch (const ShadowVolumeError &) { caught = true; }
        t.check(caught, "non-positive extrusion rejected");
    }
}

void topo_grazing_face(Case &t) {
    // Side faces have dot(n, L) == 0: they count as facing away, so only the
    // +z cap is lit and the silhouette is the cap's four edges.
    Mesh box = make_box({0,0,2}, 1.0);
    ShadowVolume vol(box.v, box.f, LIGHT, 4.0);
    t.check(vol.ntris() == 12 && vol.nedges() == 4, "grazing faces are dark");
}

// ---------------------------------------------------------------------------
// zfail group
// ---------------------------------------------------------------------------

struct Scene {
    int w = W, h = H;
    mat<4,4> vp;
    mat<4,4> clip_m;
    double cx, cy, cz, sx, sy, sz;
    vec3 l = LIGHT;

    Scene(double cx_, double cy_, double cz_, double sx_, double sy_, double sz_)
        : vp(screen_viewport(W,H)), clip_m(make_ortho(cx_,cy_,cz_,sx_,sy_,sz_)),
          cx(cx_), cy(cy_), cz(cz_), sx(sx_), sy(sy_), sz(sz_) {}

    void pixel_of(double x, double y, double z, int &px, int &py) const {
        vec2 s = project(clip_m, vp, {x,y,z});
        px = static_cast<int>(std::floor(s.x));
        py = static_cast<int>(std::floor(s.y));
    }
    vec3 world_of(int px, int py, double z) const {
        double ndcx = (px + 0.5 - w/2.0) / (w/2.0);
        double ndcy = (py + 0.5 - h/2.0) / (h/2.0);
        return {cx + ndcx*sx, cy + ndcy*sy, z};
    }
};

// Full-pixel cross check: stencil non-zero iff the test's own ray from the
// receiver point along L intersects an original occluder. Points close to a
// silhouette boundary are skipped: the spec asks for points well away from
// the outline, where fill-convention edge ownership cannot matter.
void check_pixels_against_rays(Case &t, const Scene &sc, const ShadowRenderer &r,
                               const std::vector<Mesh> &occ, double rz,
                               double margin = 0.6) {
    const auto &st = r.stencil_buffer();
    const auto &valid = r.valid_buffer();
    int checked = 0;
    for (int py = 0; py < sc.h; ++py) {
        for (int px = 0; px < sc.w; ++px) {
            int idx = px + py*sc.w;
            if (!valid[idx]) continue;
            vec3 p = sc.world_of(px, py, rz);
            bool near_outline = false;
            for (const Mesh &m : occ) {
                double xmin=1e30,xmax=-1e30,ymin=1e30,ymax=-1e30;
                for (const vec3 &q : m.v) {
                    xmin=std::min(xmin,q.x); xmax=std::max(xmax,q.x);
                    ymin=std::min(ymin,q.y); ymax=std::max(ymax,q.y);
                }
                double dx = std::max({xmin-p.x, 0.0, p.x-xmax});
                double dy = std::max({ymin-p.y, 0.0, p.y-ymax});
                if (std::hypot(dx,dy) < margin) { near_outline = true; break; }
            }
            if (near_outline) continue;
            bool blocked = ray_blocked(p, sc.l, occ);
            t.check((st[idx] != 0) == blocked, "stencil matches test ray trace");
            ++checked;
        }
    }
    t.check(checked > 100, "ray cross check covers many interior pixels");
}

void zfail_basic(Case &t) {
    Scene sc(0,0,1, 12,12,12);
    Mesh recv = make_plane(10, 0);
    Mesh box = make_box({0,0,2}, 1.0);
    ShadowRenderer r(W, H, sc.vp);
    SolidSurfaceShader sh(recv, sc.clip_m);
    r.draw_surface(2, sh);
    ShadowVolume vol(box.v, box.f, LIGHT, 5.0);
    r.draw_volume(vol, sc.clip_m);
    std::vector<Mesh> occ = {box};
    check_pixels_against_rays(t, sc, r, occ, 0.0);
    int px, py;
    sc.pixel_of(0,0,0, px,py);
    t.check(r.stencil_buffer()[px+py*W] == 1,
            "receiver outside the volume: only the back far cap fails -> +1");
    sc.pixel_of(5,5,0, px,py);
    t.check(r.stencil_buffer()[px+py*W] == 0, "lit receiver point has zero count");
}

void zfail_overlapping_volumes(Case &t) {
    Scene sc(0,0,1, 12,12,12);
    Mesh recv = make_plane(10, 0);
    Mesh b1 = make_box({-0.5,0,2}, 1.0);
    Mesh b2 = make_box({ 0.5,0,2}, 1.0);
    ShadowRenderer r(W, H, sc.vp);
    SolidSurfaceShader sh(recv, sc.clip_m);
    r.draw_surface(2, sh);
    ShadowVolume v1(b1.v, b1.f, LIGHT, 5.0);
    ShadowVolume v2(b2.v, b2.f, LIGHT, 5.0);
    r.draw_volume(v1, sc.clip_m);
    r.draw_volume(v2, sc.clip_m);
    int px, py;
    sc.pixel_of(0,0,0, px,py);
    t.check(r.stencil_buffer()[px+py*W] == 2,
            "overlapping volumes add signed counts, they are not XORed");
    std::vector<Mesh> occ = {b1, b2};
    check_pixels_against_rays(t, sc, r, occ, 0.0);
    r.clear_stencil();
    r.draw_volume(v1, sc.clip_m);
    t.check(r.stencil_buffer()[px+py*W] == 1, "stencil reset gives one volume's count");
}

void zfail_near_plane_caps_cut(Case &t) {
    // Near plane z=2 cuts through the box (spans 1..3); the far cap at z=-4
    // stays inside (far plane z=-6). z-fail must still hold.
    Scene sc(0,0,-2, 12,12,4);
    Mesh recv = make_plane(10, 0);
    Mesh box = make_box({0,0,2}, 1.0);
    ShadowRenderer r(W, H, sc.vp);
    SolidSurfaceShader sh(recv, sc.clip_m);
    r.draw_surface(2, sh);
    ShadowVolume vol(box.v, box.f, LIGHT, 5.0);
    r.draw_volume(vol, sc.clip_m);
    std::vector<Mesh> occ = {box};
    check_pixels_against_rays(t, sc, r, occ, 0.0);
    int px, py;
    sc.pixel_of(0,0,0, px,py);
    t.check(r.stencil_buffer()[px+py*W] != 0, "shadow survives near-plane cap clipping");
}

void zfail_camera_inside_perspective(Case &t) {
    // Eye at z=2 lies inside the occluder (box spans z 1..3); the lit cap and
    // wall pieces behind/around the eye are clipped, z-fail still holds.
    Mesh recv = make_plane(10, 0);
    Mesh box = make_box({0,0,2}, 1.0);
    mat<4,4> clip_m = make_perspective(2.0, 0.5, 20.0);
    mat<4,4> vp = screen_viewport(W, H);
    ShadowRenderer r(W, H, vp);
    SolidSurfaceShader sh(recv, clip_m);
    r.draw_surface(2, sh);
    ShadowVolume vol(box.v, box.f, LIGHT, 5.0);
    r.draw_volume(vol, clip_m);
    struct Pt { double x,y; bool blocked; };
    Pt pts[] = {{0,0,true}, {-1.2,1.2,false}, {1.2,-1.2,false}, {0.5,-0.5,true}};
    for (auto &p : pts) {
        vec2 s = project(clip_m, vp, {p.x,p.y,0});
        int px = static_cast<int>(std::floor(s.x));
        int py = static_cast<int>(std::floor(s.y));
        t.check(px >= 0 && px < W && py >= 0 && py < H, "sample point on screen");
        int idx = px + py*W;
        t.check(r.valid_buffer()[idx] == 1, "receiver visible");
        bool ray = ray_blocked({p.x,p.y,0}, LIGHT, {box});
        t.check(ray == p.blocked, "ray ground truth sanity");
        t.check((r.stencil_buffer()[idx] != 0) == p.blocked,
                "z-fail works with the camera inside the volume");
    }
}

void zfail_equal_depth_caps(Case &t) {
    // Receiver through the lit top cap (z=3): equal-depth fragments count, so
    // front and back caps cancel -> the occluder does not shadow itself.
    Mesh box = make_box({0,0,2}, 1.0);
    ShadowVolume vol(box.v, box.f, LIGHT, 5.0);
    auto render_at = [&](double rz) {
        Scene sc(0,0,-2, 12,12,6);
        Mesh recv = make_plane(10, rz);
        ShadowRenderer r(W, H, sc.vp);
        SolidSurfaceShader sh(recv, sc.clip_m);
        r.draw_surface(2, sh);
        r.draw_volume(vol, sc.clip_m);
        int px, py;
        sc.pixel_of(0,0,rz, px,py);
        return r.stencil_buffer()[px+py*W];
    };
    t.check(render_at(3.0) == 0, "equal-depth lit cap cancels the far cap");
    t.check(render_at(3.0001) == 0, "receiver in front of the occluder is lit");
    t.check(render_at(2.999) != 0, "receiver inside the occluder body is shadowed");
}

// ---------------------------------------------------------------------------
// clipping group
// ---------------------------------------------------------------------------

void clip_side_planes(Case &t) {
    // Narrow frustum: the box footprint straddles the side planes.
    Scene sc(0,0,1, 1.6,1.6,12);
    Mesh recv = make_plane(10, 0);
    Mesh box = make_box({0,0,2}, 1.0);
    ShadowRenderer r(W, H, sc.vp);
    SolidSurfaceShader sh(recv, sc.clip_m);
    r.draw_surface(2, sh);
    ShadowVolume vol(box.v, box.f, LIGHT, 5.0);
    r.draw_volume(vol, sc.clip_m);
    std::vector<Mesh> occ = {box};
    check_pixels_against_rays(t, sc, r, occ, 0.0);
}

void clip_eye_plane_perspective(Case &t) {
    // Lit cap at z=3 is behind the eye (z=1.5): volume geometry crosses w=0.
    Mesh recv = make_plane(10, 0);
    Mesh box = make_box({0,0,2}, 1.0);
    mat<4,4> clip_m = make_perspective(1.5, 0.2, 20.0);
    mat<4,4> vp = screen_viewport(W, H);
    ShadowRenderer r(W, H, vp);
    SolidSurfaceShader sh(recv, clip_m);
    r.draw_surface(2, sh);
    ShadowVolume vol(box.v, box.f, LIGHT, 5.0);
    r.draw_volume(vol, clip_m);
    for (double xy : {0.0, 0.6, -0.6}) {
        vec2 s = project(clip_m, vp, {xy,xy,0});
        int px = static_cast<int>(std::floor(s.x));
        int py = static_cast<int>(std::floor(s.y));
        int idx = px + py*W;
        bool blocked = ray_blocked({xy,xy,0}, LIGHT, {box});
        t.check((r.stencil_buffer()[idx] != 0) == blocked,
                "eye-plane clipped volume keeps correct z-fail counts");
    }
}

void clip_shared_edge_covered_once(Case &t) {
    // Heavy side clipping turns triangles into fans; inner fan edges must
    // cover each sample once, so per-pixel counts stay in {-1,0,1}.
    Scene sc(0,0,1, 1.6,12,12);
    Mesh recv = make_plane(10, 0);
    Mesh box = make_box({0,0,2}, 1.0);
    ShadowRenderer r(W, H, sc.vp);
    SolidSurfaceShader sh(recv, sc.clip_m);
    r.draw_surface(2, sh);
    ShadowVolume vol(box.v, box.f, LIGHT, 5.0);
    r.draw_volume(vol, sc.clip_m);
    for (int c : r.stencil_buffer())
        t.check(c >= -1 && c <= 1, "fan inner edges never double-count fragments");
    std::vector<Mesh> occ = {box};
    check_pixels_against_rays(t, sc, r, occ, 0.0);
}

void clip_receiver_six_plane(Case &t) {
    // Receiver crosses near and side planes; visibility survives clipping.
    Scene sc(0,0,0, 4,4,4);
    Mesh recv = make_plane(10, 0);
    ShadowRenderer r(W, H, sc.vp);
    SolidSurfaceShader sh(recv, sc.clip_m);
    r.draw_surface(2, sh);
    int covered = 0;
    for (unsigned char v : r.valid_buffer()) covered += v;
    t.check(covered == W*H, "clipped receiver still covers the whole screen");
}

// ---------------------------------------------------------------------------
// lighting group
// ---------------------------------------------------------------------------

void light_compose(Case &t) {
    Scene sc(0,0,1, 12,12,12);
    Mesh recv = make_plane(10, 0);
    Mesh box = make_box({0,0,2}, 1.0);
    ShadowRenderer r(W, H, sc.vp);
    SolidSurfaceShader sh(recv, sc.clip_m);
    sh.color = {200,100,50}; sh.ambient = .4; sh.direct = .6;
    r.draw_surface(2, sh);
    ShadowVolume vol(box.v, box.f, LIGHT, 5.0);
    r.draw_volume(vol, sc.clip_m);
    TGAColor black{{0,0,0,255},4};
    TGAImage img(W, H, TGAImage::RGB, black);
    TGAColor bg{{177,195,209,255},4};
    r.compose(img, bg);
    int sx, sy;
    sc.pixel_of(5,5,0, sx,sy);
    TGAColor lit = img.get(sx,sy);
    t.check(lit[0]==200 && lit[1]==100 && lit[2]==50, "lit pixel keeps full albedo");
    sc.pixel_of(0,0,0, sx,sy);
    TGAColor dark = img.get(sx,sy);
    t.check(dark[0]==80 && dark[1]==40 && dark[2]==20,
            "only the direct term is removed; ambient remains");
    t.check(std::fabs(r.ambient_buffer()[sx+sy*W] - .4) < 1e-12,
            "ambient stored unquantized");
    t.check(std::fabs(r.direct_buffer()[sx+sy*W] - .6) < 1e-12,
            "diffuse+specular stored unquantized");
}

void light_discard_surface(Case &t) {
    // The front receiver discards everywhere; the plane behind it must become
    // the visible surface and receive the shadow.
    Scene sc(0,0,1, 12,12,12);
    Mesh front = make_plane(10, 1.0);
    Mesh back  = make_plane(10, 0.0);
    Mesh box = make_box({0,0,2}, 1.0);
    ShadowRenderer r(W, H, sc.vp);
    SolidSurfaceShader shf(front, sc.clip_m);
    shf.discard_face.assign(2, 1);
    r.draw_surface(2, shf);
    SolidSurfaceShader shb(back, sc.clip_m);
    r.draw_surface(2, shb);
    int px, py;
    sc.pixel_of(0,0,0, px,py);
    int idx = px + py*W;
    t.check(r.valid_buffer()[idx] == 1, "discarded front reveals the back surface");
    double expected_ndc = (0.0 - sc.cz) / sc.sz;
    t.check(std::fabs(r.depth_buffer()[idx] - expected_ndc) < 1e-12,
            "recorded depth belongs to the post-discard visible surface");
    ShadowVolume vol(box.v, box.f, LIGHT, 5.0);
    r.draw_volume(vol, sc.clip_m);
    bool blocked = ray_blocked({0,0,0}, LIGHT, {box});
    t.check(blocked && r.stencil_buffer()[idx] != 0,
            "shadow is evaluated against the post-discard visible surface");
}

void light_background_and_clear(Case &t) {
    Scene sc(0,0,1, 12,12,12);
    Mesh small = make_plane(1.0, 0);
    ShadowRenderer r(W, H, sc.vp);
    SolidSurfaceShader sh(small, sc.clip_m);
    r.draw_surface(2, sh);
    int valid = 0;
    for (unsigned char v : r.valid_buffer()) valid += v;
    t.check(valid > 0 && valid < W*H, "part of the screen is background");
    TGAColor black{{0,0,0,255},4};
    TGAImage img(W, H, TGAImage::RGB, black);
    TGAColor bg{{177,195,209,255},4};
    r.compose(img, bg);
    t.check(img.get(0,0)[0]==177 && img.get(0,0)[1]==195 && img.get(0,0)[2]==209,
            "background pixels are not shaded or counted");
    r.clear();
    for (int c : r.stencil_buffer()) t.check(c == 0, "clear resets the signed count");
    int any_valid = 0;
    for (unsigned char v : r.valid_buffer()) any_valid += v;
    t.check(any_valid == 0, "clear resets validity and depth records");
}

void light_dim_mismatch_rejected(Case &t) {
    Scene sc(0,0,1, 12,12,12);
    Mesh recv = make_plane(10, 0);
    ShadowRenderer r(W, H, sc.vp);
    SolidSurfaceShader sh(recv, sc.clip_m);
    r.draw_surface(2, sh);
    TGAColor black{{0,0,0,255},4};
    TGAImage wrong(W, H/2, TGAImage::RGB, black);
    bool caught = false;
    TGAColor black2{{0,0,0,255},4};
    try { r.compose(wrong, black2); }
    catch (const std::runtime_error &) { caught = true; }
    t.check(caught, "framebuffer size mismatch is reported");
}

} // namespace

void register_task_tests(Registry &r) {
    r.add("topology", "basic_closed_box", topo_basic_box);
    r.add("topology", "uv_seamed_box", topo_seamed_box);
    r.add("topology", "model_position_index_seam", topo_model_seam);
    r.add("topology", "invalid_topology_messages", topo_errors);
    r.add("topology", "grazing_faces_are_dark", topo_grazing_face);

    r.add("zfail", "basic_ray_cross_check", zfail_basic);
    r.add("zfail", "overlapping_volumes_add", zfail_overlapping_volumes);
    r.add("zfail", "near_plane_cuts_caps", zfail_near_plane_caps_cut);
    r.add("zfail", "camera_inside_perspective", zfail_camera_inside_perspective);
    r.add("zfail", "equal_depth_cap_cancellation", zfail_equal_depth_caps);

    r.add("clipping", "side_plane_clipping", clip_side_planes);
    r.add("clipping", "eye_plane_perspective", clip_eye_plane_perspective);
    r.add("clipping", "fan_shared_edge_once", clip_shared_edge_covered_once);
    r.add("clipping", "receiver_six_plane_clip", clip_receiver_six_plane);

    r.add("lighting", "ambient_plus_visibility", light_compose);
    r.add("lighting", "discard_reveals_back_surface", light_discard_surface);
    r.add("lighting", "background_and_explicit_clear", light_background_and_clear);
    r.add("lighting", "framebuffer_size_mismatch", light_dim_mismatch_rejected);
}
