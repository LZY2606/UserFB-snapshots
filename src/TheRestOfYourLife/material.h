#ifndef MATERIAL_H
#define MATERIAL_H
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
#include "pdf.h"
#include "texture.h"


class scatter_record {
  public:
    color attenuation;
    shared_ptr<pdf> pdf_ptr;
    bool skip_pdf;
    ray skip_pdf_ray;
};


class material {
  public:
    virtual ~material() = default;

    virtual color emitted(
        const ray& r_in, const hit_record& rec, double u, double v, const point3& p
    ) const {
        return color(0,0,0);
    }

    virtual bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const {
        return false;
    }

    virtual double scattering_pdf(const ray& r_in, const hit_record& rec, const ray& scattered)
    const {
        return 0.0;
    }
};


// Differential helpers for ideal specular events. Everything here is
// differentiated along the actual screen-coordinate path that produced the
// central ray; no neighboring rays are traced.
namespace specular_diff {

    inline bool finite(const vec3& v) {
        return std::isfinite(v.x()) && std::isfinite(v.y())
            && std::isfinite(v.z());
    }

    // Derivative of unit_vector(w) given w and dw/ds.
    inline vec3 normalize_deriv(const vec3& w, const vec3& dw) {
        double len = w.length();
        if (!std::isfinite(len) || len <= 0.0)
            return vec3(0,0,0);
        return (dw - (dot(w, dw) / (len*len)) * w) / len;
    }

    // Differentiate the normalized mirror reflection of d about unit normal n.
    // r = d - 2(d.n) n; the result is projected tangent to r and divided by
    // |r|, i.e. it is the derivative of unit_vector(reflect(d, n)). `dd` is
    // the derivative of the (possibly unnormalized) d.
    inline vec3 reflect_deriv(const vec3& d, const vec3& dd,
                              const vec3& n, const vec3& dn) {
        vec3 r = reflect(d, n);
        vec3 dr = dd - 2.0 * (dot(dd, n) + dot(d, dn)) * n
                      - 2.0 * dot(d, n) * dn;
        double len = r.length();
        if (!std::isfinite(len) || len <= 0.0)
            return vec3(0,0,0);
        vec3 unit_r = r / len;
        vec3 result = (dr - dot(dr, unit_r) * unit_r) / len;
        return finite(result) ? result : vec3(0,0,0);
    }

    // Differentiate Snell refraction. `w` is the normalized incident direction
    // (pointing into the surface), n the unit surface normal oriented against
    // it, eta = etai/etat. w_out = eta w + (eta cos_i - cos_t) n with
    // cos_i = -w.n, cos_t = sqrt(1 - eta^2 (1 - cos_i^2)). The derivative
    // covers normalization of the incident direction, the curved-surface normal
    // change, and the index ratio; it diverges at the critical angle, where
    // cos_t -> 0 and the caller must discard the differentials instead.
    inline bool refract_deriv(const vec3& w, const vec3& dw,
                              const vec3& n, const vec3& dn, double eta,
                              vec3& result) {
        double cos_i = -dot(w, n);
        double sin2_i = std::fmax(0.0, 1.0 - cos_i*cos_i);
        double cos_t_sq = 1.0 - eta*eta*sin2_i;
        // The refraction mapping is non-differentiable as cos_t -> 0 (the
        // critical angle); keep a margin so numerical near-critical hits clear
        // their differentials instead of propagating an exploding derivative.
        if (!std::isfinite(cos_t_sq) || cos_t_sq < 1e-8)
            return false;                       // at/past the critical point
        double cos_t = std::sqrt(cos_t_sq);

        double d_cos_i = -dot(dw, n) - dot(w, dn);
        double d_cos_t = -(eta*eta * cos_i * d_cos_i) / cos_t;

        vec3 w_out = eta * w + (eta * cos_i - cos_t) * n;
        vec3 d_w_out = eta * dw
                     + (eta * d_cos_i - d_cos_t) * n
                     + (eta * cos_i - cos_t) * dn;

        double out_len = w_out.length();
        if (!std::isfinite(out_len) || out_len <= 0.0)
            return false;
        vec3 unit_out = w_out / out_len;
        result = (d_w_out - dot(d_w_out, unit_out) * unit_out) / out_len;
        return finite(result);
    }
}


