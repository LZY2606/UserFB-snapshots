// Standalone demo: render an analytic isosurface stored only as an in-memory
// scalar vertex grid, together with the original spheres. The output is a
// PPM framebuffer written next to the executable's working directory.
#include "renderer.h"
#include "isosurface.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

int main() {
    // A wavy sheet: F(x,y,z) = (z - z0) - amp*sin(x)*cos(y), so the
    // F = 0 surface is z = z0 + amp*sin(x)*cos(y). dF/dz is always 1,
    // hence the gradient never vanishes on the surface.
    const int n = 30;                 // 29 cells per axis: partial last brick
    const vec3 grid_origin{3,-3,-16};
    const vec3 grid_spacing{6.0f/(n-1), 6.0f/(n-1), 6.0f/(n-1)};
    const double z0 = -13.0, amp = 1.2;

    std::vector<double> scalar(size_t(n)*n*n);
    for (int k = 0; k < n; ++k)
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const double x = grid_origin.x + i*grid_spacing.x;
                const double y = grid_origin.y + j*grid_spacing.y;
                const double z = grid_origin.z + k*grid_spacing.z;
                scalar[(k*n + j)*n + i] =
                    (z - z0) - amp*std::sin(x)*std::cos(y);
            }

    Material sheet = glass;
    IsoGrid grid(grid_origin, grid_spacing, n, n, n,
                 scalar, 0.0, sheet);

    Scene scene;
    scene.scene_spheres.assign(std::begin(spheres), std::end(spheres));
    scene.scene_lights.assign(std::begin(lights), std::end(lights));
    scene.iso = &grid;

    constexpr int width = 256, height = 192;
    constexpr float fov = 1.05f;
    std::vector<vec3> framebuffer(width*height);
    for (int pix = 0; pix < width*height; ++pix) {
        const float dir_x =  (pix%width + 0.5f) -  width/2.f;
        const float dir_y = -(pix/width + 0.5f) + height/2.f;
        const float dir_z = -height/(2.f*std::tan(fov/2.f));
        framebuffer[pix] =
            cast_ray(vec3{0,0,0},
                     vec3{dir_x, dir_y, dir_z}.normalized(), scene);
    }

    std::ofstream ofs("./isosurface_example.ppm", std::ios::binary);
    ofs << "P6\n" << width << " " << height << "\n255\n";
    for (const vec3& color : framebuffer) {
        const float mx = std::max(1.f,
            std::max(color[0], std::max(color[1], color[2])));
        for (int chan = 0; chan < 3; ++chan)
            ofs << char(255 * color[chan]/mx);
    }

    // One standalone grid query, direction deliberately non-unit, showing
    // the acceleration statistics produced by the production traversal.
    QueryStats stats;
    IsoHit hit;
    const bool found = grid.intersect(vec3{0,0,0}, vec3{2,0,-5},
                                      0.f, 100.f, hit, &stats);
    std::printf("wrote isosurface_example.ppm (%dx%d)\n", width, height);
    std::printf("probe ray hit=%d t=%f point=(%.3f,%.3f,%.3f) "
                "cell=(%d,%d,%d)\n",
                int(found), hit.t, hit.point.x, hit.point.y, hit.point.z,
                hit.cell[0], hit.cell[1], hit.cell[2]);
    std::printf("traversal work: bricks_visited=%llu cells_solved=%llu\n",
                stats.bricks_visited, stats.cells_solved);
    return 0;
}
