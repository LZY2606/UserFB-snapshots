#pragma once

#include "BRDF.hpp"

class PerfectSpecular : public BRDF {
public:
    float kr;
    RGBColor cr;

    PerfectSpecular();
    PerfectSpecular(float k, const RGBColor &c);

    RGBColor f(const ShadeInfo &si, const Vector3D &wi, const Vector3D &wo) const override;
    RGBColor rho(const ShadeInfo &si, const Vector3D &wo) const override;
};
