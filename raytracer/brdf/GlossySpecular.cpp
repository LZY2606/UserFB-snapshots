#include "GlossySpecular.hpp"
#include "../utilities/ShadeInfo.hpp"
#include <cmath>

GlossySpecular::GlossySpecular() : ks(0), cs(1), exp(1) {}
GlossySpecular::GlossySpecular(float k, const RGBColor &c, float e) : ks(k), cs(c), exp(e) {}

RGBColor GlossySpecular::f(const ShadeInfo &si, const Vector3D &wi, const Vector3D &wo) const {
    double ndot = si.normal * wi;
    Vector3D r = -wi + si.normal * 2.0 * ndot;
    double rdot = r * wo;
    if (rdot > 0.0)
        return cs * ks * (float)std::pow(rdot, exp);
    return RGBColor(0);
}

RGBColor GlossySpecular::rho(const ShadeInfo &si, const Vector3D &wo) const {
    return RGBColor(0);
}
