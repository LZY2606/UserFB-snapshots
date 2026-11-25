#include "renderer.h"
#include "isosurface.h"

#include <algorithm>
#include <cmath>

vec3 cross(const vec3 v1, const vec3 v2) {
    return { v1.y*v2.z - v1.z*v2.y, v1.z*v2.x - v1.x*v2.z, v1.x*v2.y - v1.y*v2.x };
}

vec3 reflect(const vec3 &I, const vec3 &N) {
    return I - N*2.f*(I*N);
}

vec3 refract(const vec3 &I, const vec3 &N, const float eta_t, const float eta_i) { // Snell's law
    float cosi = - std::max(-1.f, std::min(1.f, I*N));
    if (cosi<0) return refract(I, -N, eta_i, eta_t); // if the ray comes from the inside the object, swap the air and the media
    float eta = eta_i / eta_t;
    float k = 1 - eta*eta*(1 - cosi*cosi);
    return k<0 ? vec3{1,0,0} : I*eta + N*(eta*cosi - std::sqrt(k)); // k<0 = total reflection, no ray to refract. I refract it anyways, this has no physical meaning
}

std::tuple<bool,float> ray_sphere_intersect(const vec3 &orig, const vec3 &dir, const Sphere &s) { // ret value is a pair [intersection found, distance]
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

// ---------------------------------------------------------------------------
// Scene construction
// ---------------------------------------------------------------------------
const Scene& default_scene() {
    static const Scene scene = [] {
        Scene s;
        s.scene_spheres.assign(std::begin(spheres), std::end(spheres));
        s.scene_lights.assign(std::begin(lights), std::end(lights));
        s.iso = nullptr;
        return s;
    }();
    return scene;
}

// Checkerboard floor hit, shared by the intersection and shadow routines.
static bool floor_hit(const vec3 &orig, const vec3 &dir, float tmax,
                      vec3 &pt, vec3 &N, Material &material) {
    if (std::abs(dir.y) <= .001) return false; // avoid division by zero
    float d = -(orig.y+4)/dir.y;                // the checkerboard plane has equation y = -4
    vec3 p = orig + dir*d;
    if (d>.001 && d<tmax && std::abs(p.x)<10 && p.z<-10 && p.z>-30) {
        pt = p;
        N = {0,1,0};
        material.diffuse_color = (int(.5*pt.x+1000) + int(.5*pt.z)) & 1 ?
            vec3{.3, .3, .3} : vec3{.3, .2, .1};
        return true;
    }
    return false;
}

// Original entry point: identical behavior to the standalone tinyraytracer.
std::tuple<bool,vec3,vec3,Material> scene_intersect(const vec3 &orig, const vec3 &dir) {
    return scene_intersect(orig, dir, default_scene());
}

// Scene-aware entry point: floor, spheres and the optional isosurface are
// compared on one nearest-hit chain.
std::tuple<bool,vec3,vec3,Material> scene_intersect(const vec3 &orig, const vec3 &dir,
                                                    const Scene &scene) {
    vec3 pt, N;
    Material material;

    float nearest_dist = 1e10;
    Material floor_material;
    vec3 floor_pt, floor_N;
    if (floor_hit(orig, dir, nearest_dist, floor_pt, floor_N, floor_material)) {
        nearest_dist = -(orig.y+4)/dir.y;
        pt = floor_pt;
        N = floor_N;
        material = floor_material;
    }

    for (const Sphere &s : scene.scene_spheres) { // intersect the ray with all spheres
        auto [intersection, d] = ray_sphere_intersect(orig, dir, s);
        if (!intersection || d > nearest_dist) continue;
        nearest_dist = d;
        pt = orig + dir*nearest_dist;
        N = (pt - s.center).normalized();
        material = s.material;
    }

    if (scene.iso) {
        // The grid query keeps dir unnormalized and is itself bounded by the
        // current nearest distance, so it can only replace the current hit.
        IsoHit iso_hit;
        if (scene.iso->intersect(orig, dir, .001f, nearest_dist, iso_hit)) {
            nearest_dist = iso_hit.t;
            pt = iso_hit.point;
            N = iso_hit.normal;
            material = iso_hit.material;
        }
    }

    return { nearest_dist<1000, pt, N, material };
}

// Finite shadow test: nothing beyond the light may cast a shadow.
static bool point_in_shadow(const vec3 &point, const vec3 &light_dir,
                            float light_dist, const Scene &scene) {
    vec3 trash_pt, trash_N;
    Material trash_m;
    if (floor_hit(point, light_dir, light_dist, trash_pt, trash_N, trash_m))
        return true;

    for (const Sphere &s : scene.scene_spheres) {
        auto [intersection, d] = ray_sphere_intersect(point, light_dir, s);
        if (intersection && d < light_dist) return true;
    }

    if (scene.iso &&
        scene.iso->occluded(point, light_dir, .001f, light_dist))
        return true;

    return false;
}

// Original recursive shading: preserved verbatim in behavior.
vec3 cast_ray(const vec3 &orig, const vec3 &dir, const int depth) {
    auto [hit, point, N, material] = scene_intersect(orig, dir);
    if (depth>4 || !hit)
        return {0.2, 0.7, 0.8}; // background color

    vec3 reflect_dir = reflect(dir, N).normalized();
    vec3 refract_dir = refract(dir, N, material.refractive_index).normalized();
    vec3 reflect_color = cast_ray(point, reflect_dir, depth + 1);
    vec3 refract_color = cast_ray(point, refract_dir, depth + 1);

    float diffuse_light_intensity = 0, specular_light_intensity = 0;
    for (const vec3 &light : lights) { // checking if the point lies in the shadow of the light
        vec3 light_dir = (light - point).normalized();
        auto [shadow_hit, shadow_pt, trashnrm, trashmat] = scene_intersect(point, light_dir);
        if (shadow_hit && (shadow_pt-point).norm() < (light-point).norm()) continue;
        diffuse_light_intensity  += std::max(0.f, light_dir*N);
        specular_light_intensity += std::pow(std::max(0.f, -reflect(-light_dir, N)*dir), material.specular_exponent);
    }
    return material.diffuse_color * diffuse_light_intensity * material.albedo[0] + vec3{1., 1., 1.}*specular_light_intensity * material.albedo[1] + reflect_color*material.albedo[2] + refract_color*material.albedo[3];
}

// Scene-aware recursive shading: reflection/refraction carry the same Scene,
// and shadow rays are bounded by the light distance.
vec3 cast_ray(const vec3 &orig, const vec3 &dir, const Scene &scene,
              const int depth) {
    auto [hit, point, N, material] = scene_intersect(orig, dir, scene);
    if (depth>4 || !hit)
        return {0.2, 0.7, 0.8}; // background color

    vec3 reflect_dir = reflect(dir, N).normalized();
    vec3 refract_dir = refract(dir, N, material.refractive_index).normalized();
    vec3 reflect_color = cast_ray(point, reflect_dir, scene, depth + 1);
    vec3 refract_color = cast_ray(point, refract_dir, scene, depth + 1);

    float diffuse_light_intensity = 0, specular_light_intensity = 0;
    for (const vec3 &light : scene.scene_lights) {
        vec3 to_light = light - point;
        float light_dist = to_light.norm();
        vec3 light_dir = to_light.normalized();
        if (point_in_shadow(point, light_dir, light_dist, scene))
            continue; // occluder before the light only
        diffuse_light_intensity  += std::max(0.f, light_dir*N);
        specular_light_intensity += std::pow(std::max(0.f, -reflect(-light_dir, N)*dir), material.specular_exponent);
    }
    return material.diffuse_color * diffuse_light_intensity * material.albedo[0] + vec3{1., 1., 1.}*specular_light_intensity * material.albedo[1] + reflect_color*material.albedo[2] + refract_color*material.albedo[3];
}
