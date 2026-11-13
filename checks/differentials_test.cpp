#ifndef DIFFERENTIALS_TEST_CPP
#define DIFFERENTIALS_TEST_CPP
//==============================================================================================
// Contract tests for ray differentials, surface differentials, specular
// propagation and mip-mapped texture footprints in TheRestOfYourLife.
//
// Every analytic differential is cross-checked against an independent central
// finite difference (two decreasing steps, checking convergence), never
// against another copy of the same analytic formula.
//==============================================================================================

#include "check.h"

#include "rtweekend.h"
#include "camera.h"
#include "sphere.h"
#include "quad.h"
#include "bvh.h"
#include "hittable_list.h"
#include "material.h"
#include "constant_medium.h"
#include "texture.h"

#include <cmath>
#include <fstream>
#include <functional>
#include <sstream>
#include <vector>


namespace {

const double PI = 3.1415926535897932385;


bool near(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

bool vec_near(const vec3& a, const vec3& b, double eps = 1e-9) {
    return near(a.x(), b.x(), eps) && near(a.y(), b.y(), eps)
        && near(a.z(), b.z(), eps);
}

double vec_norm(const vec3& v) { return v.length(); }

template <class F>
vec3 central_diff_vec(F f, double h) {
    return (f(h) - f(-h)) / (2.0 * h);
}

template <class F>
double central_diff_scalar(F f, double h) {
    return (f(h) - f(-h)) / (2.0 * h);
}

// Compare an analytic vector derivative with central differences at two
// decreasing steps. Both quotients must agree with the analytic value; the
// finer-step error may equal the coarser one once the floating-point noise
// floor is reached.
bool check_vec_deriv(const vec3& analytic,
                     const std::function<vec3(double)>& f,
                     double h1, double h2,
                     double scale = 1.0) {
    const vec3 n1 = central_diff_vec(f, h1);
    const vec3 n2 = central_diff_vec(f, h2);
    const double tol = 1e-2 * std::fmax(1.0, vec_norm(analytic)) * scale;
    const double floor = 1e-10 * std::fmax(1.0, vec_norm(analytic));
    const vec3 e1 = n1 - analytic;
    const vec3 e2 = n2 - analytic;
    for (int k = 0; k < 3; k++) {
        if (!(std::fabs(e1[k]) <= tol))
            return false;
        if (!(std::fabs(e2[k]) <= std::fabs(e1[k]) + floor))
            return false;
    }
    return true;
}

bool check_scalar_deriv(double analytic,
                        const std::function<double(double)>& f,
                        double h1, double h2,
                        double scale = 1.0) {
    const double n1 = central_diff_scalar(f, h1);
    const double n2 = central_diff_scalar(f, h2);
    const double tol = 1e-2 * std::fmax(1.0, std::fabs(analytic)) * scale;
    const double floor = 1e-10 * std::fmax(1.0, std::fabs(analytic));
    const double e1 = std::fabs(n1 - analytic);
    const double e2 = std::fabs(n2 - analytic);
    return e1 <= tol && e2 <= e1 + floor;
}

// Unwrap a u value near reference u so finite differences never see the
// artificial 0/1 seam jump: u_local = u + round(u_ref - u).
double unwrap(double u, double ref) {
    return u + std::floor(ref - u + 0.5);
}

// Re-aim a prepared camera ray at pixel (px + s, py) with every other sample
// parameter locked so only the screen coordinate changes.
ray offset_ray(const camera& cam, double px, double py, double s,
               double lens_x, double lens_y, double time) {
    return cam.sample_ray(px + s, py, lens_x, lens_y, time);
}

ray offset_ray_y(const camera& cam, double px, double py, double s,
                 double lens_x, double lens_y, double time) {
    return cam.sample_ray(px, py + s, lens_x, lens_y, time);
}

camera make_camera() {
    camera cam;
    cam.image_width = 16;
    cam.aspect_ratio = 1.0;
    cam.samples_per_pixel = 1;
    cam.max_depth = 8;
    cam.background = color(0, 0, 0);
    cam.vfov = 60;
    cam.lookfrom = point3(0, 0, 0);
    cam.lookat = point3(0, 0, -1);
    cam.vup = vec3(0, 1, 0);
    cam.defocus_angle = 0;
    cam.focus_dist = 8;
    cam.enable_differentials = true;
    cam.prepare();
    return cam;
}


//----------------------------------------------------------------------------------------------
// camera group
//----------------------------------------------------------------------------------------------

void test_camera() {
    // Off by default: plain ray, zeroed differentials.
    {
        camera cam;
        cam.image_width = 8;
        cam.aspect_ratio = 1.0;
        cam.samples_per_pixel = 4;
        cam.focus_dist = 5;
        cam.prepare();
        ray r = cam.sample_ray(3.2, 2.1, 0.0, 0.0, 0.37);
        UNIT_CHECK(!r.has_differentials);
        UNIT_CHECK(vec_near(r.do_dx, vec3(0,0,0)));
        UNIT_CHECK(vec_near(r.do_dy, vec3(0,0,0)));
        UNIT_CHECK(vec_near(r.dd_dx, vec3(0,0,0)));
        UNIT_CHECK(vec_near(r.dd_dy, vec3(0,0,0)));
        UNIT_CHECK(near(r.time(), 0.37));
    }

    // Enabled: derivatives are exactly the full-pixel deltas, origin is fixed.
    {
        camera cam = make_camera();
        ray r = cam.sample_ray(5.25, 3.75, 0.0, 0.0, 0.11);
        UNIT_CHECK(r.has_differentials);
        UNIT_CHECK(vec_near(r.do_dx, vec3(0,0,0)));
        UNIT_CHECK(vec_near(r.do_dy, vec3(0,0,0)));

        // Numeric check: shifting the screen coordinate by one full pixel moves
        // the ray direction by the analytic differential, with time locked.
        ray rx = cam.sample_ray(6.25, 3.75, 0.0, 0.0, 0.11);
        ray ry = cam.sample_ray(5.25, 4.75, 0.0, 0.0, 0.11);
        UNIT_CHECK(vec_near(rx.direction() - r.direction(), r.dd_dx, 1e-12));
        UNIT_CHECK(vec_near(ry.direction() - r.direction(), r.dd_dy, 1e-12));
        UNIT_CHECK(near(rx.time(), r.time()));
        UNIT_CHECK(vec_near(rx.origin(), r.origin(), 1e-12));
    }

    // Determinism: identical arguments give identical rays.
    {
        camera cam = make_camera();
        ray a = cam.sample_ray(2.3, 4.1, 0.2, -0.1, 0.5);
        ray b = cam.sample_ray(2.3, 4.1, 0.2, -0.1, 0.5);
        UNIT_CHECK(vec_near(a.origin(), b.origin()));
        UNIT_CHECK(vec_near(a.direction(), b.direction()));
        UNIT_CHECK(near(a.time(), b.time()));
        UNIT_CHECK(vec_near(a.dd_dx, b.dd_dx));
    }

    // Differential scale must not change with samples_per_pixel.
    {
        camera c1 = make_camera();
        camera c2 = make_camera();
        c2.samples_per_pixel = 64;
        c2.prepare();
        ray r1 = c1.sample_ray(1, 1, 0, 0, 0.2);
        ray r2 = c2.sample_ray(1, 1, 0, 0, 0.2);
        UNIT_CHECK(vec_near(r1.dd_dx, r2.dd_dx, 1e-12));
        UNIT_CHECK(vec_near(r1.dd_dy, r2.dd_dy, 1e-12));
    }

    // sample_ray consumes no random numbers itself.
    {
        std::srand(12345);
        int before = std::rand();
        std::srand(12345);
        camera cam = make_camera();
        cam.sample_ray(1.0, 2.0, 0.1, 0.1, 0.4);
        cam.sample_ray(3.0, 4.0, 0.0, 0.0, 0.6);
        UNIT_CHECK(std::rand() == before);
    }

    // Defocus: with a fixed lens sample, origin stays constant across pixels.
    {
        camera cam = make_camera();
        cam.defocus_angle = 1.5;
        cam.prepare();
        ray r0 = cam.sample_ray(4, 4, 0.3, -0.2, 0.7);
        ray r1 = cam.sample_ray(5, 4, 0.3, -0.2, 0.7);
        UNIT_CHECK(r0.has_differentials);
        UNIT_CHECK(vec_near(r0.origin(), r1.origin(), 1e-12));
        UNIT_CHECK(!vec_near(r0.dd_dx, vec3(0,0,0), 1e-6));
    }

    // Default-off render still produces the exact legacy 8x8 emitted image.
    {
        auto matte = make_shared<lambertian>(color(.5, .5, .5));
        hittable_list world, lights;
        world.add(make_shared<quad>(point3(-100,-100,-1), vec3(200,0,0),
                                    vec3(0,200,0),
                                    make_shared<diffuse_light>(color(.25,.5,1))));
        camera cam;
        cam.image_width = 8;
        cam.aspect_ratio = 1;
        cam.samples_per_pixel = 1;
        cam.max_depth = 2;
        cam.background = color(0,0,0);
        cam.defocus_angle = 0;
        std::ostringstream image;
        auto* old = std::cout.rdbuf(image.rdbuf());
        cam.render(world, lights);
        std::cout.rdbuf(old);
        std::istringstream in(image.str());
        std::string magic;
        int w, h, maxv;
        in >> magic >> w >> h >> maxv;
        UNIT_CHECK(magic == "P3" && w == 8 && h == 8 && maxv == 255);
        for (int n = 0; n < 64; n++) {
            int rr, gg, bb;
            in >> rr >> gg >> bb;
            UNIT_CHECK(rr == 128 && gg == 181 && bb == 255);
        }
    }
}


//----------------------------------------------------------------------------------------------
// geometry group
//----------------------------------------------------------------------------------------------

// Hit a hittable with a differentiated camera ray aimed at (px, py).
bool hit_at(const hittable& obj, const camera& cam, double px, double py,
            double time, hit_record& rec, const ray* explicit_ray = nullptr) {
    ray r = explicit_ray ? *explicit_ray
                         : cam.sample_ray(px, py, 0.0, 0.0, time);
    return obj.hit(r, interval(0.001, infinity), rec);
}

void test_geometry() {
    const double time = 0.31;
    const double h1 = 1e-2, h2 = 2.5e-3;

    // Stationary sphere, generic off-axis hit.
    {
        camera cam = make_camera();
        auto matte = make_shared<lambertian>(color(.5,.5,.5));
        sphere s(point3(-0.7, 0.4, -6), 1.3, matte);

        hit_record rec;
        UNIT_CHECK(hit_at(s, cam, 6.0, 6.0, time, rec));
        UNIT_CHECK(rec.has_differentials);
        UNIT_CHECK(rec.uv_footprint.valid);

        auto px = [&](double ds) {
            hit_record r;
            hit_at(s, cam, 6.0 + ds, 6.0, time, r);
            return r.p;
        };
        auto py = [&](double ds) {
            hit_record r;
            hit_at(s, cam, 6.0, 6.0 + ds, time, r);
            return r.p;
        };
        UNIT_CHECK(check_vec_deriv(rec.dpdx, px, h1, h2));
        UNIT_CHECK(check_vec_deriv(rec.dpdy, py, h1, h2));

        // Curvature must show up in the normal derivative (not a flat-plane zero).
        UNIT_CHECK(vec_norm(rec.dndx) > 1e-4);
        UNIT_CHECK(vec_norm(rec.dndy) > 1e-4);
        auto nx = [&](double ds) {
            hit_record r;
            hit_at(s, cam, 6.0 + ds, 6.0, time, r);
            return r.normal;
        };
        auto ny = [&](double ds) {
            hit_record r;
            hit_at(s, cam, 6.0, 6.0 + ds, time, r);
            return r.normal;
        };
        UNIT_CHECK(check_vec_deriv(rec.dndx, nx, h1, h2));
        UNIT_CHECK(check_vec_deriv(rec.dndy, ny, h1, h2));

        // Local UV derivatives, including seam-continuous numerics below.
        auto ux = [&](double ds) {
            hit_record r;
            hit_at(s, cam, 6.0 + ds, 6.0, time, r);
            return unwrap(r.u, rec.u);
        };
        auto vx = [&](double ds) {
            hit_record r;
            hit_at(s, cam, 6.0 + ds, 6.0, time, r);
            return r.v;
        };
        auto uy = [&](double ds) {
            hit_record r;
            hit_at(s, cam, 6.0, 6.0 + ds, time, r);
            return unwrap(r.u, rec.u);
        };
        auto vy = [&](double ds) {
            hit_record r;
            hit_at(s, cam, 6.0, 6.0 + ds, time, r);
            return r.v;
        };
        UNIT_CHECK(check_scalar_deriv(rec.uv_footprint.dudx, ux, h1, h2));
        UNIT_CHECK(check_scalar_deriv(rec.uv_footprint.dvdx, vx, h1, h2));
        UNIT_CHECK(check_scalar_deriv(rec.uv_footprint.dudy, uy, h1, h2));
        UNIT_CHECK(check_scalar_deriv(rec.uv_footprint.dvdy, vy, h1, h2));
    }

    // Meridian seam: analytic u derivative stays local while a raw wrapped
    // difference spans the whole [0,1) range. Rays are aimed explicitly at the
    // (-x) seam meridian from a viewpoint where it faces the camera; a small
    // search lands just beside u=0 so one finite-difference step crosses it.
    {
        auto matte = make_shared<lambertian>(color(.5,.5,.5));
        sphere s(point3(0.5, 0, -6), 1.0, matte);
        const point3 O(-3.0, 0.2, -3.0);
        const vec3 ddx(1, 0, 0);
        const vec3 ddy(0, 1, 0);

        bool found = false;
        hit_record rec;
        for (double a = 0.0; a > -0.3 && !found; a -= 0.002) {
            point3 aim(-0.5 + a, 0.0, -6.0);
            ray base(O, aim - O, time);
            base.has_differentials = true;
            base.dd_dx = ddx;
            base.dd_dy = ddy;
            if (!s.hit(base, interval(0.001, infinity), rec)) continue;
            double seam_distance = std::fmin(rec.u, 1.0 - rec.u);
            if (seam_distance <= 0.002 || seam_distance >= 0.015) continue;
            found = true;

            UNIT_CHECK(rec.uv_footprint.valid);
            UNIT_CHECK(std::fabs(rec.uv_footprint.dudx) < 1.0);

            const double H1 = 0.02, H2 = 0.005;
            // The coarser step straddles the seam: a raw wrapped quotient sees
            // the full +/-1 jump instead of the local derivative.
            hit_record rp, rm;
            ray rplus(O, (point3(-0.5 + a, 0.0, -6.0) - O) + H1 * ddx, time);
            ray rminus(O, (point3(-0.5 + a, 0.0, -6.0) - O) - H1 * ddx, time);
            UNIT_CHECK(s.hit(rplus, interval(0.001, infinity), rp));
            UNIT_CHECK(s.hit(rminus, interval(0.001, infinity), rm));
            double wrapped = std::fabs((rp.u - rm.u) / (2 * H1));
            UNIT_CHECK(wrapped > 0.5);   // dominated by the +/-1 wrap jump

            auto local = [&](double ds) {
                ray rr(O, (point3(-0.5 + a, 0.0, -6.0) - O) + ds * ddx, time);
                rr.has_differentials = true;
                rr.dd_dx = ddx;
                hit_record r;
                s.hit(rr, interval(0.001, infinity), r);
                return unwrap(r.u, rec.u);
            };
            UNIT_CHECK(check_scalar_deriv(rec.uv_footprint.dudx,
                                          local, H1, H2, 5.0));
        }
        UNIT_CHECK(found);
    }

    // Pole: a ray aimed exactly through the top pole invalidates only the UV
    // footprint; position and normal differentials stay defined. The check is
    // independent of any camera frustum.
    {
        auto matte = make_shared<lambertian>(color(.5,.5,.5));
        sphere s(point3(0, 0, -6), 1.0, matte);
        const point3 O(0, 3, -3);
        const vec3 D(0, -2, -3);              // passes through (0,1,-6), the pole
        const vec3 ddx(0.02, 0.0, 0.01);
        const vec3 ddy(0.01, 0.02, 0.0);
        ray pole_ray(O, D, time);
        pole_ray.has_differentials = true;
        pole_ray.dd_dx = ddx;
        pole_ray.dd_dy = ddy;

        hit_record rec;
        UNIT_CHECK(s.hit(pole_ray, interval(0.001, infinity), rec));
        UNIT_CHECK(rec.has_differentials);
        UNIT_CHECK(!rec.uv_footprint.valid);
        UNIT_CHECK(near(rec.uv_footprint.dudx, 0));
        UNIT_CHECK(near(rec.uv_footprint.dvdy, 0));
        UNIT_CHECK(near(rec.normal.y(), 1.0, 1e-9));
        UNIT_CHECK(vec_norm(rec.dpdx) > 0 || vec_norm(rec.dpdy) > 0);

        auto px = [&](double ds) {
            ray rr(O, D + ds * ddx, time);
            rr.has_differentials = true;
            hit_record r;
            s.hit(rr, interval(0.001, infinity), r);
            return r.p;
        };
        auto py = [&](double ds) {
            ray rr(O, D + ds * ddy, time);
            rr.has_differentials = true;
            hit_record r;
            s.hit(rr, interval(0.001, infinity), r);
            return r.p;
        };
        auto nx = [&](double ds) {
            ray rr(O, D + ds * ddx, time);
            rr.has_differentials = true;
            hit_record r;
            s.hit(rr, interval(0.001, infinity), r);
            return r.normal;
        };
        UNIT_CHECK(check_vec_deriv(rec.dpdx, px, h1, h2));
        UNIT_CHECK(check_vec_deriv(rec.dpdy, py, h1, h2));
        UNIT_CHECK(check_vec_deriv(rec.dndx, nx, h1, h2));
    }

    // Moving sphere: center velocity enters both point and normal derivatives.
    {
        camera cam = make_camera();
        auto matte = make_shared<lambertian>(color(.5,.5,.5));
        sphere s(point3(0.3, -0.2, -6), point3(0.9, 0.5, -6.4), 1.1, matte);

        hit_record rec;
        UNIT_CHECK(hit_at(s, cam, 9.0, 8.0, time, rec));
        UNIT_CHECK(rec.has_differentials && rec.uv_footprint.valid);
        auto px = [&](double ds) {
            hit_record r;
            hit_at(s, cam, 9.0 + ds, 8.0, time, r);
            return r.p;
        };
        auto ny = [&](double ds) {
            hit_record r;
            hit_at(s, cam, 9.0, 8.0 + ds, time, r);
            return r.normal;
        };
        UNIT_CHECK(check_vec_deriv(rec.dpdx, px, h1, h2));
        UNIT_CHECK(check_vec_deriv(rec.dndy, ny, h1, h2));
        auto ux = [&](double ds) {
            hit_record r;
            hit_at(s, cam, 9.0 + ds, 8.0, time, r);
            return unwrap(r.u, rec.u);
        };
        UNIT_CHECK(check_scalar_deriv(rec.uv_footprint.dudx, ux, h1, h2));
    }

    // Quad: planar point derivatives, constant (zero) normal derivative,
    // linear UV derivatives.
    {
        camera cam = make_camera();
        auto matte = make_shared<lambertian>(color(.5,.5,.5));
        quad q(point3(-3, -3, -7), vec3(6, 0, 0), vec3(0, 6, 0), matte);

        hit_record rec;
        UNIT_CHECK(hit_at(q, cam, 8.0, 8.0, time, rec));
        UNIT_CHECK(rec.has_differentials && rec.uv_footprint.valid);
        UNIT_CHECK(vec_near(rec.dndx, vec3(0,0,0)));
        UNIT_CHECK(vec_near(rec.dndy, vec3(0,0,0)));
        auto px = [&](double ds) {
            hit_record r;
            hit_at(q, cam, 8.0 + ds, 8.0, time, r);
            return r.p;
        };
        auto py = [&](double ds) {
            hit_record r;
            hit_at(q, cam, 8.0, 8.0 + ds, time, r);
            return r.p;
        };
        UNIT_CHECK(check_vec_deriv(rec.dpdx, px, h1, h2));
        UNIT_CHECK(check_vec_deriv(rec.dpdy, py, h1, h2));
        auto ux = [&](double ds) {
            hit_record r;
            hit_at(q, cam, 8.0 + ds, 8.0, time, r);
            return r.u;
        };
        auto vy = [&](double ds) {
            hit_record r;
            hit_at(q, cam, 8.0, 8.0 + ds, time, r);
            return r.v;
        };
        UNIT_CHECK(check_scalar_deriv(rec.uv_footprint.dudx, ux, h1, h2));
        UNIT_CHECK(check_scalar_deriv(rec.uv_footprint.dvdy, vy, h1, h2));
        UNIT_CHECK(near(rec.uv_footprint.dvdx, 0.0, 1e-8));
        UNIT_CHECK(near(rec.uv_footprint.dudy, 0.0, 1e-8));
    }

    // Rotate + translate wrappers preserve the differentials end to end.
    {
        camera cam = make_camera();
        auto matte = make_shared<lambertian>(color(.5,.5,.5));
        auto inner = make_shared<sphere>(point3(0, 0, 0), 1.2, matte);
        shared_ptr<hittable> world =
            make_shared<translate>(make_shared<rotate_y>(inner, 35),
                                   vec3(0.3, -0.2, -6));
        hit_record rec;
        UNIT_CHECK(hit_at(*world, cam, 8.0, 8.0, time, rec));
        UNIT_CHECK(rec.has_differentials && rec.uv_footprint.valid);
        auto px = [&](double ds) {
            hit_record r;
            hit_at(*world, cam, 8.0 + ds, 8.0, time, r);
            return r.p;
        };
        auto nx = [&](double ds) {
            hit_record r;
            hit_at(*world, cam, 8.0 + ds, 8.0, time, r);
            return r.normal;
        };
        auto vy = [&](double ds) {
            hit_record r;
            hit_at(*world, cam, 8.0, 8.0 + ds, time, r);
            return r.v;
        };
        UNIT_CHECK(check_vec_deriv(rec.dpdx, px, h1, h2));
        UNIT_CHECK(check_vec_deriv(rec.dndx, nx, h1, h2));
        UNIT_CHECK(check_scalar_deriv(rec.uv_footprint.dvdy, vy, h1, h2));
    }

    // BVH and list traversal return identical differential content.
    {
        camera cam = make_camera();
        auto matte = make_shared<lambertian>(color(.5,.5,.5));
        hittable_list list;
        list.add(make_shared<sphere>(point3(1.5, 1.0, -5), 0.6, matte));
        list.add(make_shared<sphere>(point3(-0.7, 0.4, -6), 1.3, matte));
        list.add(make_shared<quad>(point3(-3,-3,-8), vec3(6,0,0),
                                   vec3(0,6,0), matte));
        bvh_node tree(list);

        hit_record a, b;
        UNIT_CHECK(hit_at(list, cam, 6.0, 6.0, time, a));
        UNIT_CHECK(hit_at(tree, cam, 6.0, 6.0, time, b));
        UNIT_CHECK(a.has_differentials && b.has_differentials);
        UNIT_CHECK(vec_near(a.dpdx, b.dpdx, 1e-12));
        UNIT_CHECK(vec_near(a.dndy, b.dndy, 1e-12));
        UNIT_CHECK(near(a.uv_footprint.dudx, b.uv_footprint.dudx, 1e-12));
    }

    // Record reuse: a plain ray after a differentiated one leaves no residue.
    {
        camera cam = make_camera();
        auto matte = make_shared<lambertian>(color(.5,.5,.5));
        sphere s(point3(-0.7, 0.4, -6), 1.3, matte);
        hit_record rec;
        UNIT_CHECK(hit_at(s, cam, 6.0, 6.0, time, rec));
        UNIT_CHECK(rec.has_differentials && rec.uv_footprint.valid);

        ray plain = cam.sample_ray(6.0, 6.0, 0, 0, time);
        plain.clear_differentials();
        UNIT_CHECK(hit_at(s, cam, 6.0, 6.0, time, rec, &plain));
        UNIT_CHECK(!rec.has_differentials);
        UNIT_CHECK(!rec.uv_footprint.valid);
        UNIT_CHECK(vec_near(rec.dpdx, vec3(0,0,0)));
        UNIT_CHECK(vec_near(rec.dndy, vec3(0,0,0)));
        UNIT_CHECK(near(rec.uv_footprint.dudy, 0));

        // A miss must also clear the record.
        ray miss(point3(0,0,0), vec3(100, 100, -1), time);
        miss.has_differentials = true;
        miss.dd_dx = vec3(1, 2, 3);
        UNIT_CHECK(!s.hit(miss, interval(0.001, infinity), rec));
        UNIT_CHECK(!rec.has_differentials);
        UNIT_CHECK(vec_near(rec.dpdx, vec3(0,0,0)));
        UNIT_CHECK(!rec.uv_footprint.valid);
    }

    // Tangent (grazing) hit: differentials explicitly invalidated.
    {
        auto matte = make_shared<lambertian>(color(.5,.5,.5));
        sphere s(point3(0, 0, -5), 1.0, matte);
        // Ray grazing the sphere exactly along its top tangent: q.D = 0 there.
        ray tangent(point3(0, 1.0, 0), vec3(0, 0, -1), time);
        tangent.has_differentials = true;
        tangent.dd_dx = vec3(0.05, 0, 0);
        tangent.dd_dy = vec3(0, 0.05, 0);
        hit_record rec;
        UNIT_CHECK(s.hit(tangent, interval(0.001, infinity), rec));
        UNIT_CHECK(!rec.has_differentials);
        UNIT_CHECK(!rec.uv_footprint.valid);
        UNIT_CHECK(vec_near(rec.dpdx, vec3(0,0,0)));

        // Volume scattering has no surface differential.
        auto boundary = make_shared<sphere>(point3(0, 0, -4), 1.5, matte);
        constant_medium fog(boundary, 2.0, color(.8,.8,.8));
        ray vr(point3(0,0,0), vec3(0,0,-1), time);
        vr.has_differentials = true;
        vr.dd_dx = vec3(0.05,0,0);
        hit_record vrec;
        std::srand(777);
        bool volume_hit = fog.hit(vr, interval(0.001, infinity), vrec);
        UNIT_CHECK(volume_hit);
        UNIT_CHECK(!vrec.has_differentials);
        UNIT_CHECK(!vrec.uv_footprint.valid);
        UNIT_CHECK(vec_near(vrec.dpdx, vec3(0,0,0)));
    }
}


//----------------------------------------------------------------------------------------------
// specular group
//----------------------------------------------------------------------------------------------

// Follow the skip_pdf (specular) chain of a ray. The same call drives both the
// analytic path and the perturbed finite-difference paths; RNG state is reset
// up front so Fresnel picks the identical branch every time.
ray specular_chain(const ray& r0, const hittable& world, int bounces,
                   unsigned seed) {
    ray cur = r0;
    for (int b = 0; b < bounces; b++) {
        hit_record rec;
        if (!world.hit(cur, interval(0.001, infinity), rec))
            break;
        scatter_record srec;
        std::srand(seed);
        if (!rec.mat || !rec.mat->scatter(cur, rec, srec))
            break;
        if (!srec.skip_pdf)
            break;
        cur = srec.skip_pdf_ray;
    }
    return cur;
}

void test_specular() {
    const double h1 = 1e-2, h2 = 2.5e-3;
    camera cam = make_camera();

    // Two ideal mirrors in series: differentials survive both reflections.
    {
        auto mirror = make_shared<metal>(color(.9,.9,.9), 0.0);
        hittable_list world;
        world.add(make_shared<quad>(point3(-8,-8,-6), vec3(16,0,0),
                                    vec3(0,16,0), mirror));
        world.add(make_shared<sphere>(point3(2.5, 0, -3), 0.8, mirror));

        const double px = 8.0, py = 8.0;
        ray r0 = cam.sample_ray(px, py, 0, 0, 0.4);
        ray final_ray = specular_chain(r0, world, 3, 4242);
        UNIT_CHECK(final_ray.has_differentials);

        auto fx = [&](double ds) {
            ray r = offset_ray(cam, px, py, ds, 0, 0, 0.4);
            return specular_chain(r, world, 3, 4242).direction();
        };
        auto fy = [&](double ds) {
            ray r = offset_ray_y(cam, px, py, ds, 0, 0, 0.4);
            return specular_chain(r, world, 3, 4242).direction();
        };
        UNIT_CHECK(check_vec_deriv(final_ray.dd_dx, fx, h1, h2));
        UNIT_CHECK(check_vec_deriv(final_ray.dd_dy, fy, h1, h2));

        auto ox = [&](double ds) {
            ray r = offset_ray(cam, px, py, ds, 0, 0, 0.4);
            return specular_chain(r, world, 3, 4242).origin();
        };
        UNIT_CHECK(check_vec_deriv(final_ray.do_dx, ox, h1, h2));
    }

    // Fuzzed metal drops the footprint, central ray still scatters.
    {
        auto fuzzed = make_shared<metal>(color(.9,.9,.9), 0.4);
        sphere s(point3(0, 0, -5), 1.0, fuzzed);
        ray r0 = cam.sample_ray(8, 8, 0, 0, 0.4);
        hit_record rec;
        UNIT_CHECK(s.hit(r0, interval(0.001, infinity), rec));
        scatter_record srec;
        std::srand(11);
        UNIT_CHECK(rec.mat->scatter(r0, rec, srec));
        UNIT_CHECK(srec.skip_pdf && !srec.skip_pdf_ray.has_differentials);
        UNIT_CHECK(vec_near(srec.skip_pdf_ray.dd_dx, vec3(0,0,0)));
    }

    // Dielectric flat interface: refraction, total internal reflection and the
    // critical point. Finite differences perturb the incident direction along
    // the stored screen differential (d(s) = d0 + s*dd) with time and the
    // Fresnel branch locked, so the difference quotient estimates the same
    // screen derivative stored on the scattered ray.
    {
        auto glass = make_shared<dielectric>(1.5);
        quad interface(point3(-50,-50,-2), vec3(100,0,0), vec3(0,100,0), glass);

        const vec3 dd_x(0.02, 0.01, 0.0);
        const vec3 dd_y(-0.01, 0.02, 0.005);
        auto build = [&](double theta, const vec3* perturb = nullptr) {
            // Ray approaches the back face from the glass side.
            vec3 d(std::sin(theta), 0, std::cos(theta));
            if (perturb) d = d + *perturb;
            ray r(point3(0, 0, -6), d, 0.4);
            r.has_differentials = true;
            r.dd_dx = dd_x;
            r.dd_dy = dd_y;
            return r;
        };
        auto scatter_once = [&](const ray& r) {
            hit_record rec;
            bool h = interface.hit(r, interval(0.001, infinity), rec);
            scatter_record srec;
            std::srand(99);
            UNIT_CHECK(h && rec.mat->scatter(r, rec, srec) && srec.skip_pdf);
            return srec;
        };

        // Shallow angle: refraction with valid differentials.
        {
            const double theta = 30.0 * PI / 180.0;
            ray r = build(theta);
            scatter_record srec = scatter_once(r);
            UNIT_CHECK(srec.skip_pdf_ray.has_differentials);
            auto fx = [&](double ds) {
                vec3 pert = ds * dd_x;
                ray rr = build(theta, &pert);
                hit_record rec;
                interface.hit(rr, interval(0.001, infinity), rec);
                scatter_record s;
                std::srand(99);
                rec.mat->scatter(rr, rec, s);
                return s.skip_pdf_ray.direction();
            };
            auto fy = [&](double ds) {
                vec3 pert = ds * dd_y;
                ray rr = build(theta, &pert);
                hit_record rec;
                interface.hit(rr, interval(0.001, infinity), rec);
                scatter_record s;
                std::srand(99);
                rec.mat->scatter(rr, rec, s);
                return s.skip_pdf_ray.direction();
            };
            UNIT_CHECK(check_vec_deriv(srec.skip_pdf_ray.dd_dx, fx, h1, h2, 5.0));
            UNIT_CHECK(check_vec_deriv(srec.skip_pdf_ray.dd_dy, fy, h1, h2, 5.0));
        }

        // Steep angle: total internal reflection follows the reflection branch
        // with propagated differentials.
        {
            const double theta = 55.0 * PI / 180.0;
            ray r = build(theta);
            scatter_record srec = scatter_once(r);
            UNIT_CHECK(srec.skip_pdf_ray.has_differentials);
            vec3 expected = reflect(unit_vector(r.direction()),
                                    vec3(0,0,-1));   // back-face record normal
            UNIT_CHECK(vec_near(srec.skip_pdf_ray.direction(), expected, 1e-9));
            auto fx = [&](double ds) {
                vec3 pert = ds * dd_x;
                ray rr = build(theta, &pert);
                hit_record rec;
                interface.hit(rr, interval(0.001, infinity), rec);
                scatter_record s;
                std::srand(99);
                rec.mat->scatter(rr, rec, s);
                return s.skip_pdf_ray.direction();
            };
            UNIT_CHECK(check_vec_deriv(srec.skip_pdf_ray.dd_dx, fx, h1, h2));
        }

        // Near-critical refraction band: the refraction mapping stays defined
        // (eta sin < 1) but its derivative diverges as cos_t -> 0. Just inside
        // the critical angle the differentials must be cleared while the
        // central ray still refracts. A reflected Fresnel draw is handled too.
        {
            const double sin_i = 2.0 / 3.0 - 2e-9;
            const double theta = std::asin(sin_i);
            ray r = build(theta);
            scatter_record srec = scatter_once(r);
            vec3 refract_dir = refract(unit_vector(r.direction()),
                                       vec3(0,0,-1), 1.5);
            bool took_refraction =
                vec_near(srec.skip_pdf_ray.direction(), refract_dir, 1e-9);
            if (took_refraction) {
                UNIT_CHECK(!srec.skip_pdf_ray.has_differentials);
                UNIT_CHECK(vec_near(srec.skip_pdf_ray.dd_dy, vec3(0,0,0)));
            } else {
                // The Fresnel draw chose reflection: that branch remains
                // differentiable, but the refraction helper itself must still
                // refuse the non-differentiable point.
                UNIT_CHECK(srec.skip_pdf_ray.has_differentials);
            }
            vec3 ignored;
            UNIT_CHECK(!specular_diff::refract_deriv(
                unit_vector(r.direction()), dd_x, vec3(0,0,-1),
                vec3(0,0,0), 1.5, ignored));

            // A healthy refraction, in contrast, is differentiable end to end.
            vec3 tdx;
            UNIT_CHECK(specular_diff::refract_deriv(
                unit_vector(r.direction()), dd_x, vec3(0,0,-1),
                vec3(0,0,0), 1.35, tdx));
        }
    }

    // Full glass sphere: two refractions (entry and exit), checked numerically.
    {
        auto glass = make_shared<dielectric>(1.5);
        sphere ball(point3(0, 0, -5), 1.0, glass);
        ray r0 = cam.sample_ray(8.0, 8.0, 0, 0, 0.4);
        ray out = specular_chain(r0, ball, 2, 5);
        UNIT_CHECK(out.has_differentials);
        auto fx = [&](double ds) {
            ray r = offset_ray(cam, 8.0, 8.0, ds, 0, 0, 0.4);
            return specular_chain(r, ball, 2, 5).direction();
        };
        auto fy = [&](double ds) {
            ray r = offset_ray_y(cam, 8.0, 8.0, ds, 0, 0, 0.4);
            return specular_chain(r, ball, 2, 5).direction();
        };
        UNIT_CHECK(check_vec_deriv(out.dd_dx, fx, h1, h2, 4.0));
        UNIT_CHECK(check_vec_deriv(out.dd_dy, fy, h1, h2, 4.0));
    }

    // Lambertian and isotropic scattering clear the footprint.
    {
        auto diffuse = make_shared<lambertian>(color(.7,.7,.7));
        sphere s(point3(0, 0, -5), 1.0, diffuse);
        ray r0 = cam.sample_ray(8, 8, 0, 0, 0.4);
        hit_record rec;
        UNIT_CHECK(s.hit(r0, interval(0.001, infinity), rec));
        scatter_record srec;
        std::srand(3);
        UNIT_CHECK(rec.mat->scatter(r0, rec, srec));
        UNIT_CHECK(!srec.skip_pdf);
        UNIT_CHECK(!srec.skip_pdf_ray.has_differentials);

        auto iso = make_shared<isotropic>(color(.5,.5,.5));
        auto boundary = make_shared<sphere>(point3(0,0,-5), 1.0, iso);
        constant_medium medium(boundary, 3.0, color(.5,.5,.5));
        ray vr = r0;
        hit_record vrec;
        std::srand(4);
        if (medium.hit(vr, interval(0.001, infinity), vrec)) {
            scatter_record vsrec;
            UNIT_CHECK(vrec.mat->scatter(vr, vrec, vsrec));
            UNIT_CHECK(!vsrec.skip_pdf_ray.has_differentials);
        }
    }

    // Scattering consumes the same random numbers with and without differentials:
    // the Fresnel draw happens once, on the central branch only.
    {
        auto glass = make_shared<dielectric>(1.5);
        sphere ball(point3(0, 0, -5), 1.0, glass);
        ray r0 = cam.sample_ray(8, 8, 0, 0, 0.4);
        hit_record rec;
        UNIT_CHECK(ball.hit(r0, interval(0.001, infinity), rec));

        std::srand(2024);
        scatter_record a;
        rec.mat->scatter(r0, rec, a);
        int after_on = std::rand();

        ray plain = r0;
        plain.clear_differentials();
        std::srand(2024);
        scatter_record b;
        rec.mat->scatter(plain, rec, b);
        int after_off = std::rand();
        UNIT_CHECK(after_on == after_off);
        UNIT_CHECK(vec_near(a.skip_pdf_ray.direction(),
                            b.skip_pdf_ray.direction(), 1e-12));
    }
}

// Small helper namespace boundary: test_specular ends above; texture tests follow.


//----------------------------------------------------------------------------------------------
// texture group
//----------------------------------------------------------------------------------------------

std::vector<color> make_pixels(int w, int h,
                               double (*fn)(int, int, int, int)) {
    std::vector<color> out(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            out[static_cast<size_t>(y) * w + x] =
                color(fn(x, y, w, h), fn(x, y, w, h) * 0.5,
                      fn(x, y, w, h) * 0.25);
    return out;
}

double asym_color(int x, int y, int w, int h) {
    return (x + 1.0) / (w + 1.0) + 0.137 * (y + 1.0) / (h + 1.0);
}

double stripe_color(int x, int, int, int) {
    return (x % 2 == 0) ? 1.0 : 0.0;
}

void test_texture() {
    // Construction validation.
    {
        std::vector<color> ok(4, color(1, 1, 1));
        bool threw = false;
        try { mip_image_texture good(2, 2, ok); (void)good; }
        catch (...) { threw = true; }
        UNIT_CHECK(!threw);
        threw = false;
        try { mip_image_texture(0, 2, ok); } catch (...) { threw = true; }
        UNIT_CHECK(threw);
        threw = false;
        try { mip_image_texture(3, 2, ok); } catch (...) { threw = true; }
        UNIT_CHECK(threw);                                   // not power of two
        threw = false;
        try { mip_image_texture(4, 4, ok); } catch (...) { threw = true; }
        UNIT_CHECK(threw);                                   // wrong length
        std::vector<color> bad = ok;
        bad[1] = color(1, std::nan(""), 1);
        threw = false;
        try { mip_image_texture(2, 2, bad); } catch (...) { threw = true; }
        UNIT_CHECK(threw);                                   // non-finite
        bad[1] = color(1, -0.1, 1);
        threw = false;
        try { mip_image_texture(2, 2, bad); } catch (...) { threw = true; }
        UNIT_CHECK(threw);                                   // negative
    }

    // Rectangular 4x2 pyramid: hand-check each mip level's means.
    {
        std::vector<color> px = make_pixels(4, 2, asym_color);
        mip_image_texture tex(4, 2, px);

        // Level 0 bilinear at a texel center returns that texel exactly.
        color c00 = px[0];
        UNIT_CHECK(vec_near(tex.value(0.125, 0.75, point3(0,0,0)), c00, 1e-12));

        // Level 1 is 2x1; along V the single edge row is repeated. Query at the
        // level-1 texel center (u=.25, v=.5) to read dst[0] without blending.
        color mean01 = (px[0] + px[1] + px[4] + px[5]) / 4.0;
        texture_footprint fp;
        fp.valid = true;
        fp.dudx = 2.0 / 4.0;      // rho = 2 -> LOD 1
        fp.dvdx = 0.0;
        fp.dudy = 0.0;
        fp.dvdy = 2.0 / 2.0;
        UNIT_CHECK(vec_near(tex.value(0.25, 0.5, point3(0,0,0), fp),
                            mean01, 1e-12));

        // Level 2 is 1x1: average of everything.
        color total(0,0,0);
        for (size_t i = 0; i < px.size(); i++) total += px[i];
        color avg = total / static_cast<double>(px.size());
        texture_footprint fp2;
        fp2.valid = true;
        fp2.dudx = 4.0 / 4.0;     // rho = 4 -> LOD 2 (top level)
        fp2.dvdy = 4.0 / 2.0;
        UNIT_CHECK(vec_near(tex.value(0.25, 0.5, point3(0,0,0), fp2),
                            avg, 1e-12));

        // LOD 1.5 must be the exact midpoint of the level-1 and level-2 samples.
        texture_footprint fp3;
        fp3.valid = true;
        fp3.dudx = std::sqrt(8.0) / 4.0;   // rho = sqrt(8) -> LOD 1.5
        color l1 = tex.value(0.25, 0.5, point3(0,0,0), fp);
        color l2 = tex.value(0.25, 0.5, point3(0,0,0), fp2);
        UNIT_CHECK(vec_near(tex.value(0.25, 0.5, point3(0,0,0), fp3),
                            (l1 + l2) / 2.0, 1e-12));
    }

    // Invalid / non-finite footprints and the plain overload use level 0,
    // and never mutate the texture.
    {
        std::vector<color> px = make_pixels(4, 4, asym_color);
        mip_image_texture tex(4, 4, px);
        texture_footprint bad;
        bad.valid = true;
        bad.dudx = std::nan("");
        color a = tex.value(0.3, 0.6, point3(0,0,0), bad);
        color b = tex.value(0.3, 0.6, point3(0,0,0));
        UNIT_CHECK(vec_near(a, b, 1e-12));
        UNIT_CHECK(!bad.valid || std::isnan(bad.dudx));   // query left it untouched

        texture_footprint off;
        color c = tex.value(0.3, 0.6, point3(0,0,0), off);
        UNIT_CHECK(vec_near(c, b, 1e-12));
    }

    // U wraps across the seam; V clamps to the edge.
    {
        std::vector<color> px(16, color(0,0,0));
        px[0] = color(1, 0, 0);     // top-left texel
        px[12] = color(0, 1, 0);    // bottom-left texel
        mip_image_texture tex(4, 4, px);
        // Just left of the seam wraps to the rightmost column, not to edge clamp.
        color wrapped = tex.value(-0.01, 0.75, point3(0,0,0));
        color inside = tex.value(0.99, 0.75, point3(0,0,0));
        UNIT_CHECK(vec_near(wrapped, inside, 0.05));
        // V above the top texel center clamps to the top row (red present).
        color top = tex.value(0.125, 0.999, point3(0,0,0));
        UNIT_CHECK(top.x() > top.y());
    }

    // Fine stripes: a wide footprint averages toward the midpoint color,
    // while the level-0 sample keeps the crisp stripe value.
    {
        std::vector<color> px = make_pixels(8, 2, stripe_color);
        mip_image_texture tex(8, 2, px);
        texture_footprint wide;
        wide.valid = true;
        wide.dudx = 8.0 / 8.0;      // rho 8 -> top level, average = 0.5
        color filtered = tex.value(0.5, 0.5, point3(0,0,0), wide);
        UNIT_CHECK(near(filtered.x(), 0.5, 1e-12));
        color crisp = tex.value(0.0625, 0.75, point3(0,0,0));
        UNIT_CHECK(near(crisp.x(), 1.0, 1e-12));
    }
}


//----------------------------------------------------------------------------------------------
// pipeline group
//----------------------------------------------------------------------------------------------

class recording_texture : public texture {
  public:
    recording_texture(shared_ptr<mip_image_texture> inner) : inner(inner) {}

    color value(double u, double v, const point3& p) const override {
        invalid_count++;
        calls++;
        return inner->value(u, v, p);
    }

    color value(double u, double v, const point3& p,
                const texture_footprint& fp) const override {
        calls++;
        if (fp.valid && fp.all_finite())
            valid_count++;
        else
            invalid_count++;
        last_valid = fp.valid && fp.all_finite();
        last_dudx = fp.dudx;
        return inner->value(u, v, p, fp);
    }

    shared_ptr<mip_image_texture> inner;
    mutable int calls = 0;
    mutable int valid_count = 0;
    mutable int invalid_count = 0;
    mutable bool last_valid = false;
    mutable double last_dudx = 0.0;
};

void test_pipeline() {
    // Run a real camera.render and report what the recording texture saw.
    struct counts { int calls; int valid; int invalid; };
    auto run_render = [](camera& cam, const hittable& world,
                         const hittable& lights,
                         const shared_ptr<recording_texture>& tex,
                         unsigned seed) {
        std::srand(seed);
        std::ostringstream sink;
        auto* old = std::cout.rdbuf(sink.rdbuf());
        cam.render(world, lights);
        std::cout.rdbuf(old);
        counts c{ tex->calls, tex->valid_count, tex->invalid_count };
        return c;
    };

    auto base_cam = []() {
        camera cam;
        cam.image_width = 12;
        cam.aspect_ratio = 1.0;
        cam.samples_per_pixel = 1;
        cam.max_depth = 6;
        cam.background = color(0, 0, 0);
        cam.vfov = 50;
        cam.lookfrom = point3(0, 0, 0);
        cam.lookat = point3(0, 0, -1);
        cam.vup = vec3(0, 1, 0);
        cam.defocus_angle = 0;
        cam.focus_dist = 8;
        cam.enable_differentials = true;
        return cam;
    };

    // Route 1: camera -> ideal mirror -> textured side wall. Rays that miss
    // the mirror return background and never query a texture, so every texture
    // query in this world is one whose footprint crossed the mirror.
    {
        auto tex = make_shared<recording_texture>(make_shared<mip_image_texture>(
            8, 4, make_pixels(8, 4, stripe_color)));
        auto mirror = make_shared<metal>(color(.95,.95,.95), 0.0);
        auto light_mat = make_shared<diffuse_light>(color(10, 10, 10));

        hittable_list world;
        // 45-degree mirror: record normal (-1,0,-1)/sqrt(2), plane x + z = -9.
        world.add(make_shared<quad>(point3(-7,-7,-2), vec3(0,14,0),
                                    vec3(10,0,-10), mirror));
        // Side wall at x = 6, only reachable after the +x reflection.
        world.add(make_shared<quad>(point3(6,-7,-14), vec3(0,14,0),
                                    vec3(0,0,10),
                                    make_shared<lambertian>(tex)));
        auto light_quad = make_shared<quad>(point3(6,-1,-9), vec3(0,2,0),
                                            vec3(0,0,2), light_mat);
        world.add(light_quad);
        hittable_list lights;
        lights.add(light_quad);

        camera cam = base_cam();
        counts c = run_render(cam, world, lights, tex, 101);
        UNIT_CHECK(c.calls > 0);
        // Every first-hit wall query crossed the mirror, so at least one query
        // must carry a valid footprint (later diffuse re-hits may be invalid).
        UNIT_CHECK(c.valid > 0);
    }

    // Route 2: camera -> glass sphere -> textured back wall. The sphere fills
    // the frustum, so every wall query arrives through two refractions.
    {
        auto tex = make_shared<recording_texture>(make_shared<mip_image_texture>(
            8, 4, make_pixels(8, 4, stripe_color)));
        auto glass = make_shared<dielectric>(1.5);
        auto light_mat = make_shared<diffuse_light>(color(10, 10, 10));

        hittable_list world;
        world.add(make_shared<quad>(point3(-8,-8,-10), vec3(16,0,0),
                                    vec3(0,16,0),
                                    make_shared<lambertian>(tex)));
        world.add(make_shared<sphere>(point3(0, 0, -3.5), 2.8, glass));
        auto light_quad = make_shared<quad>(point3(-1, 3.0, -9.9),
                                            vec3(2,0,0), vec3(0,0,2), light_mat);
        world.add(light_quad);
        hittable_list lights;
        lights.add(light_quad);

        camera cam = base_cam();
        counts c = run_render(cam, world, lights, tex, 202);
        UNIT_CHECK(c.calls > 0);
        // The first wall query of every camera ray traversed two refractions,
        // so valid footprints reach the wall through the glass.
        UNIT_CHECK(c.valid > 0);
    }

    // Route 3: camera -> diffuse sphere -> textured wall. Wall queries reached
    // after a Lambertian bounce must carry an invalid footprint (direct wall
    // hits stay valid; the assertion targets the invalidated bounce queries).
    {
        auto tex = make_shared<recording_texture>(make_shared<mip_image_texture>(
            8, 4, make_pixels(8, 4, stripe_color)));
        auto diffuse = make_shared<lambertian>(color(.8,.2,.2));
        auto light_mat = make_shared<diffuse_light>(color(10, 10, 10));

        hittable_list world;
        auto wall = make_shared<lambertian>(tex);
        // Enclosing textured walls so bounced rays always find a surface.
        world.add(make_shared<quad>(point3(-8,-8,-10), vec3(16,0,0),
                                    vec3(0,16,0), wall));
        world.add(make_shared<quad>(point3(-8,-8,-10), vec3(0,0,8),
                                    vec3(0,16,0), wall));
        world.add(make_shared<quad>(point3(8,-8,-10), vec3(0,0,8),
                                    vec3(0,16,0), wall));
        world.add(make_shared<sphere>(point3(0, 0, -3.0), 1.1, diffuse));
        auto light_quad = make_shared<quad>(point3(-1, 3.0, -9.9),
                                            vec3(2,0,0), vec3(0,0,2), light_mat);
        world.add(light_quad);
        hittable_list lights;
        lights.add(light_quad);

        camera cam = base_cam();
        cam.samples_per_pixel = 4;
        cam.max_depth = 6;
        counts c = run_render(cam, world, lights, tex, 303);
        UNIT_CHECK(c.calls > 0);
        UNIT_CHECK(c.invalid > 0);
    }

    // Mixed scene rendered through the public camera.render entry point and
    // persisted as a PPM artifact; direct, mirror and glass queries coexist.
    {
        auto stripes_a = make_shared<mip_image_texture>(
            8, 4, make_pixels(8, 4, stripe_color));
        auto stripes_b = make_shared<mip_image_texture>(
            8, 4, make_pixels(8, 4, stripe_color));
        auto rec_a = make_shared<recording_texture>(stripes_a);
        auto rec_b = make_shared<recording_texture>(stripes_b);

        auto wall_mat = make_shared<lambertian>(rec_a);
        auto mirror_wall_mat = make_shared<lambertian>(rec_b);
        auto mirror = make_shared<metal>(color(.95,.95,.95), 0.0);
        auto glass = make_shared<dielectric>(1.5);
        auto diffuse = make_shared<lambertian>(color(.8,.2,.2));
        auto light_mat = make_shared<diffuse_light>(color(12, 12, 12));

        hittable_list world;
        // Back wall: seen directly and through the glass sphere.
        world.add(make_shared<quad>(point3(-6,-6,-10), vec3(12,0,0),
                                    vec3(0,12,0), wall_mat));
        // Small emissive area light (also the light-sampling PDF target).
        auto light_quad = make_shared<quad>(point3(-1.5, 4.0, -9.8),
                                            vec3(3,0,0), vec3(0,0,2), light_mat);
        world.add(light_quad);
        // Side wall reached only via the 45-degree mirror (reflected rays head +x).
        world.add(make_shared<quad>(point3(6,-6,-10), vec3(0,12,0),
                                    vec3(0,0,8), mirror_wall_mat));
        // Mirror tilted 45 degrees about the Y axis.
        world.add(make_shared<quad>(point3(-2,-2,-6), vec3(0,4,0),
                                    vec3(4,0,-4), mirror));
        // Glass sphere between camera and back wall.
        world.add(make_shared<sphere>(point3(-1.5, -1.0, -5), 1.0, glass));
        // Diffuse sphere: paths bouncing off it lose their footprint.
        world.add(make_shared<sphere>(point3(2.5, 1.5, -3.5), 0.9, diffuse));

        hittable_list lights;
        lights.add(light_quad);

        camera cam;
        cam.image_width = 24;
        cam.aspect_ratio = 1.0;
        cam.samples_per_pixel = 1;
        cam.max_depth = 8;
        cam.background = color(0, 0, 0);
        cam.vfov = 70;
        cam.lookfrom = point3(0, 0, 0);
        cam.lookat = point3(0, 0, -1);
        cam.vup = vec3(0, 1, 0);
        cam.defocus_angle = 0;
        cam.focus_dist = 10;
        cam.enable_differentials = true;

        std::srand(20240517);
        std::ostringstream image;
        auto* old = std::cout.rdbuf(image.rdbuf());
        cam.render(world, lights);
        std::cout.rdbuf(old);

        // Real footprint queries reached the walls, including filtered (nonzero
        // derivative) ones, and diffuse bounces produced invalidated queries.
        UNIT_CHECK(rec_a->calls > 0);
        UNIT_CHECK(rec_a->valid_count > 0);
        UNIT_CHECK(rec_a->invalid_count > 0);
        UNIT_CHECK(rec_b->calls > 0);
        UNIT_CHECK(rec_b->valid_count > 0);

        // Default-off render: every query carries an invalid footprint.
        auto stripes_c = make_shared<mip_image_texture>(
            8, 4, make_pixels(8, 4, stripe_color));
        auto rec_c = make_shared<recording_texture>(stripes_c);
        auto light_c = make_shared<diffuse_light>(color(8, 8, 8));
        auto light_quad_c = make_shared<quad>(point3(-1, 4.0, -9.8),
                                              vec3(2,0,0), vec3(0,0,2), light_c);
        hittable_list world2;
        world2.add(make_shared<quad>(point3(-6,-6,-10), vec3(12,0,0),
                                     vec3(0,12,0),
                                     make_shared<lambertian>(rec_c)));
        world2.add(light_quad_c);
        hittable_list lights2;
        lights2.add(light_quad_c);
        camera cam2;
        cam2.image_width = 8;
        cam2.aspect_ratio = 1.0;
        cam2.samples_per_pixel = 1;
        cam2.max_depth = 2;
        cam2.background = color(0,0,0);
        cam2.focus_dist = 10;
        std::srand(42);
        std::ostringstream image2;
        old = std::cout.rdbuf(image2.rdbuf());
        cam2.render(world2, lights2);
        std::cout.rdbuf(old);
        UNIT_CHECK(rec_c->calls > 0 && rec_c->valid_count == 0);

        // Persist the differentiated render as a PPM.
        std::ofstream out(".cache/differentials.ppm");
        out << image.str();
        UNIT_CHECK(out.good());
        out.close();

        // Sanity-check the PPM header of the artifact.
        std::istringstream verify(image.str());
        std::string magic;
        int w, h, maxv;
        verify >> magic >> w >> h >> maxv;
        UNIT_CHECK(magic == "P3" && w == 24 && h == 24 && maxv == 255);
    }
}

} // namespace


void register_contract() {
    unit_add("camera",     &test_camera);
    unit_add("geometry",   &test_geometry);
    unit_add("specular",   &test_specular);
    unit_add("texture",    &test_texture);
    unit_add("pipeline",   &test_pipeline);
}


#endif
