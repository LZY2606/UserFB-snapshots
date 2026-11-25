#ifndef RENDERER_H
#define RENDERER_H

#include <tuple>
#include <vector>
#include <cmath>

// ---------------------------------------------------------------------------
// Vector math
// ---------------------------------------------------------------------------
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

vec3 cross(const vec3 v1, const vec3 v2);

// ---------------------------------------------------------------------------
// Materials and the original scene primitives
// ---------------------------------------------------------------------------
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

constexpr Material      ivory = {1.0, {0.9,  0.5, 0.1, 0.0}, {0.4, 0.4, 0.3},   50.};
constexpr Material      glass = {1.5, {0.0,  0.9, 0.1, 0.8}, {0.6, 0.7, 0.8},  125.};
constexpr Material red_rubber = {1.0, {1.4,  0.3, 0.0, 0.0}, {0.3, 0.1, 0.1},   10.};
constexpr Material     mirror = {1.0, {0.0, 16.0, 0.8, 0.0}, {1.0, 1.0, 1.0}, 1425.};

constexpr Sphere spheres[] = {
    {{-3,    0,   -16}, 2,      ivory},
    {{-1.0, -1.5, -12}, 2,      glass},
    {{ 1.5, -0.5, -18}, 3, red_rubber},
    {{ 7,    5,   -18}, 4,     mirror}
};

constexpr vec3 lights[] = {
    {-20, 20,  20},
    { 30, 50, -25},
    { 30, 20,  30}
};

vec3 reflect(const vec3 &I, const vec3 &N);
vec3 refract(const vec3 &I, const vec3 &N, const float eta_t, const float eta_i=1.f);

// Original sphere intersection; dir is assumed to be a unit direction.
std::tuple<bool,float> ray_sphere_intersect(const vec3 &orig, const vec3 &dir, const Sphere &s);

// ---------------------------------------------------------------------------
// IsoGrid: read-only isosurface inside a regular scalar vertex grid.
// Defined in isosurface.h/.cpp.
// ---------------------------------------------------------------------------
struct IsoGrid;
struct IsoHit {
    float t = 0;                 // ray parameter of the hit
    vec3  point = {0,0,0};       // world-space hit position
    vec3  normal = {0,0,0};      // unit normal, pointing toward increasing scalar
    int   cell[3] = {0,0,0};     // 3D index of the owning cell
    Material material;
};
struct QueryStats {
    unsigned long long bricks_visited = 0; // bricks entered by the ray walk
    unsigned long long cells_solved   = 0; // cells that ran polynomial root finding
};

// ---------------------------------------------------------------------------
// Scene: the original spheres/lights plus one optional read-only isosurface.
// The checkerboard floor and the sky background stay implicit, as before.
// ---------------------------------------------------------------------------
struct Scene {
    std::vector<Sphere> scene_spheres;
    std::vector<vec3>   scene_lights;
    const IsoGrid*      iso = nullptr; // optional, not owned
};

const Scene& default_scene();

// Original entry points: behave exactly as in the standalone tinyraytracer.
std::tuple<bool,vec3,vec3,Material> scene_intersect(const vec3 &orig, const vec3 &dir);
vec3 cast_ray(const vec3 &orig, const vec3 &dir, const int depth=0);

// Scene-aware entry points: the isosurface competes on the same nearest-hit
// chain with the spheres and the floor; reflection/refraction recurse with
// the same Scene.
std::tuple<bool,vec3,vec3,Material> scene_intersect(const vec3 &orig, const vec3 &dir, const Scene &scene);
vec3 cast_ray(const vec3 &orig, const vec3 &dir, const Scene &scene, const int depth=0);

#endif // RENDERER_H
