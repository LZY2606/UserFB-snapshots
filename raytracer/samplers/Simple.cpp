#include "Simple.hpp"

#include "../cameras/Camera.hpp"
#include "../utilities/Point3D.hpp"
#include "../utilities/Ray.hpp"
#include "../utilities/Vector3D.hpp"
#include "../world/ViewPlane.hpp"

Simple::Simple(Camera *c_ptr, ViewPlane *v_ptr) : Sampler(c_ptr, v_ptr) {}

Simple::Simple(const Simple &other) : Sampler(other) {}

Simple &Simple::operator=(const Simple &other) {
    if (this != &other) Sampler::operator=(other);
    return *this;
}

std::vector<Ray> Simple::get_rays(int px, int py) const {
    // Map pixel (px, py) to a point on the view plane.
    // px goes right, py goes down (origin top-left).
    // We interpolate between top_left and bottom_right.
    float u = (px + 0.5f) / viewplane_ptr->hres;
    float v = (py + 0.5f) / viewplane_ptr->vres;

    // Interpolate x and y using the viewplane corners.
    float x = viewplane_ptr->top_left.x + u * (viewplane_ptr->bottom_right.x - viewplane_ptr->top_left.x);
    float y = viewplane_ptr->top_left.y + v * (viewplane_ptr->bottom_right.y - viewplane_ptr->top_left.y);
    float z = viewplane_ptr->top_left.z; // view plane z

    Point3D pixel_point(x, y, z);
    Vector3D dir = camera_ptr->get_direction(pixel_point);

    // For parallel cameras: direction is constant, pixel_point as origin is correct.
    // For perspective: get_direction returns normalize(pixel - pos), pointing from
    // COP through the pixel into the scene. Since the view plane sits between the
    // camera and the scene, pixel_point + t*dir correctly traces into the scene.
    Ray ray(pixel_point, dir);
    ray.w = 1.0f;
    return { ray };
}
