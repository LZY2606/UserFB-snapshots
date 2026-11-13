#ifndef HITTABLE_H
#define HITTABLE_H
//==============================================================================================
// Originally written in 2016 by Peter Shirley <ptrshrl@gmail.com>
//
// To the extent possible under law, the author(s) have dedicated all copyright and related and
// neighboring rights to this software to the public domain worldwide. This software is
// distributed without any warranty.
//
// You should have received a copy (see file COPYING.txt) of the CC0 Public Domain Dedication
// along with this software. If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.
//==============================================================================================

#include "aabb.h"
#include "texture_footprint.h"


class material;


class hit_record {
  public:
    point3 p;
    vec3 normal;
    shared_ptr<material> mat;
    double t;
    double u;
    double v;
    bool front_face;

    // Surface differentials (with respect to screen x/y pixel coordinates).
    // has_differentials mirrors the incoming ray's flag and gates dp/dn;
    // uv_footprint additionally goes invalid at sphere poles and tangent hits.
    bool has_differentials;
    vec3 dpdx, dpdy;   // d(hit point) / d(screen x), d(hit point) / d(screen y)
    vec3 dndx, dndy;   // d(unit geometric normal) / d(screen x), / d(screen y)
    texture_footprint uv_footprint;

    hit_record()
      : p(0,0,0), normal(0,0,0), mat(nullptr), t(0), u(0), v(0),
        front_face(false), has_differentials(false),
        dpdx(0,0,0), dpdy(0,0,0), dndx(0,0,0), dndy(0,0,0),
        uv_footprint() {}

    void set_face_normal(const ray& r, const vec3& outward_normal) {
        // Sets the hit record normal vector.
        // NOTE: the parameter `outward_normal` is assumed to have unit length.

        front_face = dot(r.direction(), outward_normal) < 0;
        normal = front_face ? outward_normal : -outward_normal;
    }

    // Drop every differential carried by this record. Primitives call this
    // before filling a record in, so a reused hit_record can never keep the
    // flags or values from a previous intersection.
    void invalidate_differentials() {
        has_differentials = false;
        dpdx = vec3(0,0,0);
        dpdy = vec3(0,0,0);
        dndx = vec3(0,0,0);
        dndy = vec3(0,0,0);
        uv_footprint.reset();
    }

    bool differentials_finite() const {
        return vec_finite(dpdx) && vec_finite(dpdy)
            && vec_finite(dndx) && vec_finite(dndy)
            && uv_footprint.all_finite();
    }

  private:
    static bool vec_finite(const vec3& v) {
        return std::isfinite(v.x()) && std::isfinite(v.y())
            && std::isfinite(v.z());
    }
};


class hittable {
  public:
    virtual ~hittable() = default;

    virtual bool hit(const ray& r, interval ray_t, hit_record& rec) const = 0;

    virtual aabb bounding_box() const = 0;

    virtual double pdf_value(const point3& origin, const vec3& direction) const {
        return 0.0;
    }

    virtual vec3 random(const point3& origin) const {
        return vec3(1,0,0);
    }
};


class translate : public hittable {
  public:
    translate(shared_ptr<hittable> object, const vec3& offset)
      : object(object), offset(offset)
    {
        bbox = object->bounding_box() + offset;
    }

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
        // Move the ray backwards by the offset
        ray offset_r(r.origin() - offset, r.direction(), r.time());
        offset_r.copy_differentials_from(r);

        // Determine whether an intersection exists along the offset ray (and if so, where)
        if (!object->hit(offset_r, ray_t, rec))
            return false;

        // Move the intersection point forwards by the offset
        rec.p += offset;

        return true;
    }

    aabb bounding_box() const override { return bbox; }

  private:
    shared_ptr<hittable> object;
    vec3 offset;
    aabb bbox;
};


