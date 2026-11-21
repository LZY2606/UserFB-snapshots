#ifndef SCENE_H
#define SCENE_H

#include "ray.h"
#include "vector.h"
#include "objects.h"

class Scene {
private:
    std::vector<Object*> m_objects;

public:
    Scene(){};
    void add(Object *object);
    ObjectIntersection intersect(const Ray &ray);
    Vec trace_ray(const Ray &ray, int depth, unsigned short*Xi);

    size_t object_count() const { return m_objects.size(); }
    Object* get_object(size_t i) { return m_objects.at(i); }

    // Throws std::invalid_argument unless the scene is usable by the
    // analytic beam film: meshes only, EMIT/IDEAL_MIRROR only, no textures,
    // finite and bounded colours and emission.
    void validate_for_beam() const;
};

#endif //SCENE_H
