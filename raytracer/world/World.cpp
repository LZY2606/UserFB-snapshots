#include "World.hpp"

#include "../cameras/Camera.hpp"
#include "../geometry/Geometry.hpp"
#include "../lights/Light.hpp"
#include "../samplers/Sampler.hpp"
#include "../tracers/Tracer.hpp"
#include "../acceleration/BVH.hpp"
#include "../utilities/Constants.hpp"
#include "../utilities/Ray.hpp"
#include "../utilities/ShadeInfo.hpp"

World::World()
    : bg_color(black), ambient(0.1f, 0.1f, 0.1f),
      camera_ptr(nullptr), sampler_ptr(nullptr),
      tracer_ptr(nullptr), accel_ptr(nullptr), use_accel(false) {}

World::~World() {
    for (auto g : geometry) delete g;
    for (auto l : lights) delete l;
    delete camera_ptr;
    delete sampler_ptr;
    delete tracer_ptr;
    delete accel_ptr;
}

void World::add_geometry(Geometry *geom_ptr) {
    geometry.push_back(geom_ptr);
}

void World::add_light(Light *light_ptr) {
    lights.push_back(light_ptr);
}

void World::set_camera(Camera *c_ptr) {
    camera_ptr = c_ptr;
}

void World::build_accel() {
    if (accel_ptr) delete accel_ptr;
    accel_ptr = new BVH();
    accel_ptr->build(geometry);
    use_accel = true;
}

ShadeInfo World::hit_objects(const Ray &ray) {
    ShadeInfo sinfo(*this);

    if (use_accel && accel_ptr) {
        float t;
        accel_ptr->hit(ray, t, sinfo);
        return sinfo;
    }

    float t_min = kHugeValue;
    float t;
    for (auto *g : geometry) {
        ShadeInfo s(*this);
        if (g->hit(ray, t, s) && t < t_min) {
            t_min = t;
            sinfo = s;
        }
    }
    return sinfo;
}
