#pragma once

#include "Light.hpp"

class PointLight : public Light {
public:
    Point3D pos;
    RGBColor color;
    float ls;

    PointLight();
    PointLight(const Point3D &p, const RGBColor &c, float intensity);

    Vector3D get_direction(const ShadeInfo &si) const override;
    RGBColor L(const ShadeInfo &si) const override;
    bool in_shadow(const Ray &ray, const ShadeInfo &si) const override;
};
