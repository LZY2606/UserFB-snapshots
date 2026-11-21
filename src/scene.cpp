#include <stdexcept>
#include <cmath>

#include "scene.h"
#include "objects.h"

static bool finite_nonneg(const Vec &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)
        && v.x >= 0.0 && v.y >= 0.0 && v.z >= 0.0;
}

void Scene::add(Object *object) {
    Mesh *mesh = dynamic_cast<Mesh*>(object);
    if (mesh != NULL) mesh->set_slot((long)m_objects.size());
    m_objects.push_back( object );
}

ObjectIntersection Scene::intersect(const Ray &ray) {
    ObjectIntersection isct = ObjectIntersection();
    ObjectIntersection temp;
    long size = m_objects.size();

    for (int i=0; i<size; i++){
        temp = m_objects.at((unsigned)i)->get_intersection(ray);

        if (temp.hit) {
            if (isct.u == 0 || temp.u < isct.u) isct = temp;
        }
    }
    return isct;
}

Vec Scene::trace_ray(const Ray &ray, int depth, unsigned short*Xi) {

    ObjectIntersection isct = intersect(ray);

    // If no hit, return world colour
    if (!isct.hit) return Vec();

    // Deterministic ideal mirror path. It shares the exact geometry and
    // identity pipeline with the analytic beam film so a single ray inside a
    // beam region can be cross-checked against the area integration.
    if (isct.m.get_type() == IDEAL_MIRROR) {
        // Deterministic recursion guard: scenes made only of mirrors can
        // never terminate otherwise. The analytic beam film uses its own
        // [0,4] limit; this cap only protects the single-ray query.
        if (depth > 64) return Vec();
        Vec x = ray.origin + ray.direction * isct.u;
        Ray reflected = isct.m.ideal_reflected_ray(ray, x, isct.n);
        Vec colour = isct.m.get_colour();
        return colour.mult(trace_ray(reflected, depth + 1, Xi));
    }

    if (isct.m.get_type() == EMIT) return isct.m.get_emission();

    Vec colour = isct.m.get_colour();

    // Calculate max reflection
    double p = colour.x>colour.y && colour.x>colour.z ? colour.x : colour.y>colour.z ? colour.y : colour.z;

    // Russian roulette termination.
    // If random number between 0 and 1 is > p, terminate and return hit object's emmission
    double rnd = erand48(Xi);
    if (++depth>5){
        if (rnd<p*0.9) { // Multiply by 0.9 to avoid infinite loop with colours of 1.0
            colour=colour*(0.9/p);
        }
        else {
            return isct.m.get_emission();
        }
    }

    Vec x = ray.origin + ray.direction * isct.u;
    Ray reflected = isct.m.get_reflected_ray(ray, x, isct.n, Xi);

    return colour.mult( trace_ray(reflected, depth, Xi) );
}

void Scene::validate_for_beam() const {
    long size = (long)m_objects.size();
    for (long i = 0; i < size; ++i) {
        const Object *obj = m_objects[(size_t)i];
        const Mesh *mesh = dynamic_cast<const Mesh*>(obj);
        if (mesh == NULL) {
            throw std::invalid_argument("beam film: scene contains a non-Mesh object (e.g. Sphere)");
        }
        const std::vector<Triangle*> &tris = mesh->get_triangles();
        for (size_t k = 0; k < tris.size(); ++k) {
            const Material *mp = tris[k]->m;
            if (mp == NULL) {
                throw std::invalid_argument("beam film: triangle without material");
            }
            if (mp->has_texture()) {
                throw std::invalid_argument("beam film: textured materials are not supported");
            }
            MaterialType type = mp->get_type();
            Vec c = mp->get_colour();
            Vec e = mp->get_emission();
            if (!finite_nonneg(c) || !finite_nonneg(e)) {
                throw std::invalid_argument("beam film: material colour/emission must be finite and non-negative");
            }
            if (type == EMIT) {
                continue;
            }
            if (type == IDEAL_MIRROR) {
                if (c.x > 1.0 || c.y > 1.0 || c.z > 1.0) {
                    throw std::invalid_argument("beam film: mirror reflectance must not exceed 1");
                }
                if (e.x != 0.0 || e.y != 0.0 || e.z != 0.0) {
                    throw std::invalid_argument("beam film: ideal mirrors must have zero emission");
                }
                continue;
            }
            throw std::invalid_argument("beam film: only EMIT and IDEAL_MIRROR materials are supported");
        }
    }
}
