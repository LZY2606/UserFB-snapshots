#pragma once

#include "Sampler.hpp"

class Jittered : public Sampler {
    int num_samples;

public:
    Jittered() = default;
    Jittered(Camera *c_ptr, ViewPlane *v_ptr, int n);

    Jittered(const Jittered &other);
    Jittered &operator=(const Jittered &other);

    virtual ~Jittered() = default;

    std::vector<Ray> get_rays(int px, int py) const override;
};
