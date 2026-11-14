#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "our_gl.h"
#include "model.h"
#include "feature_edges.h"

extern mat<4,4> ModelView, Perspective; // "OpenGL" state matrices and
extern std::vector<double> zbuffer;     // the depth buffer

struct PhongShader : IShader {
    const Model &model;
    vec4 l;              // light direction in eye coordinates
    vec2  varying_uv[3]; // triangle uv coordinates, written by the vertex shader, read by the fragment shader
    vec4 varying_nrm[3]; // normal per vertex to be interpolated by the fragment shader
    vec4 tri[3];         // triangle in view coordinates

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

    virtual std::pair<bool,TGAColor> fragment(const vec3 bar) const {
        mat<2,4> E = { tri[1]-tri[0], tri[2]-tri[0] };
        mat<2,2> U = { varying_uv[1]-varying_uv[0], varying_uv[2]-varying_uv[0] };
        mat<2,4> T = U.invert() * E;
        mat<4,4> D = {normalized(T[0]),  // tangent vector
                      normalized(T[1]),  // bitangent vector
                      normalized(varying_nrm[0]*bar[0] + varying_nrm[1]*bar[1] + varying_nrm[2]*bar[2]), // interpolated normal
                      {0,0,0,1}}; // Darboux frame
        vec2 uv = varying_uv[0] * bar[0] + varying_uv[1] * bar[1] + varying_uv[2] * bar[2];
        vec4 n = normalized(D.transpose() * model.normal(uv));
        vec4 r = normalized(n * (n * l)*2 - l);                   // reflected light direction
        double ambient  = .4;                                     // ambient light intensity
        double diffuse  = 1.*std::max(0., n * l);                 // diffuse light intensity
        double specular = (.5+2.*sample2D(model.specular(), uv)[0]/255.) * std::pow(std::max(r.z, 0.), 35);  // specular intensity, note that the camera lies on the z-axis (in eye coordinates), therefore simple r.z, since (0,0,1)*(r.x, r.y, r.z) = r.z
        TGAColor gl_FragColor = sample2D(model.diffuse(), uv);
        for (int channel : {0,1,2})
            gl_FragColor[channel] = std::min<int>(255, gl_FragColor[channel]*(ambient + diffuse + specular));
        return {false, gl_FragColor};                             // do not discard the pixel
    }
};

namespace {

vec3 parse_vec3(const std::string &text) {
    vec3 out{0., 0., 0.};
    std::string copy = text;
    std::replace(copy.begin(), copy.end(), ',', ' ');
    std::istringstream iss(copy);
    iss >> out.x >> out.y >> out.z;
    return out;
}

struct FeatureLineOptions {
    bool enabled = false;
    vec3 eye{-1., 0., 2.};
    vec3 center{0., 0., 0.};
    vec3 up{0., 1., 0.};
    double crease_angle = 30.;   // degrees
    double line_width = 1.5;     // pixels
    TGAColor line_color{{0, 0, 0, 255}};
    int width = 800;
    int height = 800;
    std::string output = "feature_lines.tga";
};

void usage(const char *prog) {
    std::cerr << "Surface render: " << prog << " obj/model.obj [more.obj]\n"
              << "Line render:    " << prog << " --feature-lines [options] obj/model.obj [more.obj]\n"
              << "  --eye x,y,z         camera position (default -1,0,2)\n"
              << "  --center x,y,z      look-at target (default 0,0,0)\n"
              << "  --up x,y,z          up vector (default 0,1,0)\n"
              << "  --crease-angle deg  crease threshold in degrees (default 30)\n"
              << "  --line-width px     line width in pixels (default 1.5)\n"
              << "  --line-color r,g,b  opaque line color 0..255 (default 0,0,0)\n"
              << "  --size w,h          image size (default 800,800)\n"
              << "  --out file          output TGA (default feature_lines.tga)\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " obj/model.obj" << std::endl;
        return 1;
    }

