#include <iostream>
#include <chrono>
#include <atomic>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "materials/Cosine.hpp"

#include "samplers/Sampler.hpp"
#include "tracers/Tracer.hpp"

#include "image/Image.hpp"
#include "utilities/RGBColor.hpp"
#include "utilities/Ray.hpp"
#include "utilities/ShadeInfo.hpp"

#include "world/World.hpp"
#include "world/ViewPlane.hpp"

int main(int argc, char **argv) {
    World world;
    world.build();

    Sampler *sampler = world.sampler_ptr;
    ViewPlane &viewplane = world.vplane;
    Image image(viewplane);
    Tracer *tracer = world.tracer_ptr;

    int hres = viewplane.hres;
    int vres = viewplane.vres;

#ifdef _OPENMP
    int nthreads = omp_get_max_threads();
    std::cout << "Using " << nthreads << " threads.\n";
#endif

    auto start = std::chrono::high_resolution_clock::now();

    std::atomic<int> done{0};
    int report_every = std::max(1, hres / 20);

#pragma omp parallel for schedule(dynamic, 4)
    for (int x = 0; x < hres; x++) {
        for (int y = 0; y < vres; y++) {
            RGBColor pixel_color(0);
            std::vector<Ray> rays = sampler->get_rays(x, y);
            for (const auto &ray : rays) {
                float weight = ray.w;
                if (tracer) {
                    pixel_color += weight * tracer->trace_ray(ray, 0);
                } else {
                    ShadeInfo sinfo = world.hit_objects(ray);
                    if (sinfo.hit)
                        pixel_color += weight * sinfo.material_ptr->shade(sinfo);
                    else
                        pixel_color += weight * world.bg_color;
                }
            }
            image.set_pixel(x, y, pixel_color);
        }
        int d = ++done;
        if (d % report_every == 0) {
#pragma omp critical
            std::cout << "  " << d << "/" << hres << " columns\n";
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    double secs = std::chrono::duration<double>(end - start).count();
    std::cout << "Render time: " << secs << " seconds\n";

    image.write_png("scene.png");
    std::cout << "Wrote image.\n";

    return 0;
}
