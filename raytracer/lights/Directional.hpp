#pragma once

#include "Light.hpp"

class Directional : public Light {
public:
    Vector3D dir;
    RGBColor color;
    float ls;

    Directional();
    Directional(const Vector3D &d, const RGBColor &c, float intensity);

    Vector3D get_direction(const ShadeInfo &si) const override;
    RGBColor L(const ShadeInfo &si) const override;
};
