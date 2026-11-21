#include "PerfectSpecular.hpp"

PerfectSpecular::PerfectSpecular() : kr(0), cr(1) {}
PerfectSpecular::PerfectSpecular(float k, const RGBColor &c) : kr(k), cr(c) {}

RGBColor PerfectSpecular::f(const ShadeInfo &si, const Vector3D &wi, const Vector3D &wo) const {
    return RGBColor(0);
}

RGBColor PerfectSpecular::rho(const ShadeInfo &si, const Vector3D &wo) const {
    return RGBColor(0);
}
