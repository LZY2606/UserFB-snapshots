#include "PointLight.hpp"
#include "../utilities/ShadeInfo.hpp"
#include "../utilities/Constants.hpp"
#include "../world/World.hpp"

PointLight::PointLight() : pos(), color(1), ls(1.0f) {}
PointLight::PointLight(const Point3D &p, const RGBColor &c, float intensity)
    : pos(p), color(c), ls(intensity) {}

Vector3D PointLight::get_direction(const ShadeInfo &si) const {
    Vector3D d = pos - si.hit_point;
    d.normalize();
    return d;
}

RGBColor PointLight::L(const ShadeInfo &si) const {
    return color * ls;
}

bool PointLight::in_shadow(const Ray &ray, const ShadeInfo &si) const {
    float dist = (float)(pos - ray.o).length();
    ShadeInfo s = const_cast<World*>(si.w)->hit_objects(ray);
    return s.hit && s.t < dist;
}
