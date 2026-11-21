#pragma once

#include "../utilities/RGBColor.hpp"

class Ray;
class World;

class Tracer {
public:
    World *world_ptr;

    Tracer();
    Tracer(World *w);
    virtual ~Tracer() = default;

    virtual RGBColor trace_ray(const Ray &ray, int depth) const = 0;
};
