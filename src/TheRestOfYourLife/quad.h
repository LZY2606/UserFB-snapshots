#ifndef QUAD_H
#define QUAD_H
//==============================================================================================
// To the extent possible under law, the author(s) have dedicated all copyright and related and
// neighboring rights to this software to the public domain worldwide. This software is
// distributed without any warranty.
//
// You should have received a copy (see file COPYING.txt) of the CC0 Public Domain Dedication
// along with this software. If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.
//==============================================================================================

#include "hittable.h"
#include "hittable_list.h"


class quad : public hittable {
  public:
    quad(const point3& Q, const vec3& u, const vec3& v, shared_ptr<material> mat)
      : Q(Q), u(u), v(v), mat(mat)
    {
        auto n = cross(u, v);
        normal = unit_vector(n);
        D = dot(normal, Q);
        w = n / dot(n,n);

        area = n.length();

        set_bounding_box();
    }

    virtual void set_bounding_box() {
        // Compute the bounding box of all four vertices.
        auto bbox_diagonal1 = aabb(Q, Q + u + v);
        auto bbox_diagonal2 = aabb(Q + u, Q + v);
        bbox = aabb(bbox_diagonal1, bbox_diagonal2);
    }

    aabb bounding_box() const override { return bbox; }

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
        // A reused record must never keep differentials from an earlier hit.
        rec.invalidate_differentials();

        auto denom = dot(normal, r.direction());

        // No hit if the ray is parallel to the plane.
        if (std::fabs(denom) < 1e-8)
            return false;

        // Return false if the hit point parameter t is outside the ray interval.
        auto t = (D - dot(normal, r.origin())) / denom;
        if (!ray_t.contains(t))
            return false;

        // Determine if the hit point lies within the planar shape using its plane coordinates.
        auto intersection = r.at(t);
        vec3 planar_hitpt_vector = intersection - Q;
        auto alpha = dot(w, cross(planar_hitpt_vector, v));
        auto beta = dot(w, cross(u, planar_hitpt_vector));

        if (!is_interior(alpha, beta, rec))
            return false;

        // Ray hits the 2D shape; set the rest of the hit record and return true.
        rec.t = t;
        rec.p = intersection;
        rec.mat = mat;
        rec.set_face_normal(r, normal);

        if (r.has_differentials)
            fill_differentials(r, rec, denom);

        return true;
    }

    virtual bool is_interior(double a, double b, hit_record& rec) const {
        interval unit_interval = interval(0, 1);
        // Given the hit point in plane coordinates, return false if it is outside the
        // primitive, otherwise set the hit record UV coordinates and return true.

        if (!unit_interval.contains(a) || !unit_interval.contains(b))
            return false;

        rec.u = a;
        rec.v = b;
        return true;
    }

    double pdf_value(const point3& origin, const vec3& direction) const override {
        hit_record rec;
        if (!this->hit(ray(origin, direction), interval(0.001, infinity), rec))
            return 0;

        auto distance_squared = rec.t * rec.t * direction.length_squared();
        auto cosine = std::fabs(dot(direction, rec.normal) / direction.length());

        return distance_squared / (cosine * area);
    }

    vec3 random(const point3& origin) const override {
        auto p = Q + (random_double() * u) + (random_double() * v);
        return p - origin;
    }

  private:
    point3 Q;
    vec3 u, v;
    vec3 w;
    shared_ptr<material> mat;
    aabb bbox;
    vec3 normal;
    double D;
    double area;

    void fill_differentials(const ray& r, hit_record& rec, double denom) const {
        // Plane: normal.P = D. With P(s) = O(s) + t(s) Dvec(s),
        //     dt/ds = -normal.(dO/ds + t dDvec/ds) / (normal.Dvec)
        // and dP/ds = dO/ds + t dDvec/ds + (dt/ds) Dvec.
        // The plane normal is constant, so d(normal)/ds = 0 on both ray sides.
        // The local coordinates are linear in P:
        //     u = w.((P-Q) x v),  v = w.(u_vec x (P-Q)).

        rec.has_differentials = true;

        if (!std::isfinite(denom) || std::fabs(denom) < 1e-12) {
            rec.has_differentials = false;
            return;
        }

        vec3 dp[2];
        double duv[4];
        const vec3 d_origin[2]    = { r.do_dx, r.do_dy };
        const vec3 d_direction[2] = { r.dd_dx, r.dd_dy };

        for (int k = 0; k < 2; k++) {
            const vec3 ray_point_deriv = d_origin[k] + rec.t * d_direction[k];
            const double t_deriv = -dot(normal, ray_point_deriv) / denom;
            dp[k] = ray_point_deriv + t_deriv * r.direction();

            const vec3 d_planar = dp[k];
            duv[2*k]     = dot(w, cross(d_planar, v));
            duv[2*k + 1] = dot(w, cross(u, d_planar));
        }

        if (!all_finite(dp[0]) || !all_finite(dp[1])
            || !std::isfinite(duv[0]) || !std::isfinite(duv[1])
            || !std::isfinite(duv[2]) || !std::isfinite(duv[3])) {
            rec.invalidate_differentials();
            return;
        }

        rec.dpdx = dp[0];
        rec.dpdy = dp[1];
        rec.dndx = vec3(0,0,0);
        rec.dndy = vec3(0,0,0);

        rec.uv_footprint.valid = true;
        rec.uv_footprint.dudx = duv[0];
        rec.uv_footprint.dvdx = duv[1];
        rec.uv_footprint.dudy = duv[2];
        rec.uv_footprint.dvdy = duv[3];
    }

    static bool all_finite(const vec3& x) {
        return std::isfinite(x.x()) && std::isfinite(x.y())
            && std::isfinite(x.z());
    }
};


inline shared_ptr<hittable_list> box(const point3& a, const point3& b, shared_ptr<material> mat)
{
    // Returns the 3D box (six sides) that contains the two opposite vertices a & b.

    auto sides = make_shared<hittable_list>();

    // Construct the two opposite vertices with the minimum and maximum coordinates.
    auto min = point3(std::fmin(a.x(),b.x()), std::fmin(a.y(),b.y()), std::fmin(a.z(),b.z()));
    auto max = point3(std::fmax(a.x(),b.x()), std::fmax(a.y(),b.y()), std::fmax(a.z(),b.z()));

    auto dx = vec3(max.x() - min.x(), 0, 0);
    auto dy = vec3(0, max.y() - min.y(), 0);
    auto dz = vec3(0, 0, max.z() - min.z());

    sides->add(make_shared<quad>(point3(min.x(), min.y(), max.z()),  dx,  dy, mat)); // front
    sides->add(make_shared<quad>(point3(max.x(), min.y(), max.z()), -dz,  dy, mat)); // right
    sides->add(make_shared<quad>(point3(max.x(), min.y(), min.z()), -dx,  dy, mat)); // back
    sides->add(make_shared<quad>(point3(min.x(), min.y(), min.z()),  dz,  dy, mat)); // left
    sides->add(make_shared<quad>(point3(min.x(), max.y(), max.z()),  dx, -dz, mat)); // top
    sides->add(make_shared<quad>(point3(min.x(), min.y(), min.z()),  dx,  dz, mat)); // bottom

    return sides;
}


#endif
