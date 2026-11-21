#include "Jittered.hpp"
#include "../cameras/Camera.hpp"
#include "../utilities/Point3D.hpp"
#include "../utilities/Ray.hpp"
#include "../utilities/Vector3D.hpp"
#include "../world/ViewPlane.hpp"
#include <random>

Jittered::Jittered(Camera *c, ViewPlane *v, int n)
    : Sampler(c, v), num_samples(n) {}

Jittered::Jittered(const Jittered &o) : Sampler(o), num_samples(o.num_samples) {}

Jittered &Jittered::operator=(const Jittered &o) {
    if (this != &o) { Sampler::operator=(o); num_samples = o.num_samples; }
    return *this;
}

static thread_local std::mt19937 rng(42);
static thread_local std::uniform_real_distribution<float> dist(0.0f, 1.0f);

std::vector<Ray> Jittered::get_rays(int px, int py) const {
    std::vector<Ray> rays;
    float w = 1.0f / (num_samples * num_samples);

    for (int i = 0; i < num_samples; i++) {
        for (int j = 0; j < num_samples; j++) {
            float jx = (i + dist(rng)) / num_samples;
            float jy = (j + dist(rng)) / num_samples;

            float u = (px + jx) / viewplane_ptr->hres;
            float v = (py + jy) / viewplane_ptr->vres;

            float x = viewplane_ptr->top_left.x + u * (viewplane_ptr->bottom_right.x - viewplane_ptr->top_left.x);
            float y = viewplane_ptr->top_left.y + v * (viewplane_ptr->bottom_right.y - viewplane_ptr->top_left.y);

            Point3D pt(x, y, viewplane_ptr->top_left.z);
            Vector3D dir = camera_ptr->get_direction(pt);

            Ray ray(pt, dir);
            ray.w = w;
            rays.push_back(ray);
        }
    }
    return rays;
}