class lambertian : public material {
  public:
    lambertian(const color& albedo) : tex(make_shared<solid_color>(albedo)) {}
    lambertian(shared_ptr<texture> tex) : tex(tex) {}

    bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
        srec.attenuation = tex->value(rec.u, rec.v, rec.p, rec.uv_footprint);
        srec.pdf_ptr = make_shared<cosine_pdf>(rec.normal);
        srec.skip_pdf = false;
        // Cosine-weighted diffuse scattering destroys the ray footprint.
        srec.skip_pdf_ray.clear_differentials();
        return true;
    }

    double scattering_pdf(const ray& r_in, const hit_record& rec, const ray& scattered)
    const override {
        auto cos_theta = dot(rec.normal, unit_vector(scattered.direction()));
        return cos_theta < 0 ? 0 : cos_theta/pi;
    }

  private:
    shared_ptr<texture> tex;
};


class metal : public material {
  public:
    metal(const color& albedo, double fuzz) : albedo(albedo), fuzz(fuzz < 1 ? fuzz : 1) {}

    bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
        vec3 reflected = reflect(r_in.direction(), rec.normal);
        reflected = unit_vector(reflected) + (fuzz * random_unit_vector());

        srec.attenuation = albedo;
        srec.pdf_ptr = nullptr;
        srec.skip_pdf = true;
        srec.skip_pdf_ray = ray(rec.p, reflected, r_in.time());
        srec.skip_pdf_ray.clear_differentials();

        // A perfect mirror propagates the footprint through ideal reflection.
        // Any fuzz adds a random perturbation that the differentials cannot
        // follow, so they are dropped.
        if (fuzz == 0.0 && r_in.has_differentials && rec.has_differentials) {
            vec3 ddx = specular_diff::reflect_deriv(
                r_in.direction(), r_in.dd_dx, rec.normal, rec.dndx);
            vec3 ddy = specular_diff::reflect_deriv(
                r_in.direction(), r_in.dd_dy, rec.normal, rec.dndy);
            if (specular_diff::finite(ddx) && specular_diff::finite(ddy)) {
                srec.skip_pdf_ray.has_differentials = true;
                srec.skip_pdf_ray.do_dx = rec.dpdx;
                srec.skip_pdf_ray.do_dy = rec.dpdy;
                srec.skip_pdf_ray.dd_dx = ddx;
                srec.skip_pdf_ray.dd_dy = ddy;
            }
        }

        return true;
    }

  private:
    color albedo;
    double fuzz;
};


class dielectric : public material {
  public:
    dielectric(double refraction_index) : refraction_index(refraction_index) {}

    bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
        srec.attenuation = color(1.0, 1.0, 1.0);
        srec.pdf_ptr = nullptr;
        srec.skip_pdf = true;
        double ri = rec.front_face ? (1.0/refraction_index) : refraction_index;

        vec3 unit_direction = unit_vector(r_in.direction());
        double cos_theta = std::fmin(dot(-unit_direction, rec.normal), 1.0);
        double sin_theta = std::sqrt(1.0 - cos_theta*cos_theta);

        bool cannot_refract = ri * sin_theta > 1.0;
        vec3 direction;

        // The Fresnel draw happens exactly once; the differentials follow the
        // same branch the central ray takes, never re-rolling for auxiliary rays.
        bool reflected_branch = cannot_refract || reflectance(cos_theta, ri) > random_double();
        if (reflected_branch)
            direction = reflect(unit_direction, rec.normal);
        else
            direction = refract(unit_direction, rec.normal, ri);

        srec.skip_pdf_ray = ray(rec.p, direction, r_in.time());
        srec.skip_pdf_ray.clear_differentials();

