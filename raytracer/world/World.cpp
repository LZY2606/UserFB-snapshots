#include "World.hpp"

#include "../acceleration/BVH.hpp"
#include "../cameras/Camera.hpp"
#include "../geometry/Geometry.hpp"
#include "../lights/Light.hpp"
#include "../samplers/Sampler.hpp"
#include "../tracers/Tracer.hpp"
#include "../utilities/BBox.hpp"
#include "../utilities/Constants.hpp"
#include "../utilities/Point3D.hpp"
#include "../utilities/Ray.hpp"
#include "../utilities/ShadeInfo.hpp"

World::World()
    : bg_color(black), ambient(0.1f, 0.1f, 0.1f),
      camera_ptr(nullptr), sampler_ptr(nullptr),
      tracer_ptr(nullptr), accel_ptr(nullptr), use_accel(false),
      query_stats(new QueryStats()) {}

World::~World() {
    for (auto g : geometry) delete g;
    for (auto l : lights) delete l;
    delete camera_ptr;
    delete sampler_ptr;
    delete tracer_ptr;
    delete accel_ptr;
    delete query_stats;
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

bool World::segment_occluded(const Point3D &O, const Point3D &Q,
                             const Geometry *skip_a, const Geometry *skip_b,
                             double s_lo, double s_hi) {
    if (use_accel && accel_ptr) {
        return accel_ptr->segment_occluded(O, Q, s_lo, s_hi,
                                           skip_a, skip_b, *query_stats);
    }

    for (auto *g : geometry) {
        if (g == skip_a || g == skip_b) continue;
        ++query_stats->prim_tests;
        double s_hit;
        if (g->hit_segment(O, Q, s_lo, s_hi, s_hit)) return true;
    }
    return false;
}

bool World::bundle_may_occlude(const BBox &bundle,
                               const Geometry *skip_a, const Geometry *skip_b) {
    if (use_accel && accel_ptr) {
        return accel_ptr->bundle_may_occlude(bundle, skip_a, skip_b,
                                             *query_stats);
    }

    for (auto *g : geometry) {
        if (g == skip_a || g == skip_b) continue;
        ++query_stats->prim_tests;
        BBox b = g->getBBox();
        if (b.overlaps(bundle)) return true;
    }
    return false;
}

unsigned long long World::node_tests() const {
    return query_stats ? query_stats->node_tests : 0;
}

unsigned long long World::primitive_tests() const {
    return query_stats ? query_stats->prim_tests : 0;
}

void World::reset_query_stats() {
    if (query_stats) query_stats->reset();
}
