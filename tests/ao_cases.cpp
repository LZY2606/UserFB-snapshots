#include "our_gl.h"
#include "ambient_occlusion.h"
#include "checks.h"
#include <cmath>
#include <limits>
#include <vector>

namespace {

constexpr double PI = 3.14159265358979323846;

bool approx(const double a, const double b, const double eps=1e-9) {
    return std::abs(a-b) < eps;
}

// Minimal shader for the tests: flat color or simplified Phong with an AO
// hook on the ambient term only, plus view-space geometry capture.
struct TestShader : IShader {
    vec4 tri[3] = {};
    vec3 nrm{0,0,1};                 // view-space normal used for lighting
    vec3 l{0,0,1};                   // light direction in view space
    TGAColor color{200,30,40,255};   // flat-mode color
    bool lit = false;                // false: flat color, true: ambient+diffuse lighting
    double base = 100, ambient = .4;
    double discard_below = -1;       // discard fragments with bar[0] < discard_below
    const std::vector<double> *ao = nullptr; // per-pixel AO, nullptr = disabled
    int ao_w = 0, ao_h = 0;

    bool geometry(const vec3 bar, vec3 &p, vec3 &n) const override {
        p = (tri[0]*bar[0] + tri[1]*bar[1] + tri[2]*bar[2]).xyz();
        n = nrm;
        return true;
    }
    std::pair<bool,TGAColor> shade(const vec3 bar, const double a) const {
        if (bar[0] < discard_below) return {true, {}};
        if (!lit) return {false, color};
        const double diff = std::max(0., nrm*l);
        const auto v = static_cast<std::uint8_t>(std::min<int>(255, base*(ambient*a + diff)));
        return {false, {v, v, v, 255}};
    }
    std::pair<bool,TGAColor> fragment(const vec3 bar) const override { return shade(bar, 1.); }
    std::pair<bool,TGAColor> fragment_at(const vec3 bar, const int x, const int y) const override {
        double a = 1.;
        if (ao && x>=0 && x<ao_w && y>=0 && y<ao_h) a = (*ao)[x+y*ao_w];
        return shade(bar, a);
    }
};

void draw_tri(TestShader &sh, const vec3 a, const vec3 b, const vec3 c, TGAImage &fb) {
    sh.tri[0] = {a.x, a.y, a.z, 1.};
    sh.tri[1] = {b.x, b.y, b.z, 1.};
    sh.tri[2] = {c.x, c.y, c.z, 1.};
    Triangle t = { Perspective*sh.tri[0], Perspective*sh.tri[1], Perspective*sh.tri[2] };
    rasterize(t, sh, fb);
}

// counter-clockwise quad (a,b,c,d) as seen on screen
void draw_quad(TestShader &sh, const vec3 a, const vec3 b, const vec3 c, const vec3 d, TGAImage &fb) {
    draw_tri(sh, a, b, c, fb);
    draw_tri(sh, a, c, d, fb);
}

vec3 view_of(const vec3 world) { // world -> view through the current ModelView
    return (ModelView*vec4{world.x, world.y, world.z, 1.}).xyz();
}

} // namespace

