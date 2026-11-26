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

void gl_project_triangle(const vec4 clip[3], const mat<4,4> &viewport,
                         vec4 ndc[3], vec2 screen[3]) {
    for (int i = 0; i < 3; i++) {
        ndc[i]    = clip[i]/clip[i].w;                                            // normalized device coordinates
        screen[i] = (viewport*ndc[i]).xy();                                       // screen coordinates
    }
}

PixelBounds gl_pixel_bounds(const vec2 screen[3], int width, int height) {
    auto [bbminx,bbmaxx] = std::minmax({screen[0].x, screen[1].x, screen[2].x}); // bounding box for the triangle
    auto [bbminy,bbmaxy] = std::minmax({screen[0].y, screen[1].y, screen[2].y}); // defined by its top left and bottom right corners
    return { std::max<int>(bbminx, 0),
             std::max<int>(bbminy, 0),
             std::min<int>(bbmaxx, width-1),
             std::min<int>(bbmaxy, height-1) };
}

bool gl_screen_barycentric(const vec2 screen[3], int x, int y, vec3 &bc_screen) {
    mat<3,3> ABC = {{ {screen[0].x, screen[0].y, 1.}, {screen[1].x, screen[1].y, 1.}, {screen[2].x, screen[2].y, 1.} }};
    if (ABC.det()<1) return false; // backface culling + discarding triangles that cover less than a pixel
    bc_screen = ABC.invert_transpose() * vec3{static_cast<double>(x), static_cast<double>(y), 1.}; // barycentric coordinates of {x,y} w.r.t the triangle
    return !(bc_screen.x<0 || bc_screen.y<0 || bc_screen.z<0); // negative barycentric coordinate => the pixel is outside the triangle
}

vec3 gl_perspective_barycentric(const vec3 &bc_screen, const vec4 clip[3]) {
    vec3 bc_clip = { bc_screen.x/clip[0].w, bc_screen.y/clip[1].w, bc_screen.z/clip[2].w };     // check https://github.com/ssloy/tinyrenderer/wiki/Technical-difficulties-linear-interpolation-with-perspective-deformations
    return bc_clip / (bc_clip.x + bc_clip.y + bc_clip.z);
}

double gl_interpolated_depth(const vec3 &bc_screen, const vec4 ndc[3]) {
    return bc_screen * vec3{ ndc[0].z, ndc[1].z, ndc[2].z };   // linear interpolation of the depth
}

void rasterize(const Triangle &clip, const IShader &shader, TGAImage &framebuffer) {
    vec4 ndc[3];
    vec2 screen[3];
    gl_project_triangle(clip, Viewport, ndc, screen);

    PixelBounds bb = gl_pixel_bounds(screen, framebuffer.width(), framebuffer.height());
#pragma omp parallel for
    for (int x=bb.x0; x<=bb.x1; x++) {         // clip the bounding box by the screen
        for (int y=bb.y0; y<=bb.y1; y++) {
            vec3 bc_screen;
            if (!gl_screen_barycentric(screen, x, y, bc_screen)) continue;
            vec3 bc_clip = gl_perspective_barycentric(bc_screen, clip);
            double z = gl_interpolated_depth(bc_screen, ndc);
            if (z <= zbuffer[x+y*framebuffer.width()]) continue;   // discard fragments that are too deep w.r.t the z-buffer
            auto [discard, color] = shader.fragment(bc_clip);
            if (discard) continue;                                 // fragment shader can discard current fragment
            zbuffer[x+y*framebuffer.width()] = z;                  // update the z-buffer
            framebuffer.set(x, y, color);                          // update the framebuffer
        }
    }
}
