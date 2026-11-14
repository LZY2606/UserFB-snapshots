// Example: render an isosurface built purely from in-memory scalar data.
// The scalar field is the squared distance from a point sampled on a regular
// grid; its F = r^2 level set is intersected directly and shaded together
// with the original spheres through the scene-aware recursive tracer.
// The output is a binary PPM written next to the executable's working dir.
#include <cstdio>
#include <vector>
#include <array>
#include <cmath>
#include <fstream>
#include <algorithm>

#include "../isosurface.h"

int main() {
    constexpr int N = 24;                       // vertices per axis (23 cells)
    const vec3 grid_origin{-2.5f, -2.5f, -11.f};
    const std::array<double,3> spacing{5.0/(N-1), 5.0/(N-1), 5.0/(N-1)};
    const vec3 center{0.0f, 0.0f, -8.5f};
    const double radius2 = 1.5*1.5;

    std::vector<double> values(static_cast<size_t>(N)*N*N);
    for (int k = 0; k < N; ++k)
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i) {
                const vec3 p{grid_origin.x + static_cast<float>(i*spacing[0]),
                             grid_origin.y + static_cast<float>(j*spacing[1]),
                             grid_origin.z + static_cast<float>(k*spacing[2])};
                const vec3 d = p - center;
                values[i + N*(j + N*k)] = d*d;
            }

    Material iso_mat = glass;
    iso_mat.diffuse_color = {0.7f, 0.85f, 0.95f};
    IsoGrid grid(grid_origin, spacing, {N,N,N}, values, radius2, iso_mat);

    Scene scene = make_default_scene();
    scene.iso = &grid;

    constexpr int width = 320, height = 240;
    constexpr float fov = 1.05f;
    std::vector<vec3> framebuffer(width*height);
    for (int pix = 0; pix < width*height; ++pix) {
        const float dir_x =  (pix%width + 0.5f) -  width/2.f;
        const float dir_y = -(pix/width + 0.5f) + height/2.f;
        const float dir_z = -height/(2.f*std::tan(fov/2.f));
        framebuffer[pix] = cast_ray(vec3{0,0,0},
                                    vec3{dir_x, dir_y, dir_z}.normalized(),
                                    scene);
    }

    std::ofstream ofs("isosurface.ppm", std::ios::binary);
    if (!ofs) { std::perror("isosurface.ppm"); return 1; }
    ofs << "P6\n" << width << " " << height << "\n255\n";
    for (const vec3 &color : framebuffer) {
        const float max = std::max(1.f, std::max(color[0], std::max(color[1], color[2])));
        for (int chan : {0,1,2})
            ofs << static_cast<char>(255 * color[chan]/max);
    }
    std::printf("wrote isosurface.ppm (%dx%d)\n", width, height);
    return 0;
}