// Geometry capture: coverage, view-space reconstruction, two-surface
// consistency, discard handling, resize/clear semantics.
void gbuffer_capture(Checks &C) {
    const int W = 64, H = 64;
    const double f = 3;
    lookat({0,0,f}, {0,0,0}, {0,1,0}); // identity ModelView: view space == world space
    init_perspective(f);
    init_viewport(0, 0, W, H);
    init_zbuffer(W, H);
    TGAImage fb(W, H, TGAImage::RGB, {9,9,9,255});
    GBuffer gb(W, H);
    C.require(gb.width()==W && gb.height()==H);
    C.require(gb.at(-1,0)==nullptr && gb.at(0,-1)==nullptr && gb.at(W,0)==nullptr && gb.at(0,H)==nullptr);

    // --- full-screen plane at z=-1.5: coverage + reconstruction ---
    g_gbuffer_sink = &gb;
    TestShader red; red.color = {255,0,0,255};
    draw_quad(red, {-1.5,-1.5,-1.5}, {1.5,-1.5,-1.5}, {1.5,1.5,-1.5}, {-1.5,1.5,-1.5}, fb);
    g_gbuffer_sink = nullptr;
    int valid = 0;
    for (int y=0; y<H; y++) for (int x=0; x<W; x++) if (gb.at(x,y)->valid) valid++;
    C.require(valid == W*H); // plane covers the whole screen (ndc = +-1)
    // pixel (40,32): ndc.x = 8/32 = .25, perspective scale 1-(-1.5)/3 = 1.5 -> view x = .375
    const GBuffer::Entry *e = gb.at(40,32);
    C.require(e && e->valid);
    C.require(approx(e->pos.x, .375, 1e-6)); // view-space position, not the non-linear z/w
    C.require(approx(e->pos.y, 0.,   1e-6));
    C.require(approx(e->pos.z, -1.5, 1e-6));
    C.require(approx(norm(e->nrm), 1., 1e-12)); // stored normal is unit
    C.require(approx(e->nrm.z, 1., 1e-12));
    e = gb.at(32,40); // ndc.y = .25 -> view y = .375
    C.require(e && e->valid && approx(e->pos.y, .375, 1e-6) && approx(e->pos.x, 0., 1e-6));

    // --- two surfaces drawn back to back: geometry and color agree ---
    init_zbuffer(W,H);
    fb = TGAImage(W, H, TGAImage::RGB, {9,9,9,255});
    gb.clear();
    C.require(!gb.at(32,32)->valid); // cleared, no stale geometry
    g_gbuffer_sink = &gb;
    draw_quad(red, {-1.5,-1.5,-1.5}, {1.5,-1.5,-1.5}, {1.5,1.5,-1.5}, {-1.5,1.5,-1.5}, fb); // far, full screen
    TestShader green; green.color = {0,255,0,255};
    draw_quad(green, {-1.5,-1.5,0.5}, {0,-1.5,0.5}, {0,1.5,0.5}, {-1.5,1.5,0.5}, fb);        // near, left half
    g_gbuffer_sink = nullptr;
    C.require(fb.get(16,32)[1]==255 && approx(gb.at(16,32)->pos.z, 0.5, 1e-6));  // near surface wins
    C.require(fb.get(48,32)[0]==255 && approx(gb.at(48,32)->pos.z, -1.5, 1e-6)); // far surface remains
    C.require(approx(gb.at(16,32)->nrm.z, 1., 1e-12));

    // --- discarded fragments leave no record; what is behind shows through ---
    init_zbuffer(W,H);
    fb = TGAImage(W, H, TGAImage::RGB, {9,9,9,255});
    gb.clear();
    g_gbuffer_sink = &gb;
    draw_quad(red, {-1.5,-1.5,-1.5}, {1.5,-1.5,-1.5}, {1.5,1.5,-1.5}, {-1.5,1.5,-1.5}, fb); // rear, full screen
    TestShader front; front.color = {0,255,0,255}; front.discard_below = .5;                // discards bar[0]<.5
    draw_quad(front, {-1,-1,0.5}, {1,-1,0.5}, {1,1,0.5}, {-1,1,0.5}, fb);                   // front, center
    g_gbuffer_sink = nullptr;
    int kept = 0, discarded = 0;
    bool consistent = true;
    for (int y=0; y<H; y++) for (int x=0; x<W; x++) {
        const TGAColor c = fb.get(x,y);
        const GBuffer::Entry *g = gb.at(x,y);
        if (c[1]==255 && c[0]==0) {          // visible front surface
            consistent &= g->valid && approx(g->pos.z, 0.5, 1e-6);
            kept++;
        } else if (c[0]==255 && c[1]==0) {   // rear surface (front was discarded here)
            consistent &= g->valid && approx(g->pos.z, -1.5, 1e-6);
            discarded++;
        } else if (c[0]==9 && c[1]==9) {     // untouched background
            consistent &= !g->valid;
        } else consistent = false;
    }
    C.require(consistent); // geometry and visible color always belong to the same surface
    C.require(kept>200 && discarded>200);

    // --- resize / clear: no stale geometry across frames or sizes ---
    gb.resize(32,16);
    C.require(gb.width()==32 && gb.height()==16);
    C.require(!gb.at(5,5)->valid);
    gb.record(3,3,{1,2,3},{0,0,2}); // non-unit normal on input
    C.require(gb.at(3,3)->valid && approx(norm(gb.at(3,3)->nrm), 1., 1e-12)); // stored normalized
    C.require(approx(gb.at(3,3)->pos.y, 2., 1e-12));
    gb.clear();
    C.require(!gb.at(3,3)->valid);
    std::vector<double> ao;
    const AOParams p{.radius=1., .bias=0., .filter_radius=0};
    C.require(compute_ao(gb, p, ao));                 // empty gbuffer: everything unoccluded
    C.require(ao.size()==32*16 && ao[7]==1. && ao[31+15*32]==1.);
}

