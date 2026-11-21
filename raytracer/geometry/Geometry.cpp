#include "Geometry.hpp"
#include "../materials/Material.hpp"

Geometry::Geometry() : material_ptr(nullptr) {}

Material *Geometry::get_material() const {
    return material_ptr;
}

void Geometry::set_material(Material *mPtr) {
    material_ptr = mPtr;
}
