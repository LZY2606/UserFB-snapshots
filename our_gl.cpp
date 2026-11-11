#include <algorithm>
#include <cmath>
#include "our_gl.h"

mat<4,4> ModelView, Viewport, Perspective; // "OpenGL" state matrices
std::vector<double> zbuffer;               // depth buffer

void lookat(const vec3 eye, const vec3 center, const vec3 up) {
    vec3 n = normalized(eye-center);
    vec3 l = normalized(cross(up,n));
    vec3 m = normalized(cross(n, l));
    ModelView = mat<4,4>{{{l.x,l.y,l.z,0}, {m.x,m.y,m.z,0}, {n.x,n.y,n.z,0}, {0,0,0,1}}} *
                mat<4,4>{{{1,0,0,-center.x}, {0,1,0,-center.y}, {0,0,1,-center.z}, {0,0,0,1}}};
}

void init_perspective(const double f) {
    Perspective = {{{1,0,0,0}, {0,1,0,0}, {0,0,1,0}, {0,0, -1/f,1}}};
}

void init_viewport(const int x, const int y, const int w, const int h) {
    Viewport = {{{w/2., 0, 0, x+w/2.}, {0, h/2., 0, y+h/2.}, {0,0,1,0}, {0,0,0,1}}};
}

void init_zbuffer(const int width, const int height) {
    zbuffer = std::vector(width*height, -1000.);
}

void rasterize(const Triangle &clip, const IShader &shader, TGAImage &framebuffer) {
    vec4 ndc[3]    = { clip[0]/clip[0].w, clip[1]/clip[1].w, clip[2]/clip[2].w };                // normalized device coordinates
    vec2 screen[3] = { (Viewport*ndc[0]).xy(), (Viewport*ndc[1]).xy(), (Viewport*ndc[2]).xy() }; // screen coordinates

    mat<3,3> ABC = {{ {screen[0].x, screen[0].y, 1.}, {screen[1].x, screen[1].y, 1.}, {screen[2].x, screen[2].y, 1.} }};
    if (ABC.det()<1) return; // backface culling + discarding triangles that cover less than a pixel

    auto [bbminx,bbmaxx] = std::minmax({screen[0].x, screen[1].x, screen[2].x}); // bounding box for the triangle
    auto [bbminy,bbmaxy] = std::minmax({screen[0].y, screen[1].y, screen[2].y}); // defined by its top left and bottom right corners
#pragma omp parallel for
    for (int x=std::max<int>(bbminx, 0); x<=std::min<int>(bbmaxx, framebuffer.width()-1); x++) {         // clip the bounding box by the screen
        for (int y=std::max<int>(bbminy, 0); y<=std::min<int>(bbmaxy, framebuffer.height()-1); y++) {
            vec3 bc_screen = ABC.invert_transpose() * vec3{static_cast<double>(x), static_cast<double>(y), 1.}; // barycentric coordinates of {x,y} w.r.t the triangle
            vec3 bc_clip   = { bc_screen.x/clip[0].w, bc_screen.y/clip[1].w, bc_screen.z/clip[2].w };     // check https://github.com/ssloy/tinyrenderer/wiki/Technical-difficulties-linear-interpolation-with-perspective-deformations
            bc_clip = bc_clip / (bc_clip.x + bc_clip.y + bc_clip.z);
            if (bc_screen.x<0 || bc_screen.y<0 || bc_screen.z<0) continue; // negative barycentric coordinate => the pixel is outside the triangle
            double z = bc_screen * vec3{ ndc[0].z, ndc[1].z, ndc[2].z };   // linear interpolation of the depth
            if (z <= zbuffer[x+y*framebuffer.width()]) continue;   // discard fragments that are too deep w.r.t the z-buffer
            auto [discard, color] = shader.fragment(bc_clip);
            if (discard) continue;                                 // fragment shader can discard current fragment
            zbuffer[x+y*framebuffer.width()] = z;                  // update the z-buffer
            framebuffer.set(x, y, color);                          // update the framebuffer
        }
    }
}

namespace {
struct ClipVertex {
    vec4 pos;    // homogeneous clip-space position
    vec3 lambda; // barycentric coordinates of the 3D (pre-projection) point w.r.t. the ORIGINAL triangle
};

double frustum_margin(const vec4 &v, const int plane) { // >= 0 means inside the frustum half-space
    switch (plane) {
        case 0: return  v.x + v.w; //  x >= -w
        case 1: return -v.x + v.w; //  x <=  w
        case 2: return  v.y + v.w; //  y >= -w
        case 3: return -v.y + v.w; //  y <=  w
        case 4: return  v.z + v.w; //  z >= -w
        default:return -v.z + v.w; //  z <=  w
    }
}

bool owns_boundary(const vec2 &a, const vec2 &b) { // half-open rule for the directed edge a->b:
    return (b.y<a.y) || (b.y==a.y && b.x<a.x);     // downward edges and leftward horizontal edges
}

bool same_point(const vec4 &a, const vec4 &b) {
    return a.x==b.x && a.y==b.y && a.z==b.z && a.w==b.w;
}
} // namespace

