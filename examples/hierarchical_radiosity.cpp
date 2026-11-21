// Hierarchical radiosity demonstration.
//
// Renders two 64x32 Whitted frames into one P3 PPM montage (128x32):
//   left  - initial emitter radiosity
//   right - after a set_emission() update that reuses the tree and links
// The scene contains a colored diffuse receiver, an emissive triangle and
// an occluding sphere; the receiver only receives indirect light, so the
// second frame visibly brightens without any new visibility query.
//
// Usage: hierarchical_radiosity <output.ppm>

#include "raytracer/world/World.hpp"
#include "raytracer/cameras/Parallel.hpp"
#include "raytracer/samplers/Simple.hpp"
#include "raytracer/geometry/Triangle.hpp"
#include "raytracer/geometry/Sphere.hpp"
#include "raytracer/materials/Matte.hpp"
#include "raytracer/materials/HierarchicalDiffuse.hpp"
#include "raytracer/lighting/HierarchicalRadiosity.hpp"
#include "raytracer/tracers/Whitted.hpp"
#include "raytracer/utilities/RGBColor.hpp"
#include "raytracer/utilities/Ray.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 32;

Triangle *emitter_face() {
    // Normal faces -z, toward the receiver below it.
    return new Triangle(Point3D(-1.0f, -1.0f, 0.6f),
                        Point3D(-1.0f, 1.0f, 0.6f),
                        Point3D(1.0f, -1.0f, 0.6f));
}

Triangle *receiver_face() {
    // Normal faces +z, toward the emitter.
    return new Triangle(Point3D(-1.0f, -1.0f, -0.6f),
                        Point3D(1.0f, -1.0f, -0.6f),
                        Point3D(-1.0f, 1.0f, -0.6f));
}

std::vector<RGBColor> render_frame(World &world) {
    std::vector<RGBColor> buffer(kWidth * kHeight, RGBColor(0.0f));
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            for (const Ray &ray : world.sampler_ptr->get_rays(x, y))
                buffer[y * kWidth + x] =
                    buffer[y * kWidth + x] +
                    ray.w * world.tracer_ptr->trace_ray(ray, 0);
        }
    }
    return buffer;
}

int encode_channel(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return (int)std::lround(v * 255.0f);
}

void write_p3(const std::string &path,
              const std::vector<RGBColor> &first,
              const std::vector<RGBColor> &second) {
    std::FILE *out = std::fopen(path.c_str(), "wb");
    if (!out) {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        std::exit(1);
    }
    std::fprintf(out, "P3\n%d %d\n255\n", 2 * kWidth, kHeight);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < 2 * kWidth; ++x) {
            const RGBColor &c = x < kWidth
                                    ? first[y * kWidth + x]
                                    : second[y * kWidth + (x - kWidth)];
            std::fprintf(out, "%d %d %d\n",
                         encode_channel(c.r), encode_channel(c.g),
                         encode_channel(c.b));
        }
    }
    std::fclose(out);
}

double frame_energy(const std::vector<RGBColor> &frame) {
    double total = 0.0;
    for (const RGBColor &c : frame) total += c.r + c.g + c.b;
    return total;
}

} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output.ppm>\n", argv[0]);
        return 1;
    }

    World world;
    world.bg_color = RGBColor(0.01f, 0.01f, 0.02f);
    world.ambient = RGBColor(0.0f); // hierarchical material ignores ambient.

    Triangle *emitter = emitter_face();
    Triangle *receiver = receiver_face();
    Sphere *occluder = new Sphere(Point3D(0.55f, 0.15f, 0.0f), 0.15f);
    world.add_geometry(emitter);
    world.add_geometry(receiver);
    world.add_geometry(occluder);

    auto *mat_emitter = new HierarchicalDiffuse();
    auto *mat_receiver = new HierarchicalDiffuse();
    auto *mat_occluder = new Matte(0.0f, 0.2f, RGBColor(0.05f, 0.05f, 0.08f));
    emitter->set_material(mat_emitter);
    receiver->set_material(mat_receiver);
    occluder->set_material(mat_occluder);

    world.build_accel();

    const int depth = 3;
    HierarchicalRadiosity radiosity(
        world, {emitter, receiver},
        {RGBColor(0.7f, 0.6f, 0.4f), RGBColor(0.2f, 0.45f, 0.8f)},
        {RGBColor(1.0f, 0.35f, 0.12f), RGBColor(0.0f, 0.0f, 0.0f)},
        depth, 1e-3);
    if (!radiosity.ok()) {
        std::fprintf(stderr, "radiosity build failed: %s\n",
                     radiosity.error().c_str());
        return 2;
    }
    mat_emitter->set_solution(&radiosity);
    mat_receiver->set_solution(&radiosity);

    // Parallel projection down -z: the receiver at z=-1 fills the frame, so
    // lit pixels are indirect light from the emitter (which sits behind the
    // view plane) rather than a tiny perspective sliver.
    world.set_camera(new Parallel(0.0f, 0.0f, -1.0f));
    world.vplane.top_left = Point3D(-1.0f, 1.0f, 0.2f);
    world.vplane.bottom_right = Point3D(1.0f, -1.0f, 0.2f);
    world.vplane.hres = kWidth;
    world.vplane.vres = kHeight;
    world.sampler_ptr = new Simple(world.camera_ptr, &world.vplane);
    world.tracer_ptr = new Whitted(&world);

    std::vector<RGBColor> first = render_frame(world);
    double first_energy = frame_energy(first);

    // Second frame: only the emission changes; tree, links and occlusion
    // results are reused with zero additional visibility queries.
    unsigned long long queries_before = radiosity.visibility_queries();
    if (!radiosity.set_emission(0, RGBColor(3.0f, 1.8f, 0.9f))) {
        std::fprintf(stderr, "emission update failed\n");
        return 3;
    }
    if (radiosity.visibility_queries() != queries_before) {
        std::fprintf(stderr, "emission update issued new visibility queries\n");
        return 4;
    }
    std::vector<RGBColor> second = render_frame(world);
    double second_energy = frame_energy(second);

    write_p3(argv[1], first, second);

    std::printf("hierarchical radiosity frame montage: %s\n", argv[1]);
    std::printf("leaves=%zu links=%zu exact_visibility_queries=%llu\n",
                radiosity.leaf_count(), radiosity.links().size(),
                radiosity.visibility_queries());
    std::printf("dense leaf pairs would be %zu; storage peak %zu bytes\n",
                radiosity.full_leaf_pair_count(),
                radiosity.storage_peak_bytes());
    std::printf("frame energy first=%.4f second=%.4f (indirect brightening)\n",
                first_energy, second_energy);
    std::printf("operator applications=%llu node tests=%llu prim tests=%llu\n",
                radiosity.operator_applications(), radiosity.node_tests(),
                radiosity.primitive_tests());

    delete mat_emitter;
    delete mat_receiver;
    delete mat_occluder;
    return 0;
}
