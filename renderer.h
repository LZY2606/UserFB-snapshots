#pragma once
// Public C++ interfaces for the tracer: math primitives, scene description,
// ray/intersection helpers, and the recursive ray caster. Both the upstream
// demo (tinyraytracer.cpp) and tests/fog_cases.cpp use these definitions, so
// the checks exercise the very same scene representation and recursion.
#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

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

struct Scene {
    std::vector<Sphere> spheres;
    std::vector<vec3>   lights;
    // RGB point-light power, one entry per light. Missing entries default to
    // unit power so the original demo lighting is unchanged.
    std::vector<vec3>   light_powers;
    vec3 background = {0.2, 0.7, 0.8};
    bool has_floor = true;          // the original checkered y=-4 plane
};

constexpr Material      ivory = {1.0, {0.9,  0.5, 0.1, 0.0}, {0.4, 0.4, 0.3},   50.};
constexpr Material      glass = {1.5, {0.0,  0.9, 0.1, 0.8}, {0.6, 0.7, 0.8},  125.};
constexpr Material red_rubber = {1.0, {1.4,  0.3, 0.0, 0.0}, {0.3, 0.1, 0.1},   10.};
constexpr Material     mirror = {1.0, {0.0, 16.0, 0.8, 0.0}, {1.0, 1.0, 1.0}, 1425.};

// The original demo's scene arrays, kept as globals for back-compatibility with
// older call sites (e.g. support/baseline.cpp) that index `spheres` directly.
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

// The original demo scene, unchanged; used whenever no scene is supplied.
inline Scene default_scene() {
    Scene s;
    s.spheres.assign(std::begin(spheres), std::end(spheres));
    s.lights.assign(std::begin(lights), std::end(lights));
    s.light_powers.assign(s.lights.size(), vec3{1, 1, 1});
    s.background = {0.2, 0.7, 0.8};
    s.has_floor = true;
    return s;
}

// Shared singleton for the zero-cost old-signature overloads.
inline const Scene &default_scene_ref() {
    static const Scene scene = default_scene();
    return scene;
}

inline vec3 reflect(const vec3 &I, const vec3 &N) {
    return I - N*2.f*(I*N);
}

inline vec3 refract(const vec3 &I, const vec3 &N, const float eta_t, const float eta_i=1.f) { // Snell's law
    float cosi = - std::max(-1.f, std::min(1.f, I*N));
    if (cosi<0) return refract(I, -N, eta_i, eta_t); // if the ray comes from the inside the object, swap the air and the media
    float eta = eta_i / eta_t;
    float k = 1 - eta*eta*(1 - cosi*cosi);
    return k<0 ? vec3{1,0,0} : I*eta + N*(eta*cosi - std::sqrt(k)); // k<0 = total reflection, no ray to refract. I refract it anyways, this has no physical meaning
}

