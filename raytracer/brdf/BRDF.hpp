#pragma once

#include "../utilities/RGBColor.hpp"
#include "../utilities/Vector3D.hpp"

class ShadeInfo;

class BRDF {
public:
    BRDF() = default;
    virtual ~BRDF() = default;

    virtual RGBColor f(const ShadeInfo &si, const Vector3D &wi, const Vector3D &wo) const = 0;
    virtual RGBColor rho(const ShadeInfo &si, const Vector3D &wo) const = 0;
};
