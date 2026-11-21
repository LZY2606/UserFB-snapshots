#pragma once

#include "BRDF.hpp"

class Lambertian : public BRDF {
public:
    float kd;
    RGBColor cd;

    Lambertian();
    Lambertian(float k, const RGBColor &c);

    RGBColor f(const ShadeInfo &si, const Vector3D &wi, const Vector3D &wo) const override;
    RGBColor rho(const ShadeInfo &si, const Vector3D &wo) const override;
};
