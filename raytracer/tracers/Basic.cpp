#include "Basic.hpp"
#include "../utilities/Ray.hpp"
#include "../utilities/ShadeInfo.hpp"
#include "../materials/Material.hpp"
#include "../world/World.hpp"

Basic::Basic() : Tracer() {}
Basic::Basic(World *w) : Tracer(w) {}

RGBColor Basic::trace_ray(const Ray &ray, int depth) const {
    ShadeInfo si = world_ptr->hit_objects(ray);
    if (si.hit) return si.material_ptr->shade(si);
    return world_ptr->bg_color;
}
