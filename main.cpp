#include <iostream>
#include <string>
#include <vector>
#include "our_gl.h"
#include "ambient_occlusion.h"
#include "model.h"

struct PhongShader : IShader {
    const Model &model;
    vec4 l;              // light direction in eye coordinates
    vec2  varying_uv[3]; // triangle uv coordinates, written by the vertex shader, read by the fragment shader
    vec4 varying_nrm[3]; // normal per vertex to be interpolated by the fragment shader
    vec4 tri[3];         // triangle in view coordinates
    const std::vector<double> *ao = nullptr; // per-pixel ambient occlusion, nullptr = disabled (default)
    int ao_width = 0, ao_height = 0;

    PhongShader(const vec3 light, const Model &m) : model(m) {
        l = normalized((ModelView*vec4{light.x, light.y, light.z, 0.})); // transform the light vector to view coordinates
    }

    virtual vec4 vertex(const int face, const int vert) {
        varying_uv[vert]  = model.uv(face, vert);
        varying_nrm[vert] = ModelView.invert_transpose() * model.normal(face, vert);
        vec4 gl_Position = ModelView * model.vert(face, vert);
        tri[vert] = gl_Position;
        return Perspective * gl_Position;                         // in clip coordinates
    }

    vec3 view_pos(const vec3 bar) const {                         // perspective-correct view-space position
        return (tri[0]*bar[0] + tri[1]*bar[1] + tri[2]*bar[2]).xyz();
    }

    vec4 shade_normal(const vec3 bar) const {                     // the normal actually used for lighting
        mat<2,4> E = { tri[1]-tri[0], tri[2]-tri[0] };
        mat<2,2> U = { varying_uv[1]-varying_uv[0], varying_uv[2]-varying_uv[0] };
        mat<2,4> T = U.invert() * E;
        mat<4,4> D = {normalized(T[0]),  // tangent vector
                      normalized(T[1]),  // bitangent vector
                      normalized(varying_nrm[0]*bar[0] + varying_nrm[1]*bar[1] + varying_nrm[2]*bar[2]), // interpolated normal
                      {0,0,0,1}}; // Darboux frame
        vec2 uv = varying_uv[0] * bar[0] + varying_uv[1] * bar[1] + varying_uv[2] * bar[2];
        return normalized(D.transpose() * model.normal(uv));      // normal-mapped, in view coordinates
    }

    bool geometry(const vec3 bar, vec3 &view_pos_out, vec3 &view_nrm_out) const override {
        view_pos_out = view_pos(bar);
        view_nrm_out = shade_normal(bar).xyz();
        return true;
    }

    TGAColor shade(const vec3 bar, const double ao_scale) const { // final shading, AO multiplies ambient only
        vec2 uv = varying_uv[0] * bar[0] + varying_uv[1] * bar[1] + varying_uv[2] * bar[2];
        vec4 n = shade_normal(bar);
        vec4 r = normalized(n * (n * l)*2 - l);                   // reflected light direction
        double ambient  = .4*ao_scale;                            // ambient light intensity, occluded
        double diffuse  = 1.*std::max(0., n * l);                 // diffuse light intensity, untouched
        double specular = (.5+2.*sample2D(model.specular(), uv)[0]/255.) * std::pow(std::max(r.z, 0.), 35);  // specular intensity, untouched
        TGAColor gl_FragColor = sample2D(model.diffuse(), uv);
        for (int channel : {0,1,2})
            gl_FragColor[channel] = std::min<int>(255, gl_FragColor[channel]*(ambient + diffuse + specular));
        return gl_FragColor;
    }

    std::pair<bool,TGAColor> fragment(const vec3 bar) const override {
        return {false, shade(bar, 1.)};                           // no occlusion: identical to the original
    }

    std::pair<bool,TGAColor> fragment_at(const vec3 bar, const int x, const int y) const override {
        double a = 1.;
        if (ao && x>=0 && x<ao_width && y>=0 && y<ao_height)
            a = (*ao)[x+y*ao_width];
        return {false, shade(bar, a)};
    }
};

int main(int argc, char** argv) {
    AOParams ao_params{.radius = .5, .bias = 1e-2, .filter_radius = 2};
    bool ao_enabled = false;                                      // AO is off by default
    std::vector<std::string> objfiles;
    for (int i=1; i<argc; i++) {
        std::string a = argv[i];
        if (a=="--ao") ao_enabled = true;
        else if (a=="--ao-radius" && i+1<argc) ao_params.radius        = std::stod(argv[++i]);
        else if (a=="--ao-bias"   && i+1<argc) ao_params.bias          = std::stod(argv[++i]);
        else if (a=="--ao-filter" && i+1<argc) ao_params.filter_radius = std::stoi(argv[++i]);
        else if (!a.empty() && a[0]=='-') { std::cerr << "unknown option " << a << std::endl; return 1; }
        else objfiles.push_back(a);
    }
    if (objfiles.empty()) {
        std::cerr << "Usage: " << argv[0] << " [--ao] [--ao-radius R] [--ao-bias B] [--ao-filter N] obj/model.obj" << std::endl;
        return 1;
    }
    if (ao_enabled && !ao_params_valid(ao_params)) {
        std::cerr << "invalid AO parameters (need finite R>0, B>=0, N>=0)" << std::endl;
        return 1;
    }

    constexpr int width  = 800;      // output image size
    constexpr int height = 800;
    constexpr vec3  light{ 1, 1, 1}; // light source
    constexpr vec3    eye{-1, 0, 2}; // camera position
    constexpr vec3 center{ 0, 0, 0}; // camera direction
    constexpr vec3     up{ 0, 1, 0}; // camera up vector

    lookat(eye, center, up);                                   // build the ModelView   matrix
    init_perspective(norm(eye-center));                        // build the Perspective matrix
    init_viewport(width/16, height/16, width*7/8, height*7/8); // build the Viewport    matrix
    init_zbuffer(width, height);
    TGAImage framebuffer(width, height, TGAImage::RGB, {177, 195, 209, 255});

    GBuffer gbuffer;
    std::vector<double> ao;
    if (ao_enabled) {                                            // geometry capture pass setup
        gbuffer.resize(width, height);
        g_gbuffer_sink = &gbuffer;
    }

    auto render = [&](const bool apply_ao) {
        for (const std::string &file : objfiles) {           // iterate through all input objects
            Model model(file);                               // load the data
            PhongShader shader(light, model);
            if (apply_ao) {
                shader.ao = &ao;
                shader.ao_width  = width;
                shader.ao_height = height;
            }
            for (int f=0; f<model.nfaces(); f++) {           // iterate through all facets
                Triangle clip = { shader.vertex(f, 0),       // assemble the primitive
                                  shader.vertex(f, 1),
                                  shader.vertex(f, 2) };
                rasterize(clip, shader, framebuffer);        // rasterize the primitive
            }
        }
    };

    render(false);                                             // visibility + geometry pass

    if (ao_enabled) {                                          // ambient occlusion pass
        g_gbuffer_sink = nullptr;
        if (!compute_ao(gbuffer, ao_params, ao)) {
            std::cerr << "AO computation failed" << std::endl;
            return 1;
        }
        if (ao_params.filter_radius>0)
            filter_ao(gbuffer, ao_params, ao);
        ao_grayscale(ao, width, height).write_tga_file("ambient_occlusion.tga");
        init_zbuffer(width, height);                           // final shading pass with the AO-modulated ambient
        framebuffer = TGAImage(width, height, TGAImage::RGB, {177, 195, 209, 255});
        render(true);
    }

    framebuffer.write_tga_file("framebuffer.tga");
    return 0;
}
