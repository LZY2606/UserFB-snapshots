// Synthetic clipped-rasterizer demo and conformance checks; no external assets required.
// Renders a checkerboard scene crossing several frustum planes into clipped_raster.tga
// and runs numeric assertions on coverage, clipping, depth and perspective-correct weights.
#include "our_gl.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

extern std::vector<double> zbuffer; // the depth buffer, to inspect it directly in the checks

namespace {

int failures = 0;
void check(const bool ok, const char *what) {
    std::cout << (ok ? "ok   " : "FAIL ") << what << '\n';
    failures += !ok;
}

const TGAColor BLACK{0,0,0,255},   WHITE{255,255,255,255}, RED{0,0,255,255},
               GREEN{0,255,0,255}, BLUE{255,0,0,255},      YELLOW{0,255,255,255},
               MAGENTA{255,0,255,255}, BG{40,40,40,255};

bool rgb_eq(const TGAColor a, const TGAColor b) {
    return a[0]==b[0] && a[1]==b[1] && a[2]==b[2];
}

int count_color(const TGAImage &img, const TGAColor c) {
    int n = 0;
    for (int y=0; y<img.height(); y++)
        for (int x=0; x<img.width(); x++)
            n += rgb_eq(img.get(x,y), c);
    return n;
}

bool zbuffer_untouched() {
    for (const double z : zbuffer)
        if (z!=-1000) return false;
    return true;
}

struct Solid : IShader {
    TGAColor c;
    Solid(const TGAColor c) : c(c) {}
    std::pair<bool,TGAColor> fragment(vec3) const override { return {false, c}; }
};

struct Counting : IShader { // counts fragment invocations, to detect double/missing coverage
    int &calls;
    TGAColor c;
    Counting(int &calls, const TGAColor c) : calls(calls), c(c) {}
    std::pair<bool,TGAColor> fragment(vec3) const override { calls++; return {false, c}; }
};

struct Checker : IShader { // procedural UV checkerboard, same interpolation contract as PhongShader
    vec2 uv[3];
    TGAColor even, odd;
    int cells;
    Checker(const vec2 a, const vec2 b, const vec2 c, const TGAColor e, const TGAColor o, const int n)
        : uv{a,b,c}, even(e), odd(o), cells(n) {}
    std::pair<bool,TGAColor> fragment(const vec3 bar) const override {
        const vec2 u = uv[0]*bar[0] + uv[1]*bar[1] + uv[2]*bar[2];
        const bool o = (int(std::floor(u.x*cells)) + int(std::floor(u.y*cells))) & 1;
        return {false, o ? odd : even};
    }
};

struct UVEncode : IShader { // packs the interpolated uv into the first two color channels
    vec2 uv[3];
    UVEncode(const vec2 a, const vec2 b, const vec2 c) : uv{a,b,c} {}
    std::pair<bool,TGAColor> fragment(const vec3 bar) const override {
        const vec2 u = uv[0]*bar[0] + uv[1]*bar[1] + uv[2]*bar[2];
        TGAColor c{0,0,0,255};
        c[0] = std::uint8_t(std::clamp(u.x, 0., 1.)*255);
        c[1] = std::uint8_t(std::clamp(u.y, 0., 1.)*255);
        return {false, c};
    }
};

struct DiscardHalf : IShader { // discards the half of the triangle where the first weight is small
    TGAColor c;
    DiscardHalf(const TGAColor c) : c(c) {}
    std::pair<bool,TGAColor> fragment(const vec3 bar) const override { return {bar[0]<.5, c}; }
};

void demo_scene() { // synthetic geometry crossing frustum planes + UV checkerboard, no external assets
    constexpr int W=256, H=256;
    init_viewport(0,0,W,H);
    init_zbuffer(W,H);
    TGAImage img(W,H,TGAImage::RGB,BG);

    Checker board({0,0},{1.5,0},{.75,1.5}, YELLOW, BLUE, 8);
    Triangle t1 = {{-2,-1.5,0,1},{1.5,-1.5,0,1},{0,2,-2,2}}; // crosses the side planes, apex on the top and near planes
    rasterize_clipped(t1, board, img);

    Checker behind({0,0},{1,0},{.5,1}, GREEN, MAGENTA, 4);
    Triangle t3 = {{-.95,.45,.2,1},{-.35,.45,.2,1},{-.65,1.8,-2,-1}}; // one vertex behind the camera
    rasterize_clipped(t3, behind, img);

    Solid occluder{RED};
    Triangle t2 = {{-.5,-.5,.6,1},{.7,-.5,.6,1},{.1,.8,.6,1}}; // closer to the camera than the checkerboard
    rasterize_clipped(t2, occluder, img);

    check(img.write_tga_file("clipped_raster.tga"), "demo: clipped_raster.tga written");
    check(count_color(img,YELLOW)>500 && count_color(img,BLUE)>500, "demo: checkerboard drawn across the clipped planes");
    check(count_color(img,GREEN)>50 && count_color(img,MAGENTA)>50, "demo: triangle crossing the camera plane is drawn");
    check(count_color(img,RED)>1000 && rgb_eq(img.get(140,119),RED), "demo: nearer triangle occludes the checkerboard");
    check(rgb_eq(img.get(255,255),BG), "demo: background preserved outside the geometry");
}

void test_shared_edge_exactly_once() { // one quad split in two, the diagonal goes through pixel centers
    constexpr int W=8;
    init_viewport(0,0,W,W);
    init_zbuffer(W,W);
    TGAImage img(W,W,TGAImage::RGB,BLACK);
    int calls = 0;
    Counting shader{calls, WHITE};
    Triangle a = {{-1,-1,0,1},{1,-1,0,1},{1,1,0,1}};     // vertices exactly on the frustum planes
    Triangle b = {{-1,-1,.2,1},{1,1,.2,1},{-1,1,.2,1}};  // uniformly closer: the z-buffer cannot mask overlaps
    rasterize_clipped(a, shader, img);
    rasterize_clipped(b, shader, img);
    check(calls==W*W, "shared edge: every pixel reaches the fragment shader exactly once");
    check(count_color(img,WHITE)==W*W, "shared edge: no gap along the diagonal");
}

void test_clipped_neighbors_exactly_once() { // two triangles sharing an edge, both clipped by several planes
    constexpr int W=32;
    init_viewport(0,0,W,W);
    const vec4 P0{-2,-.5,-1.5,1}, P1{.5,-.5,.5,1}, P2{.5,.5,.5,1}, P3{-2,.5,-1.5,1}; // quad crossing the left and near planes
    const Triangle ta = {P0,P1,P2}, tb = {P0,P2,P3}; // shared edge P0-P2, traversed in opposite directions

    init_zbuffer(W,W);
    TGAImage ia(W,W,TGAImage::RGB,BLACK);
    int ca=0; Counting sa{ca,WHITE};
    rasterize_clipped(ta,sa,ia);
    init_zbuffer(W,W);
    TGAImage ib(W,W,TGAImage::RGB,BLACK);
    int cb=0; Counting sb{cb,WHITE};
    rasterize_clipped(tb,sb,ib);
    const int pa = count_color(ia,WHITE), pb = count_color(ib,WHITE);
    check(pa>0 && ca==pa, "clipped neighbor A: one fragment per covered pixel (fan internal edges included)");
    check(pb>0 && cb==pb, "clipped neighbor B: one fragment per covered pixel (fan internal edges included)");
    int overlap = 0;
    for (int y=0; y<W; y++)
        for (int x=0; x<W; x++)
            overlap += rgb_eq(ia.get(x,y),WHITE) && rgb_eq(ib.get(x,y),WHITE);
    check(overlap==0, "clipped neighbors: zero overlap along the shared clipped edge");

    init_zbuffer(W,W); // combined render, B uniformly closer so the z-buffer cannot hide double coverage
    TGAImage both(W,W,TGAImage::RGB,BLACK);
    vec4 q0=P0, q2=P2, q3=P3;
    q0.z+=.3; q2.z+=.3; q3.z+=.3;
    const Triangle tb_closer = {q0,q2,q3};
    int calls=0; Counting sc{calls,WHITE};
    rasterize_clipped(ta,sc,both);
    rasterize_clipped(tb_closer,sc,both);
    check(calls==pa+pb && count_color(both,WHITE)==pa+pb, "clipped neighbors: union covered exactly once, no gap");
}

void test_perspective_weights_survive_clipping() { // clipped big triangle vs fully-inside sub-triangle
    constexpr int W=64;
    init_viewport(0,0,W,W);
    Checker big({0,0},{2,0},{1,2}, BLACK, WHITE, 4);
    Triangle t = {{-2,-1,.5,1},{2,-1,.5,1},{0,2,.5,2}}; // crosses both side planes, w in {1,1,2}
    init_zbuffer(W,W);
    TGAImage i1(W,W,TGAImage::RGB,BLUE);
    rasterize_clipped(t,big,i1);

    // mid-segment triangle of the same 3D triangle: clip-space midpoints (P_i+P_j)/2,
    // uv from the corresponding original-triangle weights, so both uv fields must coincide
    Checker small({1,0},{1.5,1},{.5,1}, BLACK, WHITE, 4);
    Triangle q = {{0,-1,.5,1},{1,.5,.5,1.5},{-1,.5,.5,1.5}};
    init_zbuffer(W,W);
    TGAImage i2(W,W,TGAImage::RGB,BLUE);
    rasterize_clipped(q,small,i2);

    int n=0, mismatch=0;
    for (int y=0; y<W; y++)
        for (int x=0; x<W; x++)
            if (!rgb_eq(i2.get(x,y),BLUE)) {
                n++;
                mismatch += !rgb_eq(i1.get(x,y), i2.get(x,y));
            }
    check(n>100 && mismatch==0, "clipped vertices keep the original perspective-correct weights (uv field identical)");
    check(count_color(i1,BLACK)>0 && count_color(i1,WHITE)>0, "checkerboard still visible after clipping");
}

void test_perspective_division() { // same screen triangle, w in {1,4,16}: uv follows the 3D ratio, not the screen ratio
    constexpr int W=64;
    init_viewport(0,0,W,W);
    init_zbuffer(W,W);
    TGAImage img(W,W,TGAImage::RGB,BG);
    UVEncode shader({0,0},{1,0},{0,1});
    Triangle t = {{-.5,-.5,0,1},{2,-2,0,4},{-8,8,0,16}}; // ndc (-.5,-.5), (.5,-.5), (-.5,.5)
    rasterize_clipped(t,shader,img);
    // analytic expectation: bar_i = (beta_i/w_i) / sum(beta/w), uv = (bar1, bar2)
    check(img.get(40,20)[0]==166 && img.get(40,20)[1]==7,  "perspective-correct weights with different w, sample 1");
    check(img.get(20,40)[0]==50  && img.get(20,40)[1]==69, "perspective-correct weights with different w, sample 2");
}

void test_vertex_on_plane() {
    constexpr int W=64;
    init_viewport(0,0,W,W);
    init_zbuffer(W,W);
    TGAImage img(W,W,TGAImage::RGB,BG);
    int calls=0;
    Counting shader{calls,GREEN};
    Triangle onplane = {{0,0,-1,1},{-.8,-.6,0,1},{.8,-.6,0,1}}; // one vertex exactly on the near plane
    rasterize_clipped(onplane,shader,img);
    check(calls>0 && rgb_eq(img.get(32,20),GREEN), "vertex exactly on a frustum plane: triangle is drawn");
    const int after = calls;
    Triangle point_contact = {{0,0,-1,1},{.5,-.5,-2,1},{-.5,.5,-2,1}}; // touches the near plane in a single point
    rasterize_clipped(point_contact,shader,img);
    check(calls==after, "zero-area contact with a plane draws nothing");
}

void test_camera_plane() { // one vertex behind the camera (w<0): the front part must still be drawn
    constexpr int W=64;
    init_viewport(0,0,W,W);
    init_zbuffer(W,W);
    TGAImage img(W,W,TGAImage::RGB,BG);
    int calls=0;
    Counting shader{calls,GREEN};
    Triangle t = {{-.5,-.5,0,1},{.5,-.5,0,1},{0,2,-3,-1}};
    rasterize_clipped(t,shader,img);
    check(calls>0 && rgb_eq(img.get(32,20),GREEN), "triangle crossing the camera plane is clipped, not dropped");
    check(rgb_eq(img.get(32,40),BG) && rgb_eq(img.get(10,20),BG), "geometry behind the camera stays invisible");
}

void test_discard_then_background() {
    constexpr int W=64;
    init_viewport(0,0,W,W);
    init_zbuffer(W,W);
    TGAImage img(W,W,TGAImage::RGB,BG);
    Triangle near_t = {{-1,-1,.5,1},{1,-1,.5,1},{-1,1,.5,1}};
    Triangle far_t  = {{-1,-1,0,1},{1,-1,0,1},{-1,1,0,1}};
    DiscardHalf disc{RED};
    rasterize_clipped(near_t,disc,img); // keeps only the bar[0]>=.5 half
    check(zbuffer[4+4*W]==.5 && zbuffer[40+4*W]==-1000, "discarded fragments leave the z-buffer untouched");
    Solid solid{GREEN};
    rasterize_clipped(far_t,solid,img);
    check(rgb_eq(img.get(4,4),RED),  "kept fragments occlude the background drawn afterwards");
    check(rgb_eq(img.get(40,4),GREEN), "background shows through discarded fragments");
    check(count_color(img,RED)==528 && count_color(img,GREEN)==1488, "discard/background exact pixel counts");
}

void test_invisible_and_degenerate() {
    constexpr int W=16;
    init_viewport(0,0,W,W);
    init_zbuffer(W,W);
    TGAImage img(W,W,TGAImage::RGB,BG);
    int calls=0;
    Counting shader{calls,WHITE};
    Triangle outside = {{2,0,0,1},{3,1,0,1},{2,-1,0,1}};          // fully beyond the right plane
    Triangle behind  = {{.5,.5,1,-1},{-.5,.5,1,-1},{0,-.5,1,-1}}; // fully behind the camera
    Triangle flat    = {{0,0,0,1},{1,0,0,1},{2,0,0,1}};           // zero area
    rasterize_clipped(outside,shader,img);
    rasterize_clipped(behind,shader,img);
    rasterize_clipped(flat,shader,img);
    check(calls==0, "invisible/degenerate triangles never reach the fragment shader");
    check(count_color(img,BG)==W*W && zbuffer_untouched(), "invisible/degenerate triangles leave color and depth untouched");
}

void test_nonfinite() {
    constexpr int W=16;
    init_viewport(0,0,W,W);
    init_zbuffer(W,W);
    TGAImage img(W,W,TGAImage::RGB,BG);
    int calls=0;
    Counting shader{calls,WHITE};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    Triangle tnan = {{nan,0,0,1},{0,1,0,1},{1,0,0,1}};
    Triangle tinf = {{0,0,0,inf},{0,1,0,1},{1,0,0,1}};
    rasterize_clipped(tnan,shader,img);
    rasterize_clipped(tinf,shader,img);
    check(calls==0 && count_color(img,BG)==W*W && zbuffer_untouched(), "non-finite input is rejected without side effects");
}

} // namespace

int main() {
    demo_scene();
    test_shared_edge_exactly_once();
    test_clipped_neighbors_exactly_once();
    test_perspective_weights_survive_clipping();
    test_perspective_division();
    test_vertex_on_plane();
    test_camera_plane();
    test_discard_then_background();
    test_invisible_and_degenerate();
    test_nonfinite();
    if (failures) std::cout << failures << " check(s) FAILED\n";
    else          std::cout << "all checks passed\n";
    return failures ? 1 : 0;
}
