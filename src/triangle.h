#ifndef TRIANGLE_H
#define TRIANGLE_H

#include <algorithm>
#include <cfloat>

#include "vector.h"
#include "aabbox.h"
#include "material.h"

struct Triangle {
    Vec v0, v1, v2;     // Vertex world space coordinates
    Vec e1, e2;         // Edge 1 and edge 2 of triangle
    Vec n, t0, t1, t2;  // Triangle normal and texture coordinates
    Material *m;        // Pointer to material
    long slot;          // Scene add() slot of the owning mesh
    long face;          // Original OBJ face order inside the owning mesh

    Triangle(Vec v0_, Vec v1_, Vec v2_, Vec t0_=Vec(), Vec t1_=Vec(), Vec t2_=Vec(),
             Material *m_=NULL, long slot_=-1, long face_=-1){
        v0=v0_, v1=v1_, v2=v2_, e1=v1-v0, e2=v2-v0, n=e1.cross(e2).norm();
        t0=t0_, t1=t1_, t2=t2_;
        m=m_;
        slot=slot_;
        face=face_;
    }

    // True when the triangle has a well defined, non-zero geometric area
    // and all of its vertices are finite.
    bool valid() const {
        Vec cr = e1.cross(e2);
        return std::isfinite(cr.x) && std::isfinite(cr.y) && std::isfinite(cr.z)
            && std::isfinite(v0.x) && std::isfinite(v0.y) && std::isfinite(v0.z)
            && std::isfinite(v1.x) && std::isfinite(v1.y) && std::isfinite(v1.z)
            && std::isfinite(v2.x) && std::isfinite(v2.y) && std::isfinite(v2.z)
            && cr.dot(cr) > 0.0;
    }

    // Returns axis aligned bounding box that contains the triangle
    AABBox get_bounding_box(){
        Vec bl = Vec(
                std::min (std::min(v0.x, v1.x), v2.x ) ,
                std::min (std::min(v0.y, v1.y), v2.y ) ,
                std::min (std::min(v0.z, v1.z), v2.z )
        );
        Vec tr = Vec(
                std::max (std::max(v0.x, v1.x), v2.x ) ,
                std::max (std::max(v0.y, v1.y), v2.y ) ,
                std::max (std::max(v0.z, v1.z), v2.z )
        );

        return AABBox(bl, tr);
    }

    // Returns the midpoint of the triangle
    Vec get_midpoint(){
        return (v0 + v1 + v2)/3;
    }

    // Checks if ray intersects with triangle. Returns true/false.
    bool intersect(Ray ray, double &t, double tmin, Vec &norm) const {

        double u, v, t_temp=0;

        Vec pvec = ray.direction.cross(e2);
        double det = e1.dot(pvec);
        if (det == 0) return false;
        double invDet = 1. / det;
        Vec tvec = ray.origin - v0;
        u = tvec.dot(pvec) * invDet;
        if (u < 0 || u > 1) return false;
        Vec qvec = tvec.cross(e1);
        v = ray.direction.dot(qvec) * invDet;
        if (v < 0 || u + v > 1) return false;
        t_temp = e2.dot(qvec) * invDet; // Set distance along ray to intersection
        if (t_temp < tmin) {
            if (t_temp > 1e-9 ){    // Fairly arbritarily small value, scared to change
                t = t_temp;         // it as it works.
                norm = n;
                return true;
            }
        }
        return false;
    }

    // Double sided nearest hit query used by the beam film and the
    // deterministic IDEAL_MIRROR path. Returns the positive ray parameter of
    // the first intersection and fills the geometric normal oriented against
    // the ray. No tmin culling is performed here so segment start planes can
    // be handled explicitly by the caller.
    bool hit_point(const Ray &ray, double &t_out, Vec &norm_out) const {
        Vec pvec = ray.direction.cross(e2);
        double det = e1.dot(pvec);
        if (det == 0.0) return false;
        double invDet = 1.0 / det;
        Vec tvec = ray.origin - v0;
        double u = tvec.dot(pvec) * invDet;
        if (u < 0.0 || u > 1.0) return false;
        Vec qvec = tvec.cross(e1);
        double v = ray.direction.dot(qvec) * invDet;
        if (v < 0.0 || u + v > 1.0) return false;
        double t = e2.dot(qvec) * invDet;
        if (!(t > 1e-12) || !std::isfinite(t)) return false;
        t_out = t;
        // Mirrors are double sided: orient the geometric normal towards the
        // incoming ray instead of returning a fixed winding normal.
        norm_out = n.dot(ray.direction) < 0.0 ? n : n * -1.0;
        return true;
    }

    // Returns barycentric coordinates of point p on the triangle
    Vec barycentric(Vec p){
        Vec v2_ = p - v0;
        double d00 = e1.dot(e1);
        double d01 = e1.dot(e2);
        double d11 = e2.dot(e2);
        double d20 = v2_.dot(e1);
        double d21 = v2_.dot(e2);
        double d = d00*d11 - d01*d01;
        double v = (d11*d20 - d01*d21) / d;
        double w = (d00*d21 - d01*d20) / d;
        double u = 1 - v - w;
        return Vec(u, v, w);
    }

    // Returns the colour at point p on the triangle
    Vec get_colour_at(Vec p){
        if(m==NULL) return Vec(1,0,1);

        Vec b = barycentric(p);
        Vec c = Vec();
        c = c + (t0 * b.x);
        c = c + (t1 * b.y);
        c = c + (t2 * b.z);

        return m->get_colour_at(c.x, c.y);
    }

};

#endif // TRIANGLE_H