    FeatureLineOptions opts;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char *name) -> std::string {
            if (i + 1 >= argc) { std::cerr << "missing value for " << name << std::endl; std::exit(1); }
            return argv[++i];
        };
        if (arg == "--feature-lines") {
            opts.enabled = true;
        } else if (arg == "--eye") {
            opts.eye = parse_vec3(next("--eye"));
        } else if (arg == "--center") {
            opts.center = parse_vec3(next("--center"));
        } else if (arg == "--up") {
            opts.up = parse_vec3(next("--up"));
        } else if (arg == "--crease-angle") {
            opts.crease_angle = std::stod(next("--crease-angle"));
        } else if (arg == "--line-width") {
            opts.line_width = std::stod(next("--line-width"));
        } else if (arg == "--line-color") {
            vec3 c = parse_vec3(next("--line-color"));
            opts.line_color = TGAColor{{static_cast<std::uint8_t>(c.x),
                                        static_cast<std::uint8_t>(c.y),
                                        static_cast<std::uint8_t>(c.z), 255}};
        } else if (arg == "--size") {
            vec3 s = parse_vec3(next("--size"));
            opts.width = static_cast<int>(s.x);
            opts.height = static_cast<int>(s.y);
        } else if (arg == "--out") {
            opts.output = next("--out");
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.empty()) {
        usage(argv[0]);
        return 1;
    }

    constexpr vec3 light{ 1, 1, 1}; // light source

    if (!opts.enabled) {
        // Original rendering path: arguments are OBJ paths, fixed camera, fixed size.
        constexpr int width  = 800;      // output image size
        constexpr int height = 800;
        constexpr vec3    eye{-1, 0, 2}; // camera position
        constexpr vec3 center{ 0, 0, 0}; // camera direction
        constexpr vec3     up{ 0, 1, 0}; // camera up vector

        lookat(eye, center, up);                                   // build the ModelView   matrix
        init_perspective(norm(eye-center));                        // build the Perspective matrix
        init_viewport(width/16, height/16, width*7/8, height*7/8); // build the Viewport    matrix
        init_zbuffer(width, height);
        TGAImage framebuffer(width, height, TGAImage::RGB, {177, 195, 209, 255});

        for (int m=1; m<argc; m++) {                    // iterate through all input objects
            Model model(argv[m]);                       // load the data
            PhongShader shader(light, model);
            for (int f=0; f<model.nfaces(); f++) {      // iterate through all facets
                Triangle clip = { shader.vertex(f, 0),  // assemble the primitive
                                  shader.vertex(f, 1),
                                  shader.vertex(f, 2) };
                rasterize(clip, shader, framebuffer);   // rasterize the primitive
            }
        }

        framebuffer.write_tga_file("framebuffer.tga");
        return 0;
    }

    // Feature line rendering: surfaces keep their real depth/discard occlusion,
    // each winning fragment also records its object and original face number.
    const int width = opts.width, height = opts.height;
    lookat(opts.eye, opts.center, opts.up);
    init_perspective(norm(opts.eye - opts.center));
    init_viewport(width/16, height/16, width*7/8, height*7/8);
    init_zbuffer(width, height);
    FragmentOwnership owner;
    resize_ownership(owner, width, height);
    TGAImage framebuffer(width, height, TGAImage::RGB, TGAColor{{177, 195, 209, 255}});

    std::vector<std::unique_ptr<Model>> models;
    for (size_t m = 0; m < positional.size(); ++m) {
        models.emplace_back(std::make_unique<Model>(positional[m]));
        Model &model = *models.back();
        PhongShader shader(light, model);
        for (int f = 0; f < model.nfaces(); ++f) {
            Triangle clip = { shader.vertex(f, 0),
                              shader.vertex(f, 1),
                              shader.vertex(f, 2) };
            rasterize(clip, shader, framebuffer, static_cast<int>(m), f, &owner);
        }
    }

    // All surfaces are drawn first, so later objects occlude earlier line work.
    for (size_t m = 0; m < models.size(); ++m) {
        Model &model = *models[m];
        FeatureEdgeSet features = classify_feature_edges(model, opts.crease_angle);
        overlay_feature_lines(features, model, static_cast<int>(m), opts.eye,
                              opts.line_width, opts.line_color, framebuffer, owner);
    }

    framebuffer.write_tga_file(opts.output.c_str());
    std::cout << "feature lines written to " << opts.output << std::endl;
    return 0;
}