inline std::tuple<bool,float> ray_sphere_intersect(const vec3 &orig, const vec3 &dir, const Sphere &s) { // ret value is a pair [intersection found, distance]
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

inline std::tuple<bool,vec3,vec3,Material> scene_intersect(const vec3 &orig, const vec3 &dir, const Scene &scene) {
    vec3 pt, N;
    Material material;

    float nearest_dist = 1e10;
    if (scene.has_floor && std::abs(dir.y)>.001) { // intersect the ray with the checkerboard, avoid division by zero
        float d = -(orig.y+4)/dir.y; // the checkerboard plane has equation y = -4
        vec3 p = orig + dir*d;
        if (d>.001 && d<nearest_dist && std::abs(p.x)<10 && p.z<-10 && p.z>-30) {
            nearest_dist = d;
            pt = p;
            N = {0,1,0};
            material.diffuse_color = (int(.5*pt.x+1000) + int(.5*pt.z)) & 1 ? vec3{.3, .3, .3} : vec3{.3, .2, .1};
        }
    }

    for (const Sphere &s : scene.spheres) { // intersect the ray with all spheres
        auto [intersection, d] = ray_sphere_intersect(orig, dir, s);
        if (!intersection || d > nearest_dist) continue;
        nearest_dist = d;
        pt = orig + dir*nearest_dist;
        N = (pt - s.center).normalized();
        material = s.material;
    }
    return { nearest_dist<1000, pt, N, material };
}

// Back-compatible overload: the original demo scene.
inline std::tuple<bool,vec3,vec3,Material> scene_intersect(const vec3 &orig, const vec3 &dir) {
    return scene_intersect(orig, dir, default_scene_ref());
}

// Fog strategy used by the recursive caster. The no-medium policy reproduces
// the original renderer exactly (and is the only one compiled without a grid).
struct NoMedium {
    bool active() const { return false; }
    vec3 apply(const vec3 &, const vec3 &, double, const vec3 &surface) const { return surface; }
    float light_transmittance(const vec3 &, const vec3 &) const { return 1.f; }
    double exit_distance(const vec3 &, const vec3 &) const { return 0.0; }
};

// Recursive caster shared by every medium policy. Each call handles exactly its
// own camera-to-surface (or camera-to-background) segment: reflection and
// refraction recurse, and each recursive call attenuates its own segment, so
// no segment is counted twice and no result is globally scaled by camera depth.
template <class Fog>
inline vec3 cast_ray_impl(const vec3 &orig, const vec3 &dir, const int depth,
                          const Scene &scene, const Fog &fog) {
    auto [hit, point, N, material] = scene_intersect(orig, dir, scene);

    if (!fog.active()) { // unchanged original path
        if (depth>4 || !hit)
            return scene.background;
        vec3 reflect_dir = reflect(dir, N).normalized();
        vec3 refract_dir = refract(dir, N, material.refractive_index).normalized();
        vec3 reflect_color = cast_ray_impl(point, reflect_dir, depth + 1, scene, fog);
        vec3 refract_color = cast_ray_impl(point, refract_dir, depth + 1, scene, fog);

        float diffuse_light_intensity = 0, specular_light_intensity = 0;
        for (const vec3 &light : scene.lights) {
            vec3 light_dir = (light - point).normalized();
            auto [shadow_hit, shadow_pt, trashnrm, trashmat] = scene_intersect(point, light_dir, scene);
            if (shadow_hit && (shadow_pt-point).norm() < (light-point).norm()) continue;
            diffuse_light_intensity  += std::max(0.f, light_dir*N);
            specular_light_intensity += std::pow(std::max(0.f, -reflect(-light_dir, N)*dir), material.specular_exponent);
        }
        return material.diffuse_color * diffuse_light_intensity * material.albedo[0]
             + vec3{1., 1., 1.}*specular_light_intensity * material.albedo[1]
             + reflect_color*material.albedo[2] + refract_color*material.albedo[3];
    }

    // The view segment always stops at the nearest surface; when nothing is
    // hit it stops where the ray leaves the medium.
    const double t_end = hit ? double((point-orig).norm()) : fog.exit_distance(orig, dir);

    if (depth>4 || !hit) {
        // Background radiance, attenuated along the camera's own segment,
        // plus the in-scattered light of that same segment.
        return fog.apply(orig, dir, t_end, scene.background);
    }

    vec3 reflect_dir = reflect(dir, N).normalized();
    vec3 refract_dir = refract(dir, N, material.refractive_index).normalized();
    // Each recursion starts at the surface and handles its own fog segment.
    vec3 reflect_color = cast_ray_impl(point, reflect_dir, depth + 1, scene, fog);
    vec3 refract_color = cast_ray_impl(point, refract_dir, depth + 1, scene, fog);

    float diffuse_light_intensity = 0, specular_light_intensity = 0;
    for (const vec3 &light : scene.lights) {
        vec3 light_dir = (light - point).normalized();
        auto [shadow_hit, shadow_pt, trashnrm, trashmat] = scene_intersect(point, light_dir, scene);
        if (shadow_hit && (shadow_pt-point).norm() < (light-point).norm()) continue;
        // Transmission along this unoccluded surface-to-light segment only;
        // it never spawns further scattering.
        const float tl = fog.light_transmittance(point, light);
        diffuse_light_intensity  += std::max(0.f, light_dir*N) * tl;
        specular_light_intensity += std::pow(std::max(0.f, -reflect(-light_dir, N)*dir), material.specular_exponent) * tl;
    }
    vec3 surface = material.diffuse_color * diffuse_light_intensity * material.albedo[0]
                 + vec3{1., 1., 1.}*specular_light_intensity * material.albedo[1]
                 + reflect_color*material.albedo[2] + refract_color*material.albedo[3];
    return fog.apply(orig, dir, t_end, surface);
}

// Original call sites: no scene/medium arguments, unchanged output.
inline vec3 cast_ray(const vec3 &orig, const vec3 &dir, const int depth=0) {
    return cast_ray_impl(orig, dir, depth, default_scene_ref(), NoMedium{});
}
inline vec3 cast_ray(const vec3 &orig, const vec3 &dir, const int depth, const Scene &scene) {
    return cast_ray_impl(orig, dir, depth, scene, NoMedium{});
}
