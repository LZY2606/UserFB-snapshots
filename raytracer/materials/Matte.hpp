#pragma once

#include "Material.hpp"
#include "../brdf/Lambertian.hpp"

class Matte : public Material {
public:
    Lambertian ambient_brdf;
    Lambertian diffuse_brdf;

    Matte();
    Matte(float ka, float kd, const RGBColor &c);

    RGBColor shade(const ShadeInfo &si) const override;
};