        if (r_in.has_differentials && rec.has_differentials) {
            if (reflected_branch) {
                // Total internal reflection, or the Fresnel-reflected branch.
                // Reflection is expressed in the normalized incident direction;
                // differentiate its normalization together with the normal.
                const vec3& raw_d = r_in.direction();
                vec3 unit_ddx = specular_diff::normalize_deriv(raw_d, r_in.dd_dx);
                vec3 unit_ddy = specular_diff::normalize_deriv(raw_d, r_in.dd_dy);
                vec3 rdx = specular_diff::reflect_deriv(
                    unit_direction, unit_ddx, rec.normal, rec.dndx);
                vec3 rdy = specular_diff::reflect_deriv(
                    unit_direction, unit_ddy, rec.normal, rec.dndy);
                if (specular_diff::finite(rdx) && specular_diff::finite(rdy)) {
                    srec.skip_pdf_ray.has_differentials = true;
                    srec.skip_pdf_ray.do_dx = rec.dpdx;
                    srec.skip_pdf_ray.do_dy = rec.dpdy;
                    srec.skip_pdf_ray.dd_dx = rdx;
                    srec.skip_pdf_ray.dd_dy = rdy;
                }
            } else {
                // Refraction. Feed in the derivatives of the normalized incident
                // direction; the helper adds the normal (curvature) and index
                // terms. At the non-differentiable critical point the central
                // scatter is kept and its differentials stay cleared.
                const vec3& raw_d = r_in.direction();
                vec3 unit_ddx = specular_diff::normalize_deriv(raw_d, r_in.dd_dx);
                vec3 unit_ddy = specular_diff::normalize_deriv(raw_d, r_in.dd_dy);
                vec3 tdx, tdy;
                bool ok_x = specular_diff::refract_deriv(
                    unit_direction, unit_ddx, rec.normal, rec.dndx, ri, tdx);
                bool ok_y = specular_diff::refract_deriv(
                    unit_direction, unit_ddy, rec.normal, rec.dndy, ri, tdy);
                if (ok_x && ok_y
                    && specular_diff::finite(tdx) && specular_diff::finite(tdy)) {
                    srec.skip_pdf_ray.has_differentials = true;
                    srec.skip_pdf_ray.do_dx = rec.dpdx;
                    srec.skip_pdf_ray.do_dy = rec.dpdy;
                    srec.skip_pdf_ray.dd_dx = tdx;
                    srec.skip_pdf_ray.dd_dy = tdy;
                }
            }
        }

        return true;
    }

  private:
    // Refractive index in vacuum or air, or the ratio of the material's refractive index over
    // the refractive index of the enclosing media
    double refraction_index;

    static double reflectance(double cosine, double refraction_index) {
        // Use Schlick's approximation for reflectance.
        auto r0 = (1 - refraction_index) / (1 + refraction_index);
        r0 = r0*r0;
        return r0 + (1-r0)*std::pow((1 - cosine),5);
    }
};


class diffuse_light : public material {
  public:
    diffuse_light(shared_ptr<texture> tex) : tex(tex) {}
    diffuse_light(const color& emit) : tex(make_shared<solid_color>(emit)) {}

    color emitted(const ray& r_in, const hit_record& rec, double u, double v, const point3& p)
    const override {
        if (!rec.front_face)
            return color(0,0,0);
        return tex->value(u, v, p, rec.uv_footprint);
    }

  private:
    shared_ptr<texture> tex;
};


class isotropic : public material {
  public:
    isotropic(const color& albedo) : tex(make_shared<solid_color>(albedo)) {}
    isotropic(shared_ptr<texture> tex) : tex(tex) {}

    bool scatter(const ray& r_in, const hit_record& rec, scatter_record& srec) const override {
        srec.attenuation = tex->value(rec.u, rec.v, rec.p, rec.uv_footprint);
        srec.pdf_ptr = make_shared<sphere_pdf>();
        srec.skip_pdf = false;
        // Isotropic phase scattering has no directional coherence.
        srec.skip_pdf_ray.clear_differentials();
        return true;
    }

    double scattering_pdf(const ray& r_in, const hit_record& rec, const ray& scattered)
    const override {
        return 1 / (4 * pi);
    }

  private:
    shared_ptr<texture> tex;
};


#endif
