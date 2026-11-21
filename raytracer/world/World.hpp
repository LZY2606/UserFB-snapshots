#pragma once

#include <vector>

#include "../utilities/RGBColor.hpp"

#include "ViewPlane.hpp"

class BBox;
class Camera;
class Geometry;
class Light;
class Point3D;
struct QueryStats;
class Ray;
class Sampler;
class ShadeInfo;
class Tracer;
class BVH;

class World {
public:
    ViewPlane vplane;
    RGBColor bg_color;
    RGBColor ambient;
    std::vector<Geometry *> geometry;
    std::vector<Light *> lights;
    Camera *camera_ptr;
    Sampler *sampler_ptr;
    Tracer *tracer_ptr;
    BVH *accel_ptr;
    bool use_accel;
    QueryStats *query_stats; // cumulative visibility work counters.

public:
    World();
    ~World();

    void add_geometry(Geometry *geom_ptr);
    void add_light(Light *light_ptr);
    void set_camera(Camera *c_ptr);

    void build();

    void build_accel();

    ShadeInfo hit_objects(const Ray &ray);

    // Open-segment visibility: O + s (Q - O), s in (s_lo, s_hi). All scene
    // geometry blocks on both sides; skip_a and skip_b (the segment's own
    // endpoints' primitives) never block. Returns true when blocked.
    bool segment_occluded(const Point3D &O, const Point3D &Q,
                          const Geometry *skip_a, const Geometry *skip_b,
                          double s_lo = 1e-8, double s_hi = 1.0 - 1e-8);

    // Conservative bundle query: true if any non-skipped primitive's AABB
    // overlaps the bundle AABB. Sampling is never used to clear a bundle.
    bool bundle_may_occlude(const BBox &bundle,
                            const Geometry *skip_a, const Geometry *skip_b);

    // Actual node/primitive test counts accumulated since the last reset.
    unsigned long long node_tests() const;
    unsigned long long primitive_tests() const;
    void reset_query_stats();
};
