#include "Whitted.hpp"
#include "../utilities/Ray.hpp"
#include "../utilities/ShadeInfo.hpp"
#include "../utilities/Constants.hpp"
#include "../materials/Material.hpp"
#include "../world/World.hpp"

Whitted::Whitted() : Tracer(), max_depth(5) {}
Whitted::Whitted(World *w, int d) : Tracer(w), max_depth(d) {}

RGBColor Whitted::trace_ray(const Ray &ray, int depth) const {
    if (depth > max_depth) return black;
    ShadeInfo si = world_ptr->hit_objects(ray);
    if (si.hit) {
        si.depth = depth;
        return si.material_ptr->shade(si);
    }
    return world_ptr->bg_color;
}
