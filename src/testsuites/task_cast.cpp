/***
 * Task test suite for the continuous translation query ccdGJKCast().
 *
 * All shapes carry their own support mappings (no triangle tessellation),
 * spheres and axis aligned boxes keep analytic time-of-impact references
 * and an asymmetric convex hull is used to verify witness support planes
 * and the per-object displacement relation.
 */

#include <ccd/ccd.h>
#include "registry.hpp"

#include <cmath>
#include <cstdarg>
#include <limits>
#include <vector>

namespace {

typedef ccd_real_t real;

ccd_vec3_t V(real x, real y, real z)
{
    ccd_vec3_t v;
    ccdVec3Set(&v, x, y, z);
    return v;
}

real dot(const ccd_vec3_t &a, const ccd_vec3_t &b)
{
    return ccdVec3Dot(&a, &b);
}

real len(const ccd_vec3_t &v)
{
    return CCD_SQRT(ccdVec3Len2(&v));
}

ccd_vec3_t sub(const ccd_vec3_t &a, const ccd_vec3_t &b)
{
    ccd_vec3_t r;
    ccdVec3Sub2(&r, &a, &b);
    return r;
}

ccd_vec3_t add(const ccd_vec3_t &a, const ccd_vec3_t &b)
{
    ccd_vec3_t r;
    ccdVec3Copy(&r, &a);
    ccdVec3Add(&r, &b);
    return r;
}

ccd_vec3_t scale(const ccd_vec3_t &a, real k)
{
    ccd_vec3_t r;
    ccdVec3Copy(&r, &a);
    ccdVec3Scale(&r, k);
    return r;
}

bool near(real a, real b, real eps)
{
    return CCD_FABS(a - b) <= eps;
}

bool vecNear(const ccd_vec3_t &a, const ccd_vec3_t &b, real eps)
{
    return near(ccdVec3X(&a), ccdVec3X(&b), eps)
        && near(ccdVec3Y(&a), ccdVec3Y(&b), eps)
        && near(ccdVec3Z(&a), ccdVec3Z(&b), eps);
}

bool vecZero(const ccd_vec3_t &a)
{
    return ccdVec3Len2(&a) == CCD_ZERO;
}

/* ----------------------------- test shapes ----------------------------- */

struct Sphere {
    ccd_vec3_t c;
    real r;
};

struct Box {
    ccd_vec3_t c;
    ccd_vec3_t h; // half extents
};

struct Hull {
    ccd_vec3_t offset;
    std::vector<ccd_vec3_t> v; // local vertices, duplicates are allowed
};

struct Capsule {
    ccd_vec3_t a;
    ccd_vec3_t b;
    real r;
};

void sphereSupport(const void *ptr, const ccd_vec3_t *d, ccd_vec3_t *out)
{
    const Sphere &s = *static_cast<const Sphere *>(ptr);
    real l2 = ccdVec3Len2(d);
    ccdVec3Copy(out, &s.c);
    if (l2 > CCD_ZERO){
        real k = s.r / CCD_SQRT(l2);
        out->v[0] += k * d->v[0];
        out->v[1] += k * d->v[1];
        out->v[2] += k * d->v[2];
    }
}

void sphereCenter(const void *ptr, ccd_vec3_t *out)
{
    ccdVec3Copy(out, &static_cast<const Sphere *>(ptr)->c);
}

void boxSupport(const void *ptr, const ccd_vec3_t *d, ccd_vec3_t *out)
{
    const Box &b = *static_cast<const Box *>(ptr);
    ccdVec3Set(out,
        ccdVec3X(&b.c) + (ccdVec3X(d) >= CCD_ZERO ? ccdVec3X(&b.h)
                                                  : -ccdVec3X(&b.h)),
        ccdVec3Y(&b.c) + (ccdVec3Y(d) >= CCD_ZERO ? ccdVec3Y(&b.h)
                                                  : -ccdVec3Y(&b.h)),
        ccdVec3Z(&b.c) + (ccdVec3Z(d) >= CCD_ZERO ? ccdVec3Z(&b.h)
                                                  : -ccdVec3Z(&b.h)));
}

void boxCenter(const void *ptr, ccd_vec3_t *out)
{
    ccdVec3Copy(out, &static_cast<const Box *>(ptr)->c);
}

void hullSupport(const void *ptr, const ccd_vec3_t *d, ccd_vec3_t *out)
{
    const Hull &h = *static_cast<const Hull *>(ptr);
    real best = 0;
    ccd_vec3_t bestV = h.v[0];
    for (size_t i = 0; i < h.v.size(); ++i){
        real val = dot(h.v[i], *d);
        if (i == 0 || val > best){
            best = val;
            bestV = h.v[i];
        }
    }
    ccdVec3Copy(out, &bestV);
    ccdVec3Add(out, &h.offset);
}

void hullCenter(const void *ptr, ccd_vec3_t *out)
{
    const Hull &h = *static_cast<const Hull *>(ptr);
    ccd_vec3_t acc = V(0, 0, 0);
    for (size_t i = 0; i < h.v.size(); ++i)
        acc = add(acc, h.v[i]);
    acc = scale(acc, CCD_ONE / (real)h.v.size());
    ccdVec3Copy(out, &acc);
    ccdVec3Add(out, &h.offset);
}

void capsuleSupport(const void *ptr, const ccd_vec3_t *d, ccd_vec3_t *out)
{
    const Capsule &cap = *static_cast<const Capsule *>(ptr);
    ccd_vec3_t tip = dot(cap.a, *d) >= dot(cap.b, *d) ? cap.a : cap.b;
    real l2 = ccdVec3Len2(d);
    ccdVec3Copy(out, &tip);
    if (l2 > CCD_ZERO){
        real k = cap.r / CCD_SQRT(l2);
        out->v[0] += k * d->v[0];
        out->v[1] += k * d->v[1];
        out->v[2] += k * d->v[2];
    }
}

void capsuleCenter(const void *ptr, ccd_vec3_t *out)
{
    const Capsule &cap = *static_cast<const Capsule *>(ptr);
    ccdVec3Set(out,
        CCD_REAL(0.5) * (cap.a.v[0] + cap.b.v[0]),
        CCD_REAL(0.5) * (cap.a.v[1] + cap.b.v[1]),
        CCD_REAL(0.5) * (cap.a.v[2] + cap.b.v[2]));
}

ccd_t makeCcd(ccd_support_fn s1, ccd_center_fn c1,
              ccd_support_fn s2, ccd_center_fn c2,
              real tol = CCD_REAL(1e-6),
              unsigned long max_it = (unsigned long)-1)
{
    ccd_t ccd;
    CCD_INIT(&ccd);
    ccd.support1 = s1;
    ccd.support2 = s2;
    ccd.center1 = c1;
    ccd.center2 = c2;
    ccd.dist_tolerance = tol;
    ccd.max_iterations = max_it;
    return ccd;
}

int cast(const ccd_t &ccd, const void *o1, const void *o2,
         const ccd_vec3_t &d1, const ccd_vec3_t &d2,
         ccd_cast_result_t &res)
{
    return ccdGJKCast(o1, o2, &ccd, &d1, &d2, &res);
}

void expectZeroed(Registry &r, const ccd_cast_result_t &res, const char *m)
{
    r.check(res.toi == CCD_ZERO, m);
    r.check(vecZero(res.normal), m);
    r.check(vecZero(res.point1), m);
    r.check(vecZero(res.point2), m);
}

/**
 * Analytic first time of impact of two spheres on linear paths.
 * Returns a value > 1 when they never touch on [0, 1].
 */
real sphereSphereToi(const Sphere &s1, const ccd_vec3_t &d1,
                     const Sphere &s2, const ccd_vec3_t &d2)
{
    ccd_vec3_t p = sub(s1.c, s2.c);
    ccd_vec3_t v = sub(d1, d2);
    real rr = s1.r + s2.r;
    real A = dot(v, v);
    real B = CCD_REAL(2.) * dot(p, v);
    real C = dot(p, p) - rr * rr;
    if (A == CCD_ZERO)
        return C <= CCD_ZERO ? CCD_ZERO : CCD_REAL(2.);
    real disc = B * B - CCD_REAL(4.) * A * C;
    if (disc < CCD_ZERO)
        return CCD_REAL(2.);
    real t = (-B - CCD_SQRT(disc)) / (CCD_REAL(2.) * A);
    if (t < CCD_ZERO)
        t = (-B + CCD_SQRT(disc)) / (CCD_REAL(2.) * A);
    return t;
}

/** Support value of an untransformed hull in direction d. */
real hullSupportValue(const Hull &h, const ccd_vec3_t &d)
{
    real best = 0;
    for (size_t i = 0; i < h.v.size(); ++i){
        real val = dot(add(h.v[i], h.offset), d);
        if (i == 0 || val > best)
            best = val;
    }
    return best;
}

} // namespace

