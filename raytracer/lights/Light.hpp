#pragma once

#include "../utilities/RGBColor.hpp"
#include "../utilities/Vector3D.hpp"
#include "../utilities/Point3D.hpp"
#include "../utilities/Ray.hpp"

class ShadeInfo;

class Light {
public:
    bool shadows;

    Light();
    virtual ~Light() = default;

    virtual Vector3D get_direction(const ShadeInfo &si) const = 0;
    virtual RGBColor L(const ShadeInfo &si) const = 0;
    virtual bool in_shadow(const Ray &ray, const ShadeInfo &si) const;
};
