#include "Lambertian.hpp"
#include "../utilities/Constants.hpp"

Lambertian::Lambertian() : kd(0), cd(0) {}
Lambertian::Lambertian(float k, const RGBColor &c) : kd(k), cd(c) {}

RGBColor Lambertian::f(const ShadeInfo &si, const Vector3D &wi, const Vector3D &wo) const {
    return cd * kd * invPI;
}

RGBColor Lambertian::rho(const ShadeInfo &si, const Vector3D &wo) const {
    return cd * kd;
}
