// Original tiny ray tracer entry point.  Geometry, materials and the legacy
// scene functions now live in renderer.h; the isosurface extension lives in
// isosurface.h / isosurface.cpp.  This file keeps the historical demo main and
// its default-scene results byte-for-byte equivalent.
#include "renderer.h"

#include <fstream>
#include <vector>
#include <algorithm>
#include <cmath>

#ifndef TINYRT_NO_MAIN
int main() {
    constexpr int   width  = 1024;
    constexpr int   height = 768;
    constexpr float fov    = 1.05; // 60 degrees field of view in radians
    std::vector<vec3> framebuffer(width*height);
#pragma omp parallel for
    for (int pix = 0; pix<width*height; pix++) { // actual rendering loop
        float dir_x =  (pix%width + 0.5) -  width/2.;
        float dir_y = -(pix/width + 0.5) + height/2.; // this flips the image at the same time
        float dir_z = -height/(2.*tan(fov/2.));
        framebuffer[pix] = cast_ray(vec3{0,0,0}, vec3{dir_x, dir_y, dir_z}.normalized());
    }

    std::ofstream ofs("./out.ppm", std::ios::binary);
    ofs << "P6\n" << width << " " << height << "\n255\n";
    for (vec3 &color : framebuffer) {
        float max = std::max(1.f, std::max(color[0], std::max(color[1], color[2])));
        for (int chan : {0,1,2})
            ofs << (char)(255 *  color[chan]/max);
    }
    return 0;
}
#endif // TINYRT_NO_MAIN
