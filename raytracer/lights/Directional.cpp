#include "Directional.hpp"

Directional::Directional() : dir(0, -1, 0), color(1), ls(1.0f) {}
Directional::Directional(const Vector3D &d, const RGBColor &c, float intensity)
    : color(c), ls(intensity) {
    dir = d;
    dir.normalize();
}

Vector3D Directional::get_direction(const ShadeInfo &si) const {
    return dir;
}

RGBColor Directional::L(const ShadeInfo &si) const {
    return color * ls;
}