class rotate_y : public hittable {
  public:
    rotate_y(shared_ptr<hittable> object, double angle) : object(object) {
        auto radians = degrees_to_radians(angle);
        sin_theta = std::sin(radians);
        cos_theta = std::cos(radians);
        bbox = object->bounding_box();

        point3 min( infinity,  infinity,  infinity);
        point3 max(-infinity, -infinity, -infinity);

        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 2; j++) {
                for (int k = 0; k < 2; k++) {
                    auto x = i*bbox.x.max + (1-i)*bbox.x.min;
                    auto y = j*bbox.y.max + (1-j)*bbox.y.min;
                    auto z = k*bbox.z.max + (1-k)*bbox.z.min;

                    auto newx =  cos_theta*x + sin_theta*z;
                    auto newz = -sin_theta*x + cos_theta*z;

                    vec3 tester(newx, y, newz);

                    for (int c = 0; c < 3; c++) {
                        min[c] = std::fmin(min[c], tester[c]);
                        max[c] = std::fmax(max[c], tester[c]);
                    }
                }
            }
        }

        bbox = aabb(min, max);
    }

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {

        // Transform the ray from world space to object space.

        auto origin = point3(
            (cos_theta * r.origin().x()) - (sin_theta * r.origin().z()),
            r.origin().y(),
            (sin_theta * r.origin().x()) + (cos_theta * r.origin().z())
        );

        auto direction = vec3(
            (cos_theta * r.direction().x()) - (sin_theta * r.direction().z()),
            r.direction().y(),
            (sin_theta * r.direction().x()) + (cos_theta * r.direction().z())
        );

        ray rotated_r(origin, direction, r.time());
        rotated_r.copy_differentials_from(r);
        if (r.has_differentials) {
            rotated_r.do_dx = rotate_in(r.do_dx);
            rotated_r.do_dy = rotate_in(r.do_dy);
            rotated_r.dd_dx = rotate_in(r.dd_dx);
            rotated_r.dd_dy = rotate_in(r.dd_dy);
        }

        // Determine whether an intersection exists in object space (and if so, where).

        if (!object->hit(rotated_r, ray_t, rec))
            return false;

        // Transform the intersection from object space back to world space.

        rec.p = point3(
            (cos_theta * rec.p.x()) + (sin_theta * rec.p.z()),
            rec.p.y(),
            (-sin_theta * rec.p.x()) + (cos_theta * rec.p.z())
        );

        rec.normal = vec3(
            (cos_theta * rec.normal.x()) + (sin_theta * rec.normal.z()),
            rec.normal.y(),
            (-sin_theta * rec.normal.x()) + (cos_theta * rec.normal.z())
        );

        if (rec.has_differentials) {
            rec.dpdx = rotate_out(rec.dpdx);
            rec.dpdy = rotate_out(rec.dpdy);
            rec.dndx = rotate_out(rec.dndx);
            rec.dndy = rotate_out(rec.dndy);
            if (!vec_finite(rec.dpdx) || !vec_finite(rec.dpdy)
                || !vec_finite(rec.dndx) || !vec_finite(rec.dndy)) {
                rec.invalidate_differentials();
            }
        }

        return true;
    }

    aabb bounding_box() const override { return bbox; }

  private:
    shared_ptr<hittable> object;
    double sin_theta;
    double cos_theta;
    aabb bbox;

    vec3 rotate_in(const vec3& v) const {
        // World -> object space rotation about the Y axis.
        return vec3(
            (cos_theta * v.x()) - (sin_theta * v.z()),
            v.y(),
            (sin_theta * v.x()) + (cos_theta * v.z())
        );
    }

    vec3 rotate_out(const vec3& v) const {
        // Object -> world space rotation about the Y axis.
        return vec3(
            (cos_theta * v.x()) + (sin_theta * v.z()),
            v.y(),
            (-sin_theta * v.x()) + (cos_theta * v.z())
        );
    }

    static bool vec_finite(const vec3& v) {
        return std::isfinite(v.x()) && std::isfinite(v.y())
            && std::isfinite(v.z());
    }
};


#endif
