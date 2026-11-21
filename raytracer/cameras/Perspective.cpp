#include "Perspective.hpp"
#include "../utilities/Point3D.hpp"
#include "../utilities/Vector3D.hpp"

Perspective::Perspective() : pos() {}
Perspective::Perspective(float c) : pos(c, c, c) {}
Perspective::Perspective(float x, float y, float z) : pos(x, y, z) {}
Perspective::Perspective(const Point3D &pt) : pos(pt) {}

Perspective::Perspective(const Perspective &camera) : Camera(camera), pos(camera.pos) {}

Perspective &Perspective::operator=(const Perspective &other) {
    if (this != &other) { Camera::operator=(other); pos = other.pos; }
    return *this;
}

Vector3D Perspective::get_direction(const Point3D &p) const {
    Vector3D d = p - pos;
    d.normalize();
    return d;
}
