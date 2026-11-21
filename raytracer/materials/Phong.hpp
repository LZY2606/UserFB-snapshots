#pragma once

#include "Material.hpp"
#include "../brdf/Lambertian.hpp"
#include "../brdf/GlossySpecular.hpp"

class Phong : public Material {
public:
    Lambertian ambient_brdf;
    Lambertian diffuse_brdf;
    GlossySpecular specular_brdf;

    Phong();
    Phong(float ka, float kd, float ks, const RGBColor &c, float exp);

    RGBColor shade(const ShadeInfo &si) const override;
};
