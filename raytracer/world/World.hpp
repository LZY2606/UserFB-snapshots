#pragma once

#include <vector>

#include "../utilities/RGBColor.hpp"

#include "ViewPlane.hpp"

class Camera;
class Geometry;
class Light;
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

public:
    World();
    ~World();

    void add_geometry(Geometry *geom_ptr);
    void add_light(Light *light_ptr);
    void set_camera(Camera *c_ptr);

    void build();

    void build_accel();

    ShadeInfo hit_objects(const Ray &ray);
};
