#pragma once

#include "Phong.hpp"
#include "../brdf/PerfectSpecular.hpp"

class Reflective : public Material {
public:
    Lambertian ambient_brdf;
    Lambertian diffuse_brdf;
    GlossySpecular specular_brdf;
    PerfectSpecular mirror_brdf;

    Reflective();
    Reflective(float ka, float kd, float ks, const RGBColor &c, float exp, float kr);

    RGBColor shade(const ShadeInfo &si) const override;
};
