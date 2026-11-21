#include "Plane.hpp"
#include "../utilities/BBox.hpp"
#include "../utilities/Constants.hpp"
#include "../utilities/Ray.hpp"
#include "../utilities/ShadeInfo.hpp"

#include <cmath>
#include <sstream>

Plane::Plane() : a(), n(0, 1, 0) {}
Plane::Plane(const Point3D &pt, const Vector3D &normal) : a(pt), n(normal) {
    n.normalize();
}

Plane::Plane(const Plane &obj) : Geometry(obj), a(obj.a), n(obj.n) {}

Plane &Plane::operator=(const Plane &rhs) {
    if (this != &rhs) { Geometry::operator=(rhs); a = rhs.a; n = rhs.n; }
    return *this;
}

std::string Plane::to_string() const {
    std::ostringstream oss;
    oss << "Plane(a=" << a.to_string() << ", n=" << n.to_string() << ")";
    return oss.str();
}

bool Plane::hit(const Ray &ray, float &t, ShadeInfo &s) const {
    // t = (a - o) . n / (d . n)
    double denom = ray.d * n;
    if (std::abs(denom) < kEpsilon) return false;

    Vector3D ao = a - ray.o;
    double t_val = (ao * n) / denom;
    if (t_val < kEpsilon) return false;

    t = (float)t_val;
    s.hit = true;
    s.t = t;
    s.hit_point = ray.o + ray.d * t;
    s.normal = n;
    s.ray = ray;
    s.material_ptr = material_ptr;
    s.hit_geometry = this;
    s.bary_u = 0.0;
    s.bary_v = 0.0;
    s.front_face = (n * (-ray.d)) > 0.0;
    return true;
}

bool Plane::hit_segment(const Point3D &O, const Point3D &Q,
                        double s_lo, double s_hi, double &s_hit) const {
    Vector3D seg(Q - O);
    double denom = seg * n;
    if (std::abs(denom) < 1e-18) return false;

    double s = ((a - O) * n) / denom;
    if (s <= s_lo || s >= s_hi) return false;
    s_hit = s;
    return true;
}

BBox Plane::getBBox() const {
    // Planes are infinite; return a large bbox
    float huge = 1e6f;
    return BBox(Point3D(-huge, -huge, -huge), Point3D(huge, huge, huge));
}
