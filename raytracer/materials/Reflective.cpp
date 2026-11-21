#include "Reflective.hpp"
#include "../utilities/ShadeInfo.hpp"
#include "../utilities/Constants.hpp"
#include "../world/World.hpp"
#include "../lights/Light.hpp"
#include "../tracers/Tracer.hpp"

Reflective::Reflective() {}
Reflective::Reflective(float ka, float kd, float ks, const RGBColor &c, float e, float kr) {
    ambient_brdf  = Lambertian(ka, c);
    diffuse_brdf  = Lambertian(kd, c);
    specular_brdf = GlossySpecular(ks, RGBColor(1), e);
    mirror_brdf   = PerfectSpecular(kr, RGBColor(1));
}

RGBColor Reflective::shade(const ShadeInfo &si) const {
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
            L += (diffuse_brdf.f(si, wi, wo) + specular_brdf.f(si, wi, wo)) * lt->L(si) * ndot;
    }

    Vector3D n = si.normal;
    double ndotwo = n * wo;
    Vector3D rd = -wo + n * 2.0 * ndotwo;
    rd.normalize();

    Ray rr(si.hit_point + n * 0.001f, rd);
    RGBColor rc = si.w->tracer_ptr->trace_ray(rr, si.depth + 1);
    L += mirror_brdf.kr * mirror_brdf.cr * rc;

    return L;
}
