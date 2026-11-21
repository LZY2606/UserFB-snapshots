#include "Light.hpp"
#include "../utilities/ShadeInfo.hpp"
#include "../utilities/Constants.hpp"
#include "../world/World.hpp"

Light::Light() : shadows(true) {}

bool Light::in_shadow(const Ray &ray, const ShadeInfo &si) const {
    ShadeInfo test = const_cast<World*>(si.w)->hit_objects(ray);
    return test.hit;
}