void rasterize_clipped(const Triangle &clip, const IShader &shader, TGAImage &framebuffer) {
    for (int i=0; i<3; i++) // non-finite input: no color, no depth, no fragment calls
        for (int c=0; c<4; c++)
            if (!std::isfinite(clip[i][c])) return;

    std::vector<ClipVertex> poly = { {clip[0], {1,0,0}}, {clip[1], {0,1,0}}, {clip[2], {0,0,1}} };
    for (int p=0; p<6 && poly.size()>2; p++) { // Sutherland-Hodgman against the six frustum planes
        std::vector<ClipVertex> clipped;
        for (size_t i=0; i<poly.size(); i++) {
            const ClipVertex &A = poly[i];
            const ClipVertex &B = poly[(i+1)%poly.size()];
            const double da = frustum_margin(A.pos, p), db = frustum_margin(B.pos, p);
            if (da>=0) clipped.push_back(A); // vertices exactly on the plane are kept
            if ((da>=0) != (db>=0)) {        // the edge crosses the plane: emit the intersection
                const double t = da/(da-db);
                // the eye-space point is linear in the clip-space parameter t (P^-1 is linear),
                // so the original-triangle barycentrics interpolate linearly, WITHOUT w weighting;
                // the perspective correction happens later, at rasterization time
                clipped.push_back({A.pos + t*(B.pos-A.pos), A.lambda + t*(B.lambda-A.lambda)});
            }
        }
        poly = clipped;
    }

    std::vector<ClipVertex> verts; // drop exact duplicate vertices produced on plane boundaries
    for (const ClipVertex &v : poly)
        if (verts.empty() || !same_point(v.pos, verts.back().pos)) verts.push_back(v);
    while (verts.size()>1 && same_point(verts.front().pos, verts.back().pos)) verts.pop_back();
    if (verts.size()<3) return; // fully clipped away or degenerate: nothing to draw

    const int W = framebuffer.width(), H = framebuffer.height();
    for (size_t k=1; k+1<verts.size(); k++) { // fan triangulation of the convex clipped polygon
        const ClipVertex *V[3] = { &verts[0], &verts[k], &verts[k+1] };
        vec2 s[3];
        vec3 lambda[3];
        double cw[3], ndcz[3];
        bool degenerate = false;
        for (int i=0; i<3; i++) {
            if (V[i]->pos.w<=0) degenerate = true; // only the zero vector reaches this, no 3D meaning
            vec4 ndc = V[i]->pos/V[i]->pos.w;
            s[i] = (Viewport*ndc).xy();
            if (!std::isfinite(s[i].x) || !std::isfinite(s[i].y) || !std::isfinite(ndc.z)) degenerate = true;
            cw[i] = V[i]->pos.w;
            ndcz[i] = ndc.z;
            lambda[i] = V[i]->lambda;
        }
        if (degenerate) continue;

        const double area2 = (s[1].x-s[0].x)*(s[2].y-s[0].y) - (s[1].y-s[0].y)*(s[2].x-s[0].x);
        if (!(area2>0)) continue; // backface or zero-area triangle: culled, only CCW front faces are drawn

        const double fx0 = std::max(0.,   std::floor(std::min({s[0].x,s[1].x,s[2].x})-.5)); // pixel centers
        const double fx1 = std::min(W-1., std::ceil (std::max({s[0].x,s[1].x,s[2].x})-.5)); // are sampled
        const double fy0 = std::max(0.,   std::floor(std::min({s[0].y,s[1].y,s[2].y})-.5));
        const double fy1 = std::min(H-1., std::ceil (std::max({s[0].y,s[1].y,s[2].y})-.5));
        for (int x=int(fx0); x<=int(fx1); x++) {
            for (int y=int(fy0); y<=int(fy1); y++) {
                const vec2 p{x+.5, y+.5};
                double E[3], beta[3];
                bool inside = true;
                for (int i=0; i<3; i++) { // edge function of the edge opposite vertex i
                    const vec2 &a = s[(i+1)%3], &b = s[(i+2)%3];
                    E[i] = (b.x-a.x)*(p.y-a.y) - (b.y-a.y)*(p.x-a.x);
                    const bool zero_edge = a.x==b.x && a.y==b.y; // coincident endpoints: no constraint
                    inside &= E[i]>0 || (E[i]==0 && (zero_edge || owns_boundary(a, b)));
                }
                if (!inside) continue;
                for (int i=0; i<3; i++) beta[i] = E[i]/area2; // screen-space barycentrics of the sub-triangle
                const double z = beta[0]*ndcz[0] + beta[1]*ndcz[1] + beta[2]*ndcz[2]; // z/w is linear in screen space
                if (z <= zbuffer[x+y*W]) continue; // larger z/w is closer
                const double c[3] = { beta[0]/cw[0], beta[1]/cw[1], beta[2]/cw[2] }; // perspective correction,
                const double sum = c[0]+c[1]+c[2];                                   // still w.r.t. the original vertices
                const vec3 bar = (c[0]*lambda[0] + c[1]*lambda[1] + c[2]*lambda[2])/sum;
                auto [discard, color] = shader.fragment(bar);
                if (discard) continue;         // discarded fragments leave color and depth untouched
                zbuffer[x+y*W] = z;
                framebuffer.set(x, y, color);
            }
        }
    }
}

