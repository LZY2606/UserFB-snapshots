#include <algorithm>
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

bool project_triangle(const vec4 clip[3], const mat<4,4> &viewport, ProjectedTriangle &out) {
    out.ndc[0]    = clip[0]/clip[0].w;                                   // normalized device coordinates
    out.ndc[1]    = clip[1]/clip[1].w;
    out.ndc[2]    = clip[2]/clip[2].w;
    out.screen[0] = (viewport*out.ndc[0]).xy();                          // screen coordinates
    out.screen[1] = (viewport*out.ndc[1]).xy();
    out.screen[2] = (viewport*out.ndc[2]).xy();

    out.ABC = {{ {out.screen[0].x, out.screen[0].y, 1.},
                 {out.screen[1].x, out.screen[1].y, 1.},
                 {out.screen[2].x, out.screen[2].y, 1.} }};
    return out.ABC.det() >= 1; // backface culling + discarding triangles that cover less than a pixel
}

vec3 screen_barycentric(const mat<3,3> &ABC, const double x, const double y) {
    return ABC.invert_transpose() * vec3{x, y, 1.}; // barycentric coordinates of {x,y} w.r.t the triangle
}

vec3 perspective_barycentric(const vec3 &bc_screen, const vec4 clip[3]) {
    vec3 bc_clip = { bc_screen.x/clip[0].w, bc_screen.y/clip[1].w, bc_screen.z/clip[2].w }; // check https://github.com/ssloy/tinyrenderer/wiki/Technical-difficulties-linear-interpolation-with-perspective-deformations
    return bc_clip / (bc_clip.x + bc_clip.y + bc_clip.z);
}

double interpolate_depth(const vec4 ndc[3], const vec3 &bc_screen) {
    return bc_screen * vec3{ ndc[0].z, ndc[1].z, ndc[2].z }; // linear interpolation of the depth
}

void rasterize(const Triangle &clip, const IShader &shader, TGAImage &framebuffer) {
    ProjectedTriangle pt;
    if (!project_triangle(clip, Viewport, pt)) return;

    auto [bbminx,bbmaxx] = std::minmax({pt.screen[0].x, pt.screen[1].x, pt.screen[2].x}); // bounding box for the triangle
    auto [bbminy,bbmaxy] = std::minmax({pt.screen[0].y, pt.screen[1].y, pt.screen[2].y}); // defined by its top left and bottom right corners
#pragma omp parallel for
    for (int x=std::max<int>(bbminx, 0); x<=std::min<int>(bbmaxx, framebuffer.width()-1); x++) {         // clip the bounding box by the screen
        for (int y=std::max<int>(bbminy, 0); y<=std::min<int>(bbmaxy, framebuffer.height()-1); y++) {
            vec3 bc_screen = screen_barycentric(pt.ABC, static_cast<double>(x), static_cast<double>(y));
            vec3 bc_clip   = perspective_barycentric(bc_screen, clip);
            if (bc_screen.x<0 || bc_screen.y<0 || bc_screen.z<0) continue; // negative barycentric coordinate => the pixel is outside the triangle
            double z = interpolate_depth(pt.ndc, bc_screen);
            if (z <= zbuffer[x+y*framebuffer.width()]) continue;   // discard fragments that are too deep w.r.t the z-buffer
            auto [discard, color] = shader.fragment(bc_clip);
            if (discard) continue;                                 // fragment shader can discard current fragment
            zbuffer[x+y*framebuffer.width()] = z;                  // update the z-buffer
            framebuffer.set(x, y, color);                          // update the framebuffer
        }
    }
}
