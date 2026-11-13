#ifndef SPHERE_H
#define SPHERE_H
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

#include "hittable.h"
#include "onb.h"


class sphere : public hittable {
  public:
    // Stationary Sphere
    sphere(const point3& static_center, double radius, shared_ptr<material> mat)
      : center(static_center, vec3(0,0,0)), radius(std::fmax(0,radius)), mat(mat)
    {
        auto rvec = vec3(radius, radius, radius);
        bbox = aabb(static_center - rvec, static_center + rvec);
    }

    // Moving Sphere
    sphere(const point3& center1, const point3& center2, double radius,
           shared_ptr<material> mat)
      : center(center1, center2 - center1), radius(std::fmax(0,radius)), mat(mat)
    {
        auto rvec = vec3(radius, radius, radius);
        aabb box1(center.at(0) - rvec, center.at(0) + rvec);
        aabb box2(center.at(1) - rvec, center.at(1) + rvec);
        bbox = aabb(box1, box2);
    }

    bool hit(const ray& r, interval ray_t, hit_record& rec) const override {
        // A reused record must never keep differentials from an earlier hit.
        rec.invalidate_differentials();

        point3 current_center = center.at(r.time());
        vec3 oc = current_center - r.origin();
        auto a = r.direction().length_squared();
        auto h = dot(r.direction(), oc);
        auto c = oc.length_squared() - radius*radius;

        auto discriminant = h*h - a*c;
        if (discriminant < 0)
            return false;

        auto sqrtd = std::sqrt(discriminant);

        // Find the nearest root that lies in the acceptable range.
        auto root = (h - sqrtd) / a;
        if (!ray_t.surrounds(root)) {
            root = (h + sqrtd) / a;
            if (!ray_t.surrounds(root))
                return false;
        }

        rec.t = root;
        rec.p = r.at(rec.t);
        vec3 outward_normal = (rec.p - current_center) / radius;
        rec.set_face_normal(r, outward_normal);
        get_sphere_uv(outward_normal, rec.u, rec.v);
        rec.mat = mat;

        if (r.has_differentials)
            fill_differentials(r, rec, current_center, outward_normal);

        return true;
    }

    aabb bounding_box() const override { return bbox; }

    double pdf_value(const point3& origin, const vec3& direction) const override {
        // This method only works for stationary spheres.

        hit_record rec;
        if (!this->hit(ray(origin, direction), interval(0.001, infinity), rec))
            return 0;

        auto dist_squared = (center.at(0) - origin).length_squared();
        auto cos_theta_max = std::sqrt(1 - radius*radius/dist_squared);
        auto solid_angle = 2*pi*(1-cos_theta_max);

        return  1 / solid_angle;
    }

    vec3 random(const point3& origin) const override {
        vec3 direction = center.at(0) - origin;
        auto distance_squared = direction.length_squared();
        onb uvw(direction);
        return uvw.transform(random_to_sphere(radius, distance_squared));
    }

  private:
    ray center;
    double radius;
    shared_ptr<material> mat;
    aabb bbox;

    static void get_sphere_uv(const point3& p, double& u, double& v) {
        // p: a given point on the sphere of radius one, centered at the origin.
        // u: returned value [0,1] of angle around the Y axis from X=-1.
        // v: returned value [0,1] of angle from Y=-1 to Y=+1.
        //     <1 0 0> yields <0.50 0.50>       <-1  0  0> yields <0.00 0.50>
        //     <0 1 0> yields <0.50 1.00>       < 0 -1  0> yields <0.50 0.00>
        //     <0 0 1> yields <0.25 0.50>       < 0  0 -1> yields <0.75 0.50>

        auto theta = std::acos(-p.y());
        auto phi = std::atan2(-p.z(), p.x()) + pi;

        u = phi / (2*pi);
        v = theta / pi;
    }

