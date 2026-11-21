#pragma once

#include "Tracer.hpp"

class Whitted : public Tracer {
public:
    int max_depth;

    Whitted();
    Whitted(World *w, int d = 5);

    RGBColor trace_ray(const Ray &ray, int depth) const override;
};
