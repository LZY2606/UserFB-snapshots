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

RasterTriangle prepare_raster_triangle(const Triangle &clip, int width, int height) {
    RasterTriangle tri;
    for (int i=0; i<3; i++)
        tri.ndc[i] = clip[i]/clip[i].w; // normalized device coordinates
    for (int i=0; i<3; i++)
        tri.screen[i] = (Viewport*tri.ndc[i]).xy(); // screen coordinates

    tri.ABC = {{ {tri.screen[0].x, tri.screen[0].y, 1.},
                 {tri.screen[1].x, tri.screen[1].y, 1.},
                 {tri.screen[2].x, tri.screen[2].y, 1.} }};
    if (tri.ABC.det()<1) return tri; // backface culling + discarding triangles that cover less than a pixel

    auto [bbminx,bbmaxx] = std::minmax({tri.screen[0].x, tri.screen[1].x, tri.screen[2].x}); // bounding box for the triangle
    auto [bbminy,bbmaxy] = std::minmax({tri.screen[0].y, tri.screen[1].y, tri.screen[2].y}); // defined by its top left and bottom right corners
    tri.bbminx = std::max<int>(bbminx, 0);              // clip the bounding box by the screen
    tri.bbmaxx = std::min<int>(bbmaxx, width-1);
    tri.bbminy = std::max<int>(bbminy, 0);
    tri.bbmaxy = std::min<int>(bbmaxy, height-1);
    tri.valid = tri.bbminx<=tri.bbmaxx && tri.bbminy<=tri.bbmaxy;
    return tri;
}

bool raster_coverage(const RasterTriangle &tri, const Triangle &clip,
                     int x, int y, vec3 &bc_clip, double &depth) {
    if (!tri.valid) return false;
    vec3 bc_screen = tri.ABC.invert_transpose() * vec3{static_cast<double>(x), static_cast<double>(y), 1.}; // barycentric coordinates of {x,y} w.r.t the triangle
    bc_clip = { bc_screen.x/clip[0].w, bc_screen.y/clip[1].w, bc_screen.z/clip[2].w }; // check https://github.com/ssloy/tinyrenderer/wiki/Technical-difficulties-linear-interpolation-with-perspective-deformations
    bc_clip = bc_clip / (bc_clip.x + bc_clip.y + bc_clip.z);
    if (bc_screen.x<0 || bc_screen.y<0 || bc_screen.z<0) return false; // negative barycentric coordinate => the pixel is outside the triangle
    depth = bc_screen * vec3{ tri.ndc[0].z, tri.ndc[1].z, tri.ndc[2].z }; // linear interpolation of the depth
    return true;
}

void rasterize(const Triangle &clip, const IShader &shader, TGAImage &framebuffer) {
    RasterTriangle tri = prepare_raster_triangle(clip, framebuffer.width(), framebuffer.height());
    if (!tri.valid) return;
    const int width = framebuffer.width();
#pragma omp parallel for
    for (int x=tri.bbminx; x<=tri.bbmaxx; x++) {
        for (int y=tri.bbminy; y<=tri.bbmaxy; y++) {
            vec3 bc_clip;
            double z;
            if (!raster_coverage(tri, clip, x, y, bc_clip, z)) continue;
            if (z <= zbuffer[x+y*width]) continue;   // discard fragments that are too deep w.r.t the z-buffer
            auto [discard, color] = shader.fragment(bc_clip);
            if (discard) continue;                                 // fragment shader can discard current fragment
            zbuffer[x+y*width] = z;                  // update the z-buffer
            framebuffer.set(x, y, color);            // update the framebuffer
        }
    }
}

