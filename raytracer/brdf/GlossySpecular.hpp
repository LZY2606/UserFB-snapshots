#pragma once

#include "BRDF.hpp"

class GlossySpecular : public BRDF {
public:
    float ks;
    RGBColor cs;
    float exp;

    GlossySpecular();
    GlossySpecular(float k, const RGBColor &c, float e);

    RGBColor f(const ShadeInfo &si, const Vector3D &wi, const Vector3D &wo) const override;
    RGBColor rho(const ShadeInfo &si, const Vector3D &wo) const override;
};
