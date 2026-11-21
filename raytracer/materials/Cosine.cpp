#include "Cosine.hpp"
#include "../utilities/ShadeInfo.hpp"
#include "../utilities/Vector3D.hpp"

#include <algorithm>

Cosine::Cosine() : color(0) {}
Cosine::Cosine(float c) : color(c) {}
Cosine::Cosine(float r, float g, float b) : color(r, g, b) {}
Cosine::Cosine(const RGBColor &c) : color(c) {}

Cosine::Cosine(const Cosine &other) : Material(other), color(other.color) {}

Cosine &Cosine::operator=(const Cosine &other) {
    if (this != &other) { Material::operator=(other); color = other.color; }
    return *this;
}

RGBColor Cosine::shade(const ShadeInfo &sinfo) const {
    // cos(theta) = normal . (-ray.dir), both assumed unit length
    double cos_theta = sinfo.normal * (-sinfo.ray.d);
    float c = (float)std::max(0.0, cos_theta);
    return color * c;
}
