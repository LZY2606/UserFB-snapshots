// Shadow-volume example: generates a small OBJ occluder + receiver and their
// textures, loads them through Model, shades them with the shared PhongShader
// and renders hard directional shadows through the reusable ShadowRenderer
// interface. Usage: shadow_volume <output-directory>
// Writes shadow-mask.tga (non-zero signed stencil) and shadow-frame.tga.
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "geometry.h"
#include "tgaimage.h"
#include "model.h"
#include "our_gl.h"
#include "phong_shader.h"
#include "shadow_volume.h"

namespace fs = std::filesystem;

namespace {

void write_textures(const std::string &base, const TGAColor &diffuse) {
    TGAImage(8, 8, TGAImage::RGB, diffuse).write_tga_file(base + "_diffuse.tga");
    // Neutral tangent-space normal (0,0,1): B=128,G=128,R=255.
    TGAImage(8, 8, TGAImage::RGB, TGAColor{{128, 128, 255, 255},4})
        .write_tga_file(base + "_nm_tangent.tga");
    TGAImage(8, 8, TGAImage::RGB, TGAColor{{20, 20, 20, 255},4})
        .write_tga_file(base + "_spec.tga");
}

// Outward-wound axis-aligned box centered at c with half size h, including
// per-face UVs and normals.
void write_box_obj(const std::string &path, const vec3 &c, double h) {
    std::ofstream o(path);
    const vec3 p[8] = {
        {c.x-h,c.y-h,c.z-h}, {c.x+h,c.y-h,c.z-h},
        {c.x+h,c.y+h,c.z-h}, {c.x-h,c.y+h,c.z-h},
        {c.x-h,c.y-h,c.z+h}, {c.x+h,c.y-h,c.z+h},
        {c.x+h,c.y+h,c.z+h}, {c.x-h,c.y+h,c.z+h},
    };
    for (const vec3 &q : p) o << "v " << q.x << ' ' << q.y << ' ' << q.z << '\n';
    o << "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n";
    o << "vn -1 0 0\nvn 1 0 0\nvn 0 -1 0\nvn 0 1 0\nvn 0 0 -1\nvn 0 0 1\n";
    const int faces[12][3] = {
        {0,3,2},{0,2,1}, {4,5,6},{4,6,7}, {0,1,5},{0,5,4},
        {3,7,6},{3,6,2}, {0,4,7},{0,7,3}, {1,2,6},{1,6,5},
    };
    const int vn[12] = {5,5, 6,6, 3,3, 4,4, 1,1, 2,2};
    const int uv[4] = {1,2,3,4};
    for (int f = 0; f < 12; ++f) {
        o << "f";
        for (int k = 0; k < 3; ++k)
            o << ' ' << faces[f][k]+1 << '/' << uv[k] << '/' << vn[f];
        o << '\n';
    }
}

// Open receiver grid on the vertical plane x = x0, normal pointing +x.
void write_plane_obj(const std::string &path, double x0,
                     double y0, double y1, double z0, double z1) {
    std::ofstream o(path);
    o << "v " << x0 << ' ' << y0 << ' ' << z0 << '\n'
      << "v " << x0 << ' ' << y1 << ' ' << z0 << '\n'
      << "v " << x0 << ' ' << y1 << ' ' << z1 << '\n'
      << "v " << x0 << ' ' << y0 << ' ' << z1 << '\n';
    o << "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 1 0 0\n";
    o << "f 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n";
}

void volume_from_model(const Model &m, const vec3 &L, double D, ShadowVolume *&out) {
    std::vector<vec3> positions(m.nverts());
    for (int i = 0; i < m.nverts(); ++i) positions[i] = m.vert(i).xyz();
    std::vector<int> faces;
    for (int f = 0; f < m.nfaces(); ++f)
        for (int k = 0; k < 3; ++k)
            faces.push_back(m.position_index(f, k)); // position adjacency, seams intact
    out = new ShadowVolume(positions, faces, L, D);
}

// Convention-correct perspective for the shadow path: eye space looks down
// -z (as produced by lookat); near maps to ndc z = +1 and far to ndc z = -1,
// i.e. the visible clip volume is -w <= x,y,z <= w with w = -ze > 0.
mat<4,4> perspective_gl(double near, double far, double fovy_rad, double aspect) {
    double s = 1.0 / std::tan(fovy_rad / 2.0);
    return mat<4,4>{{{s/aspect,0,0,0},
                     {0,s,0,0},
                     {0,0,-(far+near)/(far-near), -2.0*far*near/(far-near)},
                     {0,0,-1,0}}};
}

// View matrix with the same orientation rows as lookat(), but translating by
// -eye so the eye sits at the eye-space origin: required because the shadow
// clip convention places its eye plane at ze = 0 (w = -ze), whereas the
// legacy lookat() leaves the eye at ze = distance(eye, center).
mat<4,4> view_eye_origin(const vec3 &eye, const vec3 &center, const vec3 &up) {
    vec3 n = normalized(eye - center);
    vec3 l = normalized(cross(up, n));
    vec3 m = normalized(cross(n, l));
    return mat<4,4>{{{l.x,l.y,l.z, -(l*eye)},
                     {m.x,m.y,m.z, -(m*eye)},
                     {n.x,n.y,n.z, -(n*eye)},
                     {0,0,0,1}}};
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <output-directory>\n";
        return 2;
    }
    try {
        fs::path out = argv[1];
        fs::create_directories(out);

        // Light shines mostly along +x, slightly upward. Extrusion along -L
        // stays roughly perpendicular to the view direction, so every shadow
        // volume vertex remains in front of the eye (z >= -w support range).
        const vec3 light = normalized(vec3{1.0, 0.25, 0.35});
        const double D = 4.0; // receiver at x = -2.5 is closer than D along L

        std::string box_obj = (out / "occluder").string();
        std::string plane_obj = (out / "receiver").string();
        write_box_obj(box_obj + ".obj", {0,0,0}, 0.7);
        write_textures(box_obj, TGAColor{{60, 90, 200, 255},4});
        write_plane_obj(plane_obj + ".obj", -2.5, -3.0, 3.0, -5.0, 3.5);
        write_textures(plane_obj, TGAColor{{180, 170, 150, 255},4});

        Model occluder(box_obj + ".obj");
        Model receiver(plane_obj + ".obj");

        constexpr int width = 256, height = 256;
        const vec3 eye{0, 1.2, 8.0}, center{0, 0, -1}, up{0, 1, 0};
        init_viewport(0, 0, width, height);
        extern mat<4,4> ModelView, Viewport, Perspective;
        // The shadow path requires the near=+w/far=-w clip convention and an
        // eye-space origin at the eye itself; the legacy lookat()/
        // init_perspective() depth mapping is only kept for rasterize().
        ModelView = view_eye_origin(eye, center, up);
        Perspective = perspective_gl(0.5, 30.0, std::atan(1.0) * 4.0 / 3.0,
                                     double(width) / double(height));
        mat<4,4> world2clip = Perspective * ModelView;

        // Support-range guard: every generated volume vertex must satisfy
        // z >= -w (and w > 0); no far-plane capping is implemented.
        ShadowVolume *volume = nullptr;
        volume_from_model(occluder, light, D, volume);
        for (const vec3 &p : volume->vertices()) {
            vec4 c = world2clip * vec4{p.x, p.y, p.z, 1};
            if (!(c.w > 0.0 && c.z >= -c.w))
                throw std::runtime_error("shadow volume leaves the supported clip range");
        }

        ShadowRenderer renderer(width, height, Viewport);
        renderer.clear();

        // Opaque visible surfaces first (receiver and occluder alike).
        ShadowPhongShader recv_shader(light, receiver);
        renderer.draw_surface(receiver.nfaces(), recv_shader);
        ShadowPhongShader occ_shader(light, occluder);
        renderer.draw_surface(occluder.nfaces(), occ_shader);

        // Frozen-depth, two-sided z-fail shadow volume.
        renderer.draw_volume(*volume, world2clip);

        // Signed stencil -> grayscale mask (255 = shadowed).
        TGAImage mask(width, height, TGAImage::GRAYSCALE, TGAColor{{0,0,0,255},4});
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                if (renderer.stencil_buffer()[x + y*width] != 0)
                    mask.set(x, y, TGAColor{{255,255,255,255},4});
        if (!mask.write_tga_file((out / "shadow-mask.tga").string()))
            throw std::runtime_error("failed to write shadow-mask.tga");

        // Final quantization happens only here.
        TGAColor bg{{177, 195, 209, 255},4};
        TGAImage frame(width, height, TGAImage::RGB, bg);
        renderer.compose(frame, bg);
        if (!frame.write_tga_file((out / "shadow-frame.tga").string()))
            throw std::runtime_error("failed to write shadow-frame.tga");

        delete volume;
        std::cout << "shadow volume example wrote " << (out / "shadow-mask.tga").string()
                  << " and " << (out / "shadow-frame.tga").string() << '\n';
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "example failed: " << e.what() << '\n';
        return 1;
    }
}
