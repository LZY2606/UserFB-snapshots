#pragma once

#include <algorithm>
#include <cmath>
#include <tuple>

// Shared geometry and common scene types for the legacy renderer and the
// polarization-only reflection path.  Keeping the intersection routine here
// means both paths use exactly the same sphere test.
struct vec3 {
    float x = 0, y = 0, z = 0;

    float& operator[](const int i)       { return i == 0 ? x : (1 == i ? y : z); }
    const float& operator[](const int i) const { return i == 0 ? x : (1 == i ? y : z); }

    vec3 operator*(const float v) const { return {x * v, y * v, z * v}; }
    float operator*(const vec3& v) const { return x * v.x + y * v.y + z * v.z; }
    vec3 operator+(const vec3& v) const { return {x + v.x, y + v.y, z + v.z}; }
    vec3 operator-(const vec3& v) const { return {x - v.x, y - v.y, z - v.z}; }
    vec3 operator-() const { return {-x, -y, -z}; }

    float norm() const { return std::sqrt(x * x + y * y + z * z); }
    vec3 normalized() const { return (*this) * (1.f / norm()); }
};

inline vec3 operator*(float v, const vec3& a) { return a * v; }

inline vec3 cross(const vec3& v1, const vec3& v2) {
    return {
        v1.y * v2.z - v1.z * v2.y,
        v1.z * v2.x - v1.x * v2.z,
        v1.x * v2.y - v1.y * v2.x
    };
}

struct Material {
    float refractive_index = 1;
    float albedo[4] = {2, 0, 0, 0};
    vec3 diffuse_color = {0, 0, 0};
    float specular_exponent = 0;
};

struct Sphere {
    vec3 center;
    float radius;
    Material material;
};

inline vec3 reflect(const vec3& I, const vec3& N) {
    return I - N * 2.f * (I * N);
}

inline vec3 refract(const vec3& I, const vec3& N,
                    const float eta_t, const float eta_i = 1.f) {
    float cosi = -std::max(-1.f, std::min(1.f, I * N));
    if (cosi < 0) return refract(I, -N, eta_i, eta_t);
    float eta = eta_i / eta_t;
    float k = 1 - eta * eta * (1 - cosi * cosi);
    return k < 0 ? vec3{1, 0, 0}
                 : I * eta + N * (eta * cosi - std::sqrt(k));
}

// The original quadratic sphere test.  t > 0.001 supplies the self-shadow /
// self-intersection offset used by TinyRayTracer.
inline std::tuple<bool, float> ray_sphere_intersect(
        const vec3& orig, const vec3& dir,
        const vec3& center, const float radius) {
    vec3 L = center - orig;
    float tca = L * dir;
    float d2 = L * L - tca * tca;
    if (d2 > radius * radius) return {false, 0};

    float thc = std::sqrt(std::max(0.f, radius * radius - d2));
    float t0 = tca - thc;
    float t1 = tca + thc;
    if (t0 > .001f) return {true, t0};
    if (t1 > .001f) return {true, t1};
    return {false, 0};
}

inline std::tuple<bool, float> ray_sphere_intersect(
        const vec3& orig, const vec3& dir, const Sphere& s) {
    return ray_sphere_intersect(orig, dir, s.center, s.radius);
}
