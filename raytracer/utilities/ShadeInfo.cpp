#include "ShadeInfo.hpp"
#include "../world/World.hpp"

ShadeInfo::ShadeInfo(const World &wr)
    : hit(false), material_ptr(nullptr), hit_point(), normal(), ray(), depth(0), t(0), w(&wr),
      hit_geometry(nullptr), bary_u(0.0), bary_v(0.0), front_face(false) {}
