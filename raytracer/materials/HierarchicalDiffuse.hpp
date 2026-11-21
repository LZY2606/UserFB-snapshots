#pragma once

#include "Material.hpp"

class HierarchicalRadiosity;

// Diffuse material driven by a HierarchicalRadiosity solution. On a front
// face hit of a registered triangle it returns the leaf's radiosity divided
// by pi; back faces are black. Ambient and point lights are not added.
// Radiosity values are not clamped to [0, 1]. The radiosity object and the
// World must outlive the material.
class HierarchicalDiffuse : public Material {
public:
    HierarchicalDiffuse();
    explicit HierarchicalDiffuse(const HierarchicalRadiosity *solution);

    const HierarchicalRadiosity *solution() const;
    void set_solution(const HierarchicalRadiosity *solution);

    RGBColor shade(const ShadeInfo &sinfo) const override;

private:
    const HierarchicalRadiosity *radiosity_solution;
};