// Hemisphere sampling and per-pixel occlusion counts, checked by hand.
void hemisphere_visibility(Checks &C) {
    const double NaN = std::numeric_limits<double>::quiet_NaN();
    const double Inf = std::numeric_limits<double>::infinity();

    // --- deterministic sample pattern ---
    for (int j=0; j<4; j++) for (int k=0; k<4; k++) {
        const vec3 d = ao_sample_dir(j,k);
        const double z = (j+.5)/4.;
        C.require(approx(d.z, z, 1e-12));
        C.require(approx(std::hypot(d.x,d.y), std::sqrt(1-z*z), 1e-12)); // transverse length
        C.require(approx(norm(d), 1., 1e-12));
        double phi = std::atan2(d.y, d.x); if (phi<0) phi += 2*PI;
        C.require(approx(phi, 2*PI*(k+.5*(j%2))/4., 1e-9));              // azimuth
    }
    const AOParams dp{.radius=2., .bias=0., .filter_radius=0};
    for (int j=0; j<4; j++) C.require(ao_sample_dist(dp,j) == 2.*(j+1)/4.); // sample distance

    // --- tangent frame: orthonormal, deterministic, built from the least-aligned axis ---
    const vec3 n = normalized(vec3{.9,.1,.05});
    vec3 t,b; ao_tangent_frame(n,t,b);
    C.require(approx(norm(t),1.,1e-12) && approx(norm(b),1.,1e-12));
    C.require(approx(t*n,0.,1e-12) && approx(b*n,0.,1e-12) && approx(t*b,0.,1e-12));
    C.require(approx(t.z,0.,1e-12)); // z is the least-aligned axis here
    vec3 t2,b2; ao_tangent_frame(n,t2,b2);
    C.require(t.x==t2.x && t.y==t2.y && t.z==t2.z); // deterministic
    C.require(b.x==b2.x && b.y==b2.y && b.z==b2.z);

    const int W = 64, H = 64;
    auto unproject = [&](const int px, const int py, const double z, const double f) {
        const double s = 1. - z/f;
        return vec3{ (px-W/2.)/(W/2.)*s, (py-H/2.)/(H/2.)*s, z };
    };

    // --- hand-computed occlusion: floor point under a ceiling at z=.5 ---
    // f=10, R=1.5, b=0. Sample heights z_j*d_j = .0469, .281, .703, 1.31:
    // j=0 and j=1 stay below the ceiling (8 samples), all within radius -> AO = 1-8/16 = .5
    lookat({0,0,10}, {0,0,0}, {0,1,0});
    init_perspective(10);
    init_viewport(0,0,W,H);
    GBuffer gb(W,H);
    for (int y=0; y<H; y++) for (int x=0; x<W; x++)
        gb.record(x,y, unproject(x,y,.5,10), {0,0,-1}); // ceiling everywhere
    gb.record(32,32, {0,0,0}, {0,0,1});                 // floor point at the center
    std::vector<double> ao;
    const AOParams p1{.radius=1.5, .bias=0., .filter_radius=0};
    C.require(compute_ao(gb, p1, ao));
    C.require(ao[32+32*W] == .5);   // exactly 8 of 16 samples occluded
    C.require(ao[40+40*W] <= .25);  // ceiling pixel: hemisphere below, (nearly) all samples under the ceiling

    // same scene with a bias of .3: only j=0 samples (.5-.0469=.45>.3) count -> AO=.75
    std::vector<double> ao2;
    const AOParams p2{.radius=1.5, .bias=.3, .filter_radius=0};
    C.require(compute_ao(gb, p2, ao2));
    C.require(ao2[32+32*W] == .75);

    // --- ceiling only on the right half: background samples never occlude ---
    // j=0,k=0 (+x) and j=1,k=0,3 (+x diagonals) reach the ceiling: 3 samples -> AO=13/16
    GBuffer gb2(W,H);
    for (int y=0; y<H; y++) for (int x=40; x<W; x++)
        gb2.record(x,y, unproject(x,y,.5,10), {0,0,-1});
    gb2.record(32,32, {0,0,0}, {0,0,1});
    std::vector<double> ao3;
    C.require(compute_ao(gb2, p1, ao3));
    C.require(approx(ao3[32+32*W], 13./16., 1e-12));

    // --- front/back separated plates: the distance condition (|q-s|<=R) ---
    // Front plate at z=+3: closer than the samples in frontal depth but 1.7+ units
    // away -> must NOT occlude. Same plate at z=+.9: within R for j=0,1,2 -> 12 occluded.
    for (const auto [zplate, want] : { std::pair{3., 1.}, std::pair{.9, .25} }) {
        GBuffer gb3(W,H);
        for (int y=0; y<H; y++) for (int x=0; x<W; x++)
            gb3.record(x,y, unproject(x,y,zplate,10), {0,0,-1});     // front plate near the camera
        gb3.record(32,32, {0,0,0}, {0,0,1});                         // back point at the center
        std::vector<double> ao4;
        C.require(compute_ao(gb3, p1, ao4));
        C.require(approx(ao4[32+32*W], want, 1e-12));
    }

    // --- behind the camera and off-screen samples count as unoccluded ---
    // f=1, R=2: j=0 samples (height .0625) sit under the z=.5 plate -> occluded;
    // j=1,2 project off-screen; j=3 (height 1.75) ends up behind the camera.
    init_perspective(1);
    GBuffer gb4(W,H);
    for (int y=0; y<H; y++) for (int x=0; x<W; x++)
        gb4.record(x,y, unproject(x,y,.5,1), {0,0,-1});
    gb4.record(32,32, {0,0,0}, {0,0,1});
    std::vector<double> ao5;
    const AOParams p3{.radius=2., .bias=0., .filter_radius=0};
    C.require(compute_ao(gb4, p3, ao5));
    C.require(ao5[32+32*W] == .75);

    // --- determinism: identical input, identical output ---
    std::vector<double> ao5b;
    C.require(compute_ao(gb4, p3, ao5b));
    C.require(ao5 == ao5b);

    // --- parameter validation ---
    std::vector<double> tmp;
    for (const AOParams bad : { AOParams{.radius=0}, AOParams{.radius=-1}, AOParams{.radius=NaN},
                                AOParams{.radius=Inf}, AOParams{.radius=1,.bias=-.5},
                                AOParams{.radius=1,.bias=NaN}, AOParams{.radius=1,.filter_radius=-1} }) {
        C.require(!ao_params_valid(bad));
        C.require(!compute_ao(gb4, bad, tmp));
    }
    C.require(ao_params_valid(p1));
    std::vector<double> wrong_size(3, 0.);
    C.require(!filter_ao(gb4, p3, wrong_size)); // buffer size mismatch rejected
}

