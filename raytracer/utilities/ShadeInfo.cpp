#include "ShadeInfo.hpp"
#include "../world/World.hpp"

ShadeInfo::ShadeInfo(const World &wr)
    : hit(false), material_ptr(nullptr), hit_point(), normal(), ray(), depth(0), t(0), w(&wr) {}
