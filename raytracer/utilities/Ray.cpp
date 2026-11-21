#include "Ray.hpp"

#include <sstream>

Ray::Ray() : o(), d(), w(1.0f) {}
Ray::Ray(const Point3D &origin, const Vector3D &dir) : o(origin), d(dir), w(1.0f) {}

std::string Ray::to_string() const {
    std::ostringstream oss;
    oss << "Ray(o=" << o.to_string() << ", d=" << d.to_string() << ", w=" << w << ")";
    return oss.str();
}
