#pragma once
// Shared geometry and common scene types.
// The scalar ray tracer (tinyraytracer.cpp) and the double-precision
// polarization path share the same sphere intersection code.

#include <tuple>
#include <cmath>

struct vec3 {
    float x=0, y=0, z=0;
          float& operator[](const int i)       { return i==0 ? x : (1==i ? y : z); }
    const float& operator[](const int i) const { return i==0 ? x : (1==i ? y : z); }
    vec3  operator*(const float v) const { return {x*v, y*v, z*v};       }
    float operator*(const vec3& v) const { return x*v.x + y*v.y + z*v.z; }
    vec3  operator+(const vec3& v) const { return {x+v.x, y+v.y, z+v.z}; }
    vec3  operator-(const vec3& v) const { return {x-v.x, y-v.y, z-v.z}; }
    vec3  operator-()              const { return {-x, -y, -z};          }
    float norm() const { return std::sqrt(x*x+y*y+z*z); }
    vec3 normalized() const { return (*this)*(1.f/norm()); }
};

inline vec3 cross(const vec3 v1, const vec3 v2) {
    return { v1.y*v2.z - v1.z*v2.y, v1.z*v2.x - v1.x*v2.z, v1.x*v2.y - v1.y*v2.x };
}

inline vec3 reflect(const vec3 &I, const vec3 &N) {
    return I - N*2.f*(I*N);
}

// Double precision vectors used by the polarization path.
struct dvec3 {
    double x=0, y=0, z=0;
          double& operator[](const int i)       { return i==0 ? x : (1==i ? y : z); }
    const double& operator[](const int i) const { return i==0 ? x : (1==i ? y : z); }
    dvec3  operator*(const double v) const { return {x*v, y*v, z*v};          }
    double operator*(const dvec3& v) const { return x*v.x + y*v.y + z*v.z;    }
    dvec3  operator+(const dvec3& v) const { return {x+v.x, y+v.y, z+v.z};   }
    dvec3  operator-(const dvec3& v) const { return {x-v.x, y-v.y, z-v.z};   }
    dvec3  operator-()               const { return {-x, -y, -z};             }
    double norm() const { return std::sqrt(x*x+y*y+z*z); }
    dvec3 normalized() const { return (*this)*(1.0/norm()); }
};

inline dvec3 cross(const dvec3& v1, const dvec3& v2) {
    return { v1.y*v2.z - v1.z*v2.y, v1.z*v2.x - v1.x*v2.z, v1.x*v2.y - v1.y*v2.x };
}

inline dvec3 reflect(const dvec3& I, const dvec3& N) {
    return I - N*2.0*(I*N);
}

inline dvec3 to_dvec3(const vec3& v) { return {v.x, v.y, v.z}; }
inline vec3  to_vec3 (const dvec3& v) { return {float(v.x), float(v.y), float(v.z)}; }

struct Material {
    float refractive_index = 1;
    float albedo[4] = {2,0,0,0};
    vec3 diffuse_color = {0,0,0};
    float specular_exponent = 0;
};

struct Sphere {
    vec3 center;
    float radius;
    Material material;
};

// The single sphere intersection implementation, shared by both renderers.
inline std::tuple<bool,float> ray_sphere_intersect(const vec3 &orig, const vec3 &dir, const Sphere &s) {
    vec3 L = s.center - orig;
    float tca = L*dir;
    float d2 = L*L - tca*tca;
    if (d2 > s.radius*s.radius) return {false, 0};
    float thc = std::sqrt(s.radius*s.radius - d2);
    float t0 = tca-thc, t1 = tca+thc;
    if (t0>.001) return {true, t0};  // offset the original point by .001 to avoid occlusion by the object itself
    if (t1>.001) return {true, t1};
    return {false, 0};
}
