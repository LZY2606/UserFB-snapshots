#pragma once

#include "Tracer.hpp"

class Basic : public Tracer {
public:
    Basic();
    Basic(World *w);

    RGBColor trace_ray(const Ray &ray, int depth) const override;
};
