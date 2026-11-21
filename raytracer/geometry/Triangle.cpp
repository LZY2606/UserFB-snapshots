#include "Triangle.hpp"
#include "../utilities/BBox.hpp"
#include "../utilities/Constants.hpp"
#include "../utilities/Ray.hpp"
#include "../utilities/ShadeInfo.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

Triangle::Triangle() : v0(), v1(), v2() {}
Triangle::Triangle(const Point3D &a, const Point3D &b, const Point3D &c)
    : v0(a), v1(b), v2(c) {}

Triangle::Triangle(const Triangle &obj)
    : Geometry(obj), v0(obj.v0), v1(obj.v1), v2(obj.v2) {}

Triangle &Triangle::operator=(const Triangle &rhs) {
    if (this != &rhs) {
        Geometry::operator=(rhs);
        v0 = rhs.v0; v1 = rhs.v1; v2 = rhs.v2;
    }
    return *this;
}

std::string Triangle::to_string() const {
    std::ostringstream oss;
    oss << "Triangle(" << v0.to_string() << ", " << v1.to_string() << ", " << v2.to_string() << ")";
    return oss.str();
}

bool Triangle::hit(const Ray &ray, float &t, ShadeInfo &s) const {
    // Möller–Trumbore
    Vector3D e1 = v1 - v0;
    Vector3D e2 = v2 - v0;
    Vector3D h = ray.d ^ e2;
    double det = e1 * h;

    if (std::abs(det) < kEpsilon) return false;

    double inv_det = 1.0 / det;
    Vector3D tv = ray.o - v0;
    double u = (tv * h) * inv_det;
    if (u < 0.0 || u > 1.0) return false;

    Vector3D q = tv ^ e1;
    double v = (ray.d * q) * inv_det;
    if (v < 0.0 || u + v > 1.0) return false;

    double t_val = (e2 * q) * inv_det;
    if (t_val < kEpsilon) return false;

    t = (float)t_val;
    s.hit = true;
    s.t = t;
    s.hit_point = ray.o + ray.d * t;
    s.normal = e1 ^ e2;
    s.normal.normalize();
    s.ray = ray;
    s.material_ptr = material_ptr;
    return true;
}

BBox Triangle::getBBox() const {
    Point3D mn = min(min(v0, v1), v2);
    Point3D mx = max(max(v0, v1), v2);
    return BBox(mn, mx);
}
