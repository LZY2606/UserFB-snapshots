#include "Matte.hpp"
#include "../utilities/ShadeInfo.hpp"
#include "../utilities/Constants.hpp"
#include "../world/World.hpp"
#include "../lights/Light.hpp"
#include <algorithm>

Matte::Matte() {}
Matte::Matte(float ka, float kd, const RGBColor &c) {
    ambient_brdf = Lambertian(ka, c);
    diffuse_brdf = Lambertian(kd, c);
}

RGBColor Matte::shade(const ShadeInfo &si) const {
    Vector3D wo = -si.ray.d;
    RGBColor L = ambient_brdf.rho(si, wo) * si.w->ambient;

    for (auto *lt : si.w->lights) {
        Vector3D wi = lt->get_direction(si);
        float ndot = (float)(si.normal * wi);
        if (ndot <= 0.0f) continue;

        bool shadowed = false;
        if (lt->shadows) {
            Ray sr(si.hit_point + si.normal * 0.001f, wi);
            shadowed = lt->in_shadow(sr, si);
        }
        if (!shadowed)
            L += diffuse_brdf.f(si, wi, wo) * lt->L(si) * ndot;
    }
    return L;
}