// Separable bilateral filter: weights, normalization, and boundaries.
void bilateral_boundaries(Checks &C) {
    // --- hand-computed 1D filtering ---
    GBuffer gb(5,1);
    for (int x=0; x<5; x++) gb.record(x,0,{1.*x,0,0},{0,0,1});
    std::vector<double> ao = {0,0,1,1,1};
    const AOParams p{.radius=1., .bias=0., .filter_radius=1};
    C.require(filter_ao(gb, p, ao));
    const double w = std::exp(-1./8.); // spatial weight at distance 1
    C.require(approx(ao[0], 0., 1e-12));
    C.require(approx(ao[1], w/(1+2*w), 1e-12));
    C.require(approx(ao[2], (1+w)/(1+2*w), 1e-12));
    C.require(approx(ao[3], 1., 1e-12));
    C.require(approx(ao[4], 1., 1e-12));

    // --- depth boundary: the dark foreground edge must not bleed onto the far wall ---
    GBuffer gb2(21,21);
    std::vector<double> ao2(21*21);
    for (int y=0; y<21; y++) for (int x=0; x<21; x++) {
        const bool near_side = x<10;
        gb2.record(x,y,{1.*x,1.*y, near_side?0.:-5.},{0,0,1});
        ao2[x+y*21] = near_side ? .2 : 1.;
    }
    const AOParams p2{.radius=1., .bias=0., .filter_radius=2};
    C.require(filter_ao(gb2, p2, ao2));
    C.require(approx(ao2[0+10*21], .2, 1e-12));  // uniform interior: normalized back to itself
    C.require(approx(ao2[20+10*21], 1., 1e-12)); // uniform far interior
    C.require(ao2[9+10*21]  > .2-1e-6 && ao2[9+10*21]  < .2+1e-6); // edge pixel, near side
    C.require(ao2[10+10*21] > 1.-1e-6); // far wall next to the dark edge stays bright (depth weight)
    C.require(ao2[11+10*21] > 1.-1e-6);

    // --- background pixels are skipped as neighbors and left untouched ---
    GBuffer gb3(3,1);
    gb3.record(1,0,{0,0,0},{0,0,1}); // only the middle pixel is valid
    std::vector<double> ao3 = {1.,.5,1.};
    C.require(filter_ao(gb3, p, ao3));
    C.require(ao3[1] == .5);  // no valid neighbors: keeps its own value
    C.require(ao3[0] == 1. && ao3[2] == 1.); // background untouched

    // --- normal weight: perpendicular normals do not mix ---
    GBuffer gb4(2,1);
    gb4.record(0,0,{0,0,0},{1,0,0});
    gb4.record(1,0,{1,0,0},{0,1,0});
    std::vector<double> ao4 = {0.,1.};
    C.require(filter_ao(gb4, p, ao4));
    C.require(ao4[0] == 0. && ao4[1] == 1.); // max(0,dot)^8 == 0 across the crease
    GBuffer gb5(2,1);
    gb5.record(0,0,{0,0,0},{0,0,1});
    gb5.record(1,0,{1,0,0},{0,0,1});
    std::vector<double> ao5 = {0.,1.};
    C.require(filter_ao(gb5, p, ao5));
    C.require(approx(ao5[0], w/(1+w), 1e-12)); // aligned normals mix with the spatial weight
    C.require(approx(ao5[1], 1./(1+w), 1e-12));

    // --- filter disabled: raw AO preserved ---
    std::vector<double> ao6 = {.3,.7,.1,.9};
    const std::vector<double> ao6_copy = ao6;
    GBuffer gb6(2,2);
    for (int y=0; y<2; y++) for (int x=0; x<2; x++) gb6.record(x,y,{1.*x,1.*y,0},{0,0,1});
    const AOParams off{.radius=1., .bias=0., .filter_radius=0};
    C.require(filter_ao(gb6, off, ao6));
    C.require(ao6 == ao6_copy);
}

