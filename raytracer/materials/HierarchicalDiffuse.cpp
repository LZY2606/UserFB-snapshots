#include "HierarchicalDiffuse.hpp"

#include "../geometry/Triangle.hpp"
#include "../lighting/HierarchicalRadiosity.hpp"
#include "../utilities/Constants.hpp"
#include "../utilities/ShadeInfo.hpp"

HierarchicalDiffuse::HierarchicalDiffuse()
    : Material(), radiosity_solution(nullptr) {}

HierarchicalDiffuse::HierarchicalDiffuse(
        const HierarchicalRadiosity *solution)
    : Material(), radiosity_solution(solution) {}

const HierarchicalRadiosity *HierarchicalDiffuse::solution() const {
    return radiosity_solution;
}

void HierarchicalDiffuse::set_solution(
        const HierarchicalRadiosity *solution) {
    radiosity_solution = solution;
}

RGBColor HierarchicalDiffuse::shade(const ShadeInfo &sinfo) const {
    if (!radiosity_solution || !radiosity_solution->ok() ||
        !sinfo.hit || !sinfo.front_face)
        return RGBColor(0.0f, 0.0f, 0.0f);

    const auto *tri = dynamic_cast<const Triangle *>(sinfo.hit_geometry);
    if (!tri) return RGBColor(0.0f, 0.0f, 0.0f);

    int leaf = radiosity_solution->leaf_for_hit(tri, sinfo.bary_u,
                                                 sinfo.bary_v);
    if (leaf < 0) return RGBColor(0.0f, 0.0f, 0.0f);

    double r = 0.0, g = 0.0, b = 0.0;
    radiosity_solution->leaf_radiosity((std::size_t)leaf, r, g, b);
    // Floating-point radiosity energy is preserved: values are not clamped
    // into [0, 1].
    return RGBColor((float)(r / PI), (float)(g / PI), (float)(b / PI));
}