    void fill_differentials(const ray& r, hit_record& rec, const point3& current_center,
                            const vec3& outward_normal) const {
        // Differentiate the implicit surface
        //     F(P,t) = |O + t D - C|^2 - R^2 = 0
        // with respect to a screen coordinate s, holding the ray time fixed.
        // The center C = C(time) is therefore constant w.r.t. s, so the
        // sphere's motion never enters these screen-space derivatives. With
        // q(s) = O + tD - C (|q| = R) and unit normal n = q/R:
        //     dt/ds = -q.(dO/ds + t dD/ds) / (q.D)
        //     dP/ds = dO/ds + t dD/ds + (dt/ds) D
        //     dn/ds = (dP/ds) / R
        // The 1/R factor carries the surface curvature, rather than treating
        // the sphere as a flat, constant-normal plane.

        const vec3 q = rec.p - current_center;          // length = radius
        const vec3 d = r.direction();
        const double denom = dot(q, d);                 // zero at a tangent hit

        rec.has_differentials = true;

        if (!std::isfinite(denom) || std::fabs(denom) < 1e-12 * radius) {
            rec.has_differentials = false;
            rec.uv_footprint.reset();
            return;
        }

        vec3 dp[2], dn[2];
        const vec3 d_origin[2] = { r.do_dx, r.do_dy };
        const vec3 d_direction[2] = { r.dd_dx, r.dd_dy };

        for (int k = 0; k < 2; k++) {
            const vec3 ray_point_deriv = d_origin[k] + rec.t * d_direction[k];
            const double t_deriv = -dot(q, ray_point_deriv) / denom;
            dp[k] = ray_point_deriv + t_deriv * d;
            dn[k] = dp[k] / radius;
        }

        if (!all_vec_finite(dp[0]) || !all_vec_finite(dp[1])
            || !all_vec_finite(dn[0]) || !all_vec_finite(dn[1])) {
            rec.invalidate_differentials();
            return;
        }

        rec.dpdx = dp[0];
        rec.dpdy = dp[1];

        // The record normal flips with the ray side; the derivative flips too.
        if (rec.front_face) {
            rec.dndx = dn[0];
            rec.dndy = dn[1];
        } else {
            rec.dndx = -dn[0];
            rec.dndy = -dn[1];
        }

        fill_uv_footprint(rec, outward_normal, dn);
    }

    void fill_uv_footprint(hit_record& rec, const vec3& outward_normal,
                           const vec3 outward_normal_deriv[2]) const {
        // Local UV derivatives. u = (atan2(-z, x) + pi)/(2 pi), v = acos(-y)/pi
        // on the unit sphere. The atan2 form is differentiated directly instead
        // of differencing wrapped u values, so derivatives stay local and finite
        // across the u = 0/1 meridian seam rather than jumping by a full wrap.
        const double x = outward_normal.x();
        const double z = outward_normal.z();
        const double phi_scale = 1.0 / (2.0 * pi);
        const double sin_theta_sq = x*x + z*z;   // 1 - y^2 on the unit sphere

        // At the poles the longitude angle (and hence u) is undefined. Invalidate
        // only the UV footprint: position and normal derivatives remain valid.
        if (!std::isfinite(sin_theta_sq) || sin_theta_sq <= 1e-20) {
            rec.uv_footprint.reset();
            return;
        }

        const vec3 d_out[2] = { outward_normal_deriv[0], outward_normal_deriv[1] };
        double deriv[4];
        for (int k = 0; k < 2; k++) {
            const double du = phi_scale * (z * d_out[k].x() - x * d_out[k].z())
                              / sin_theta_sq;
            const double dv = d_out[k].y() / (pi * std::sqrt(sin_theta_sq));
            deriv[2*k]     = du;
            deriv[2*k + 1] = dv;
        }

        if (!std::isfinite(deriv[0]) || !std::isfinite(deriv[1])
            || !std::isfinite(deriv[2]) || !std::isfinite(deriv[3])) {
            rec.uv_footprint.reset();
            return;
        }

        rec.uv_footprint.valid = true;
        rec.uv_footprint.dudx = deriv[0];
        rec.uv_footprint.dvdx = deriv[1];
        rec.uv_footprint.dudy = deriv[2];
        rec.uv_footprint.dvdy = deriv[3];
    }

    static bool all_vec_finite(const vec3& v) {
        return std::isfinite(v.x()) && std::isfinite(v.y())
            && std::isfinite(v.z());
    }

    static vec3 random_to_sphere(double radius, double distance_squared) {
        auto r1 = random_double();
        auto r2 = random_double();
        auto z = 1 + r2*(std::sqrt(1-radius*radius/distance_squared) - 1);

        auto phi = 2*pi*r1;
        auto x = std::cos(phi) * std::sqrt(1-z*z);
        auto y = std::sin(phi) * std::sqrt(1-z*z);

        return vec3(x, y, z);
    }
};


#endif