void register_task_tests(Registry &r)
{
    const real TOL = CCD_REAL(1e-6);

    /* ------------------------------ sweep ------------------------------ */

    r.add("sweep", "thin_slab_passthrough", [&]{
        // A very thin slab is crossed by a fast thin box; the contact window
        // in time is only ~0.025 wide and must not be missed.
        Box slab{V(0, 0, 0), V(0.001, 1, 1)};
        Box bullet{V(3, 0, 0), V(0.05, 0.5, 0.5)};
        ccd_t ccd = makeCcd(boxSupport, boxCenter, boxSupport, boxCenter);
        ccd_cast_result_t res;
        int rc = cast(ccd, &slab, &bullet, V(0, 0, 0), V(-4, 0, 0), res);
        real analytic = (3 - 0.05 - 0.001) / 4.0;
        r.check(rc == 0, "thin slab contact must be found");
        r.check(near(res.toi, analytic, 1e-3), "thin slab analytic toi");
        r.check(ccdVec3X(&res.normal) > 0.99, "normal points obj1 -> obj2");
        r.check(len(res.normal) > 0.9999 && len(res.normal) < 1.0001,
                "normal is unit length");
        r.check(len(sub(res.point2, res.point1)) <= CCD_REAL(8e-6),
                "witness points are close");
    });

    r.add("sweep", "ultra_short_contact_window", [&]{
        // Fast bullet fully crosses an ultra-thin slab: it is separated at
        // BOTH endpoints and the true contact window is only ~0.003 wide.
        // Deterministic minimization must still find it.
        Box slab{V(0, 0, 0), V(0.0005, 1, 1)};
        Box bullet{V(8, 0, 0), V(0.01, 0.4, 0.4)};
        ccd_t ccd = makeCcd(boxSupport, boxCenter, boxSupport, boxCenter,
                            CCD_REAL(1e-8));
        ccd_cast_result_t res;
        int rc = cast(ccd, &slab, &bullet, V(0, 0, 0),
                      V(-16, 0, 0), res);
        // first touch when bullet leading face (x - 0.01) reaches +0.0005
        real analytic = (8 - 0.01 - 0.0005) / 16.0;
        r.check(rc == 0, "ultra-short window contact found");
        r.check(near(res.toi, analytic, 1e-4), "ultra-short window toi");
        r.check(len(sub(res.point2, res.point1)) <= CCD_REAL(8e-8),
                "ultra-short window witnesses close");
    });

    r.add("sweep", "clean_miss_zeroed", [&]{
        Box slab{V(0, 0, 0), V(0.1, 1, 1)};
        Sphere s{V(3, 2, 0), 0.5};
        ccd_t ccd = makeCcd(boxSupport, boxCenter,
                            sphereSupport, sphereCenter);
        ccd_cast_result_t res;
        int rc = cast(ccd, &slab, &s, V(0, 0, 0), V(-4, 0, 0), res);
        r.check(rc == 1, "trajectory misses the slab");
        expectZeroed(r, res, "no-contact result must be zeroed");
    });

    r.add("sweep", "grazing_trajectory", [&]{
        // Sphere center stays exactly one radius above the slab top: the
        // distance touches zero over a short plateau and grows again.
        Box slab{V(0, 0, 0), V(0.1, 1, 1)};
        Sphere s{V(3, 1.5, 0), 0.5};
        ccd_t ccd = makeCcd(boxSupport, boxCenter,
                            sphereSupport, sphereCenter);
        ccd_cast_result_t res;
        int rc = cast(ccd, &slab, &s, V(0, 0, 0), V(-4, 0, 0), res);
        r.check(rc == 0, "grazing contact must be found");
        // first touch: sphere center x reaches the slab x extent
        r.check(near(res.toi, (3 - 0.1) / 4.0, 2e-3), "grazing toi");
        r.check(len(sub(res.point2, res.point1)) <= CCD_REAL(8e-6),
                "grazing witnesses coincide");
    });

    r.add("sweep", "touch_exactly_at_endpoint", [&]{
        Sphere a{V(0, 0, 0), 0.5};
        Sphere b{V(3, 0, 0), 0.5};
        ccd_t ccd = makeCcd(sphereSupport, sphereCenter,
                            sphereSupport, sphereCenter);
        ccd_cast_result_t res;
        int rc = cast(ccd, &a, &b, V(0, 0, 0), V(-2, 0, 0), res);
        r.check(rc == 0, "endpoint contact must be reported");
        r.check(res.toi > CCD_ONE - CCD_REAL(1e-4)
                && res.toi <= CCD_ONE, "toi is the interval endpoint");
    });

    r.add("sweep", "both_objects_move", [&]{
        Sphere a{V(-2, 0, 0), 0.5};
        Sphere b{V(2, 0, 0), 0.5};
        ccd_t ccd = makeCcd(sphereSupport, sphereCenter,
                            sphereSupport, sphereCenter);
        ccd_cast_result_t res;
        int rc = cast(ccd, &a, &b, V(2, 0, 0), V(-2, 0, 0), res);
        r.check(rc == 0, "both moving contact");
        r.check(near(res.toi, 0.75, 1e-4), "both moving analytic toi");
        // at the touching instant both witnesses coincide at x = 0
        r.check(vecNear(res.point1, V(0, 0, 0), 1e-4),
                "point1 is the contact point on obj1");
        r.check(vecNear(res.point2, V(0, 0, 0), 1e-4),
                "point2 is the contact point on obj2");
        // witnesses must include each object's own displacement: subtract
        // the travel and recover the original support point of each sphere
        r.check(vecNear(sub(res.point1, scale(V(2, 0, 0), res.toi)),
                        V(-1.5, 0, 0), 1e-4),
                "point1 carries obj1 displacement");
        r.check(vecNear(sub(res.point2, scale(V(-2, 0, 0), res.toi)),
                        V(1.5, 0, 0), 1e-4),
                "point2 carries obj2 displacement");
    });

    r.add("sweep", "common_translation_no_relative_motion", [&]{
        Box a{V(0, 0, 0), V(0.5, 0.5, 0.5)};
        Box b{V(3, 0, 0), V(0.5, 0.5, 0.5)};
        ccd_t ccd = makeCcd(boxSupport, boxCenter, boxSupport, boxCenter);
        ccd_cast_result_t res;
        ccd_vec3_t d = V(5, 1, 0);
        int rc = cast(ccd, &a, &b, d, d, res);
        r.check(rc == 1, "identical deltas cannot create contact");
        expectZeroed(r, res, "common translation result zeroed");
    });

    r.add("sweep", "sphere_analytic_reference", [&]{
        Sphere a{V(0, 0, 0), 1.1};
        Sphere b{V(4, 0.2, -0.1), 0.8};
        ccd_vec3_t d1 = V(0.2, 0.1, 0.0);
        ccd_vec3_t d2 = V(-2.8, 0.15, 0.2);
        ccd_t ccd = makeCcd(sphereSupport, sphereCenter,
                            sphereSupport, sphereCenter);
        ccd_cast_result_t res;
        int rc = cast(ccd, &a, &b, d1, d2, res);
        real analytic = sphereSphereToi(a, d1, b, d2);
        r.check(analytic >= CCD_ZERO && analytic <= CCD_ONE,
                "analytic scene does collide");
        r.check(rc == 0, "sphere sweep contact found");
        r.check(near(res.toi, analytic, 1e-4), "sphere analytic toi");
        r.check(len(sub(res.point2, res.point1)) <= CCD_REAL(8e-6),
                "sphere witnesses close");
    });

    r.add("sweep", "capsule_vs_sphere_sweep", [&]{
        Capsule cap{V(0, -1, 0), V(0, 1, 0), 0.2};
        Sphere s{V(3, 0.5, 0), 0.3};
        ccd_t ccd = makeCcd(capsuleSupport, capsuleCenter,
                            sphereSupport, sphereCenter);
        ccd_cast_result_t res;
        int rc = cast(ccd, &cap, &s, V(0, 0, 0), V(-4, 0, 0), res);
        r.check(rc == 0, "capsule contact found through generic support");
        r.check(near(res.toi, (3 - 0.5) / 4.0, 1e-3), "capsule analytic toi");
        r.check(near(res.point1.v[1], 0.5, 1e-4), "capsule witness on segment");
        r.check(near(res.point1.v[0], 0.2, 1e-4), "capsule witness radius");
    });

    /* ----------------------------- witness ----------------------------- */

    r.add("witness", "hull_support_planes_and_displacements", [&]{
        // Asymmetric five-vertex hull against a moving box.
        Hull h1{V(0, 0, 0), {V(0, 0, 0), V(2, 0, 0), V(0, 1, 0),
                             V(0, 0, 1), V(0.5, 0.4, 0.4)}};
        Box b{V(4, 0, 0), V(0.5, 0.5, 0.5)};
        ccd_vec3_t d1 = V(0, 0, 0);
        ccd_vec3_t d2 = V(-4, 0, 0);
        ccd_t ccd = makeCcd(hullSupport, hullCenter,
                            boxSupport, boxCenter);
        ccd_cast_result_t res;
        int rc = cast(ccd, &h1, &b, d1, d2, res);
        real analytic = 1.5 / 4.0; // gap 3.5 - 2.0 over speed 4
        r.check(rc == 0, "hull contact found");
        r.check(near(res.toi, analytic, 1e-3), "hull analytic toi");
        r.check(near(len(res.normal), 1.0, 1e-9), "unit normal");

        // normal is a valid common support direction at the hit time
        r.check(near(dot(res.normal, res.point1),
                     hullSupportValue(h1, res.normal), 8e-6),
                "point1 on obj1 support plane");
        ccd_vec3_t neg = scale(res.normal, -CCD_ONE);
        {
            ccd_vec3_t sup;
            boxSupport(&b, &neg, &sup); // support of the original box
            r.check(near(dot(neg, sub(res.point2, scale(d2, res.toi))),
                         dot(neg, sup), 8e-6),
                    "point2 on obj2 support plane");
        }

        // witnesses contain each object's own sweep displacement, not just
        // the relative-motion coordinates
        r.check(vecNear(sub(res.point1, scale(d1, res.toi)),
                        res.point1, 1e-12), "obj1 has zero displacement");
        {
            ccd_vec3_t movedBack = sub(res.point2, scale(d2, res.toi));
            ccd_vec3_t sup;
            boxSupport(&b, &neg, &sup);
            r.check(near(dot(neg, movedBack), dot(neg, sup), 8e-6),
                    "point2 minus its own displacement is an original support");
        }
        r.check(len(sub(res.point2, res.point1)) <= CCD_REAL(8e-6),
                "hull witness gap within several tolerances");
    });

    r.add("witness", "hull_both_moving_transform_relation", [&]{
        Hull h1{V(-1, 0, 0), {V(0, 0, 0), V(1, 0, 0), V(0, 1, 0),
                              V(0, 0, 1), V(0.3, 0.3, 0.3)}};
        Hull h2{V(3, 0, 0), {V(0, 0, 0), V(1, 0, 0), V(0, 1, 0),
                              V(0, 0, 1), V(0.3, 0.3, 0.3)}};
        ccd_vec3_t d1 = V(1, 0, 0);
        ccd_vec3_t d2 = V(-3, 0, 0);
        ccd_t ccd = makeCcd(hullSupport, hullCenter,
                            hullSupport, hullCenter);
        ccd_cast_result_t res;
        int rc = cast(ccd, &h1, &h2, d1, d2, res);
        // h1 right extent world x = 0, h2 left face world x = 3 initially,
        // relative speed 4 -> t = 0.75
        r.check(rc == 0, "two moving hulls contact");
        r.check(near(res.toi, 0.75, 1e-3), "two hulls analytic toi");
        // at t = 0.75 the moving faces meet at world x = 0.75
        r.check(near(res.point1.v[0], 0.75, 8e-6), "hull1 witness at right face");
        r.check(near(res.point2.v[0], 0.75, 8e-6), "hull2 witness at left face");
        // each point minus its own travel must lie on the original hull
        ccd_vec3_t local1 = sub(res.point1, scale(d1, res.toi));
        ccd_vec3_t local2 = sub(res.point2, scale(d2, res.toi));
        r.check(near(dot(res.normal, local1),
                     hullSupportValue(h1, res.normal), 8e-6),
                "hull1 local witness on original support plane");
        r.check(near(dot(scale(res.normal, -CCD_ONE), local2),
                     hullSupportValue(h2, scale(res.normal, -CCD_ONE)),
                     8e-6),
                "hull2 local witness on original support plane");
    });

    /* ---------------------------- degenerate --------------------------- */

    r.add("degenerate", "duplicate_hull_vertices", [&]{
        Hull h1{V(0, 0, 0), {V(0, 0, 0), V(2, 0, 0), V(2, 0, 0),
                             V(0, 1, 0), V(0, 0, 1), V(0, 0, 1)}};
        Box b{V(4, 0, 0), V(0.5, 0.5, 0.5)};
        ccd_t ccd = makeCcd(hullSupport, hullCenter,
                            boxSupport, boxCenter);
        ccd_cast_result_t res;
        int rc = cast(ccd, &h1, &b, V(0, 0, 0), V(-4, 0, 0), res);
        r.check(rc == 0, "duplicate support vertices handled");
        r.check(near(res.toi, 1.5 / 4.0, 1e-3), "duplicate vertex toi");
    });

    r.add("degenerate", "zero_displacement_separated_and_overlapping", [&]{
        Sphere a{V(0, 0, 0), 0.5};
        Sphere farB{V(3, 0, 0), 0.5};
        Sphere nearB{V(0.6, 0, 0), 0.5};
        ccd_t ccd = makeCcd(sphereSupport, sphereCenter,
                            sphereSupport, sphereCenter);
        ccd_cast_result_t res;
        r.check(cast(ccd, &a, &farB, V(0, 0, 0), V(0, 0, 0), res) == 1,
                "zero motion, separated pair -> no contact");
        expectZeroed(r, res, "separated zero-motion result zeroed");
        r.check(cast(ccd, &a, &nearB, V(0, 0, 0), V(0, 0, 0), res) == 0,
                "zero motion, overlapping pair -> t = 0 contact");
        r.check(res.toi == CCD_ZERO, "overlap reports toi 0");
        r.check(vecZero(res.normal) && vecZero(res.point1)
                && vecZero(res.point2), "initial overlap fields are zero");
    });

    r.add("degenerate", "initial_touching_contact", [&]{
        Sphere a{V(0, 0, 0), 0.5};
        Sphere b{V(1, 0, 0), 0.5}; // exactly touching
        ccd_t ccd = makeCcd(sphereSupport, sphereCenter,
                            sphereSupport, sphereCenter);
        ccd_cast_result_t res;
        int rc = cast(ccd, &a, &b, V(0, 0, 0), V(1, 0, 0), res);
        r.check(rc == 0, "touching at t=0 is a contact");
        r.check(res.toi == CCD_ZERO && vecZero(res.normal)
                && vecZero(res.point1) && vecZero(res.point2),
                "touching at t=0 uses the zeroed initial form");
    });

    r.add("degenerate", "shallow_near_miss_stays_separated", [&]{
        Box slab{V(0, 0, 0), V(0.1, 1, 1)};
        Sphere s{V(3, 1.51, 0), 0.5}; // minimum gap 0.01
        ccd_t ccd = makeCcd(boxSupport, boxCenter,
                            sphereSupport, sphereCenter);
        ccd_cast_result_t res;
        int rc = cast(ccd, &slab, &s, V(0, 0, 0), V(-4, 0, 0), res);
        r.check(rc == 1, "shallow miss is not promoted to a hit");
        expectZeroed(r, res, "near-miss result zeroed");
    });

    r.add("degenerate", "invalid_arguments", [&]{
        Sphere a{V(0, 0, 0), 0.5};
        Sphere b{V(3, 0, 0), 0.5};
        ccd_t ccd = makeCcd(sphereSupport, sphereCenter,
                            sphereSupport, sphereCenter);
        ccd_cast_result_t res;
        ccd_vec3_t d = V(-2, 0, 0), zero = V(0, 0, 0);

        r.check(ccdGJKCast(NULL, &b, &ccd, &zero, &d, &res) == -1,
                "null obj1 rejected");
        r.check(ccdGJKCast(&a, NULL, &ccd, &zero, &d, &res) == -1,
                "null obj2 rejected");
        r.check(ccdGJKCast(&a, &b, NULL, &zero, &d, &res) == -1,
                "null ccd rejected");
        r.check(ccdGJKCast(&a, &b, &ccd, NULL, &d, &res) == -1,
                "null delta rejected");
        r.check(ccdGJKCast(&a, &b, &ccd, &zero, &d, NULL) == -1,
                "null result rejected");

        ccd_t broken = ccd;
        broken.support1 = NULL;
        r.check(ccdGJKCast(&a, &b, &broken, &zero, &d, &res) == -1,
                "missing support1 rejected");
        broken = ccd;
        broken.support2 = NULL;
        r.check(ccdGJKCast(&a, &b, &broken, &zero, &d, &res) == -1,
                "missing support2 rejected");

        real inf = std::numeric_limits<real>::infinity();
        real nan = std::numeric_limits<real>::quiet_NaN();
        ccd_vec3_t badInf = V(inf, 0, 0);
        ccd_vec3_t badNan = V(0, nan, 0);
        r.check(ccdGJKCast(&a, &b, &ccd, &badInf, &d, &res) == -1,
                "infinite displacement rejected");
        r.check(ccdGJKCast(&a, &b, &ccd, &zero, &badNan, &res) == -1,
                "NaN displacement rejected");

        broken = ccd;
        broken.dist_tolerance = CCD_ZERO;
        r.check(ccdGJKCast(&a, &b, &broken, &zero, &d, &res) == -1,
                "zero tolerance rejected");
        broken.dist_tolerance = -TOL;
        r.check(ccdGJKCast(&a, &b, &broken, &zero, &d, &res) == -1,
                "negative tolerance rejected");
    });

    r.add("degenerate", "iteration_exhaustion_is_not_a_miss", [&]{
        Sphere a{V(0, 0, 0), 0.5};
        Sphere b{V(3, 0, 0), 0.5};
        // a handful of evaluations cannot finish the minimization
        ccd_t ccd = makeCcd(sphereSupport, sphereCenter,
                            sphereSupport, sphereCenter, TOL, 3UL);
        ccd_cast_result_t res;
        int rc = cast(ccd, &a, &b, V(0, 0, 0), V(-2, 0, 0), res);
        r.check(rc == -3, "exhaustion reported as non-convergence");
    });

    /* --------------------------- compatibility -------------------------- */

    r.add("compatibility", "objects_and_config_unchanged", [&]{
        Sphere a{V(0, 0, 0), 0.5};
        Sphere b{V(2, 0.5, 0), 0.7};
        Hull h{V(5, 0, 0), {V(0, 0, 0), V(1, 0, 0), V(0, 1, 0),
                            V(0, 0, 1), V(0.2, 0.2, 0.2)}};
        ccd_t ccdA = makeCcd(sphereSupport, sphereCenter,
                             sphereSupport, sphereCenter);
        ccd_t ccdB = makeCcd(sphereSupport, sphereCenter,
                             hullSupport, hullCenter);
        ccd_t snapA = ccdA, snapB = ccdB;
        Sphere snapSphere = b;
        Hull snapHull = h;

        ccd_cast_result_t res;
        r.check(cast(ccdA, &a, &b, V(0.1, 0, 0), V(-2.5, 0, 0), res) == 0,
                "sphere/sphere sweep runs");
        // the hull stays around x in [5, 6] while the sphere sweeps x in
        // [-0.5, 2], so this pair never meets
        r.check(cast(ccdB, &b, &h, V(-2, 0, 0), V(0, 0, 0), res) == 1,
                "sphere/hull miss runs");

        r.check(a.c.v[0] == 0 && a.r == 0.5, "obj1 fields untouched");
        r.check(b.c.v[0] == snapSphere.c.v[0] && b.c.v[1] == snapSphere.c.v[1]
                && b.r == snapSphere.r, "sphere object untouched");
        r.check(h.offset.v[0] == snapHull.offset.v[0]
                && h.v.size() == snapHull.v.size(), "hull object untouched");
        bool hullSame = true;
        for (size_t i = 0; i < h.v.size(); ++i)
            hullSame = hullSame && vecNear(h.v[i], snapHull.v[i], 0);
        r.check(hullSame, "hull vertices untouched");
        r.check(ccdA.support1 == snapA.support1
                && ccdA.dist_tolerance == snapA.dist_tolerance
                && ccdA.max_iterations == snapA.max_iterations,
                "ccd config untouched");
        r.check(ccdB.support2 == snapB.support2
                && ccdB.dist_tolerance == snapB.dist_tolerance,
                "second ccd config untouched");
    });

    r.add("compatibility", "swap_symmetry", [&]{
        Sphere a{V(-2, 0.3, 0), 0.6};
        Sphere b{V(2, -0.2, 0), 0.4};
        // relative velocity is exactly opposite the initial separation, so
        // the centers stay on one line and the analytic hit is t ~= 0.752
        ccd_vec3_t da = V(2.0, -0.25, 0);
        ccd_vec3_t db = V(-2.0, 0.25, 0);
        ccd_t ccd = makeCcd(sphereSupport, sphereCenter,
                            sphereSupport, sphereCenter);
        ccd_cast_result_t fwd, rev;
        int rc1 = cast(ccd, &a, &b, da, db, fwd);
        int rc2 = cast(ccd, &b, &a, db, da, rev);
        r.check(rc1 == 0 && rc2 == 0, "both orientations report contact");
        r.check(fwd.toi > 0.7 && fwd.toi < 0.8, "contact inside the interval");
        r.check(near(fwd.toi, rev.toi, 1e-5), "swapped toi is identical");
        r.check(near(dot(fwd.normal, rev.normal), -1.0, 1e-5),
                "swapped normal is reversed");
        r.check(near(len(fwd.normal), 1.0, 1e-9), "forward normal is unit");
        r.check(vecNear(fwd.point1, rev.point2, 1e-5),
                "swapped witnesses exchange (1)");
        r.check(vecNear(fwd.point2, rev.point1, 1e-5),
                "swapped witnesses exchange (2)");
    });

    r.add("compatibility", "tolerance_scales_with_scene", [&]{
        const real scales[2] = {100.0, 0.01};
        for (int k = 0; k < 2; ++k){
            real s = scales[k];
            Sphere a{scale(V(0, 0, 0), s), 1.0 * s};
            Sphere b{scale(V(5, 0, 0), s), 1.0 * s};
            ccd_vec3_t d1 = V(0, 0, 0);
            ccd_vec3_t d2 = scale(V(-4, 0, 0), s);
            ccd_t ccd = makeCcd(sphereSupport, sphereCenter,
                                sphereSupport, sphereCenter,
                                TOL * s);
            ccd_cast_result_t res;
            int rc = cast(ccd, &a, &b, d1, d2, res);
            r.check(rc == 0, "scaled scene contact found");
            r.check(near(res.toi, 0.75, 1e-4), "scaled toi unchanged");
            r.check(len(sub(res.point2, res.point1)) <= CCD_REAL(8.) * TOL * s,
                    "witness gap scales with the tolerance");
            r.check(near(len(res.normal), 1.0, 1e-9), "scaled normal unit");
        }
    });

    r.add("compatibility", "static_queries_still_work", [&]{
        Box a{V(0, 0, 0), V(1, 1, 1)};
        Box b{V(3, 0, 0), V(1, 1, 1)};
        ccd_t ccd = makeCcd(boxSupport, boxCenter, boxSupport, boxCenter);
        ccd_cast_result_t res;
        r.check(cast(ccd, &a, &b, V(0, 0, 0), V(-4, 0, 0), res) == 0,
                "cast coexists with static queries");
        r.check(ccdGJKIntersect(&a, &b, &ccd) == 0,
                "static GJK still sees the pair as separated");
        Box overlap{V(1.5, 0, 0), V(1, 1, 1)};
        r.check(ccdGJKIntersect(&a, &overlap, &ccd) != 0,
                "static GJK still detects overlap");
        real depth = 0;
        ccd_vec3_t dir, pos;
        r.check(ccdGJKPenetration(&a, &overlap, &ccd, &depth, &dir, &pos)
                == 0, "static EPA penetration still works");
        r.check(depth > 0.4 && depth < 0.6, "penetration depth unchanged");
    });
}