// Lighting integration: AO on ambient only, background, reconstruction, TGAs.
void lighting_integration(Checks &C) {
    const int W = 64, H = 64;
    lookat({0,0,3}, {0,0,0}, {0,1,0});
    init_perspective(3);
    init_viewport(0,0,W,H);

    // --- AO scales the ambient term only; direct light is preserved ---
    TestShader sh;
    sh.lit = true; sh.nrm = {0,0,1}; sh.l = {0,0,1}; sh.base = 100; sh.ambient = .4;
    std::vector<double> ao(W*H, 1.);
    sh.ao = &ao; sh.ao_w = W; sh.ao_h = H;
    auto render_quad = [&]() {
        init_zbuffer(W,H);
        TGAImage fb(W,H,TGAImage::RGB,{9,9,9,255});
        draw_quad(sh, {-1.5,-1.5,-0.75}, {1.5,-1.5,-0.75}, {1.5,1.5,-0.75}, {-1.5,1.5,-0.75}, fb);
        return fb;
    };
    TGAImage fb_full = render_quad();                    // AO == 1 everywhere
    C.require(fb_full.get(32,32)[0] == 140);             // 100*(.4+1)
    std::fill(ao.begin(), ao.end(), .5);
    TGAImage fb_half = render_quad();
    C.require(fb_half.get(32,32)[0] == 120);             // 100*(.4*.5+1): only ambient dropped
    std::fill(ao.begin(), ao.end(), 0.);
    TGAImage fb_zero = render_quad();
    C.require(fb_zero.get(32,32)[0] == 100);             // direct light output preserved
    sh.ao = nullptr;                                     // AO disabled: original result
    TGAImage fb_off = render_quad();
    C.require(fb_off.get(32,32)[0] == 140);

    // --- background pixels are never shaded and keep the clear color ---
    init_zbuffer(W,H);
    TGAImage fb_bg(W,H,TGAImage::RGB,{9,9,9,255});
    sh.ao = &ao;
    draw_quad(sh, {-1.5,-1.5,-0.75}, {0,-1.5,-0.75}, {0,1.5,-0.75}, {-1.5,1.5,-0.75}, fb_bg); // left half only
    C.require(fb_bg.get(48,32)[0]==9 && fb_bg.get(48,32)[1]==9 && fb_bg.get(48,32)[2]==9);
    C.require(fb_bg.get(16,32)[0]==100); // shaded (ao==0 there), background untouched elsewhere

    // --- reconstruction through the lit shader's geometry hook ---
    init_zbuffer(W,H);
    TGAImage fb_g(W,H,TGAImage::RGB,{9,9,9,255});
    GBuffer gb(W,H);
    g_gbuffer_sink = &gb;
    draw_quad(sh, {-1.5,-1.5,-0.75}, {1.5,-1.5,-0.75}, {1.5,1.5,-0.75}, {-1.5,1.5,-0.75}, fb_g);
    g_gbuffer_sink = nullptr;
    const GBuffer::Entry *e = gb.at(40,32);
    C.require(e && e->valid);
    C.require(approx(e->pos.x, .3125, 1e-6)); // ndc .25 * scale 1.25
    C.require(approx(e->pos.z, -0.75, 1e-6));
    C.require(approx(norm(e->nrm), 1., 1e-12));

    // --- grayscale mapping ---
    {
        const std::vector<double> g = {0., .5, 1.};
        TGAImage img = ao_grayscale(g, 3, 1);
        C.require(img.get(0,0)[0]==0 && img.get(1,0)[0]==128 && img.get(2,0)[0]==255);
    }

    // --- full pipeline on a concave corner: geometry built in C++, no models ---
    const int W2 = 96, H2 = 96;
    lookat({0,1,14}, {0,-1,0}, {0,1,0});
    init_perspective(norm(vec3{0,1,14}-vec3{0,-1,0}));
    init_viewport(0,0,W2,H2);
    init_zbuffer(W2,H2);
    TGAImage fb1(W2,H2,TGAImage::RGB,{9,9,9,255});
    GBuffer gbc(W2,H2);
    g_gbuffer_sink = &gbc;

    TestShader floor_sh, wall_sh;
    floor_sh.lit = wall_sh.lit = true;
    floor_sh.nrm = normalized((ModelView*vec4{0,1,0,0}).xyz()); // floor normal +y in view space
    wall_sh.nrm  = normalized((ModelView*vec4{0,0,1,0}).xyz()); // wall normal +z in view space
    floor_sh.l = wall_sh.l = normalized((ModelView*vec4{1,1,1,0}).xyz());
    const vec3 F1=view_of({-1.5,-1.5, 2}), F2=view_of({1.5,-1.5, 2}),  // floor quad
               F3=view_of({ 1.5,-1.5,-3}), F4=view_of({-1.5,-1.5,-3});
    const vec3 W1=view_of({-1.5,-1.5,-2}), W2v=view_of({1.5,-1.5,-2}), // wall quad
               W3=view_of({ 1.5, 0.5,-2}), W4=view_of({-1.5, 0.5,-2});
    draw_quad(floor_sh, F1, F2, F3, F4, fb1);
    draw_quad(wall_sh,  W1, W2v, W3, W4, fb1);
    g_gbuffer_sink = nullptr;

    int covered = 0;
    for (int y=0; y<H2; y++) for (int x=0; x<W2; x++) if (gbc.at(x,y)->valid) covered++;
    C.require(covered > 500);

    const AOParams aop{.radius=.8, .bias=.01, .filter_radius=2};
    std::vector<double> ao_c;
    C.require(compute_ao(gbc, aop, ao_c));
    C.require(filter_ao(gbc, aop, ao_c));
    double amin = 1., amax = 0.;
    int argmin = 0, dark = 0;
    for (int i=0; i<W2*H2; i++) {
        if (!gbc.at(i%W2, i/W2)->valid) continue;
        if (ao_c[i] < amin) { amin = ao_c[i]; argmin = i; }
        amax = std::max(amax, ao_c[i]);
        if (ao_c[i] < .99) dark++;
    }
    C.require(amin < .95);        // the concave corner is darkened
    C.require(amax > .999);       // open areas stay fully lit
    C.require(dark > 20);         // a visible darkened band along the corner

    // small AO grayscale image + final shaded TGA
    C.require(ao_grayscale(ao_c, W2, H2).write_tga_file("ao_grayscale.tga"));
    floor_sh.ao = wall_sh.ao = &ao_c;
    floor_sh.ao_w = wall_sh.ao_w = W2;
    floor_sh.ao_h = wall_sh.ao_h = H2;
    init_zbuffer(W2,H2);
    TGAImage fb2(W2,H2,TGAImage::RGB,{9,9,9,255});
    draw_quad(floor_sh, F1, F2, F3, F4, fb2);
    draw_quad(wall_sh,  W1, W2v, W3, W4, fb2);
    C.require(fb2.write_tga_file("ao_final.tga"));
    // the darkest AO pixel is darker in the AO render than in the unoccluded one
    C.require(fb2.get(argmin%W2, argmin/W2)[0] < fb1.get(argmin%W2, argmin/W2)[0]);
}
