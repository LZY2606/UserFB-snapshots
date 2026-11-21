#include "Sphere.hpp"
#include "../utilities/BBox.hpp"
#include "../utilities/Constants.hpp"
#include "../utilities/Ray.hpp"
#include "../utilities/ShadeInfo.hpp"

#include <cmath>
#include <sstream>

Sphere::Sphere() : c(), r(0) {}
Sphere::Sphere(const Point3D &center, float radius) : c(center), r(radius) {}
Sphere::Sphere(const Sphere &obj) : Geometry(obj), c(obj.c), r(obj.r) {}

Sphere &Sphere::operator=(const Sphere &rhs) {
    if (this != &rhs) { Geometry::operator=(rhs); c = rhs.c; r = rhs.r; }
    return *this;
}

std::string Sphere::to_string() const {
    std::ostringstream oss;
    oss << "Sphere(c=" << c.to_string() << ", r=" << r << ")";
    return oss.str();
}

bool Sphere::hit(const Ray &ray, float &t, ShadeInfo &s) const {
    // Solve quadratic: |o + t*d - c|^2 = r^2
    Vector3D oc = ray.o - c;  // vector from center to ray origin
    double a = ray.d * ray.d;
    double b = 2.0 * (oc * ray.d);
    double c_coef = oc * oc - r * r;
    double disc = b*b - 4.0*a*c_coef;

    if (disc < 0) return false;

    double sqrt_disc = std::sqrt(disc);
    double t0 = (-b - sqrt_disc) / (2.0 * a);
    double t1 = (-b + sqrt_disc) / (2.0 * a);

    float hit_t = -1;
    if (t0 > kEpsilon) hit_t = (float)t0;
    else if (t1 > kEpsilon) hit_t = (float)t1;
    else return false;

    t = hit_t;
    s.hit = true;
    s.t = t;
    s.hit_point = ray.o + ray.d * t;
    s.normal = Vector3D(s.hit_point - this->c);
    s.normal.normalize();
    s.ray = ray;
    s.material_ptr = material_ptr;
    return true;
}

BBox Sphere::getBBox() const {
    return BBox(Point3D(c.x - r, c.y - r, c.z - r),
                Point3D(c.x + r, c.y + r, c.z + r));
}
