#include <cmath>
#include <stdexcept>
#include <fstream>
#include <iomanip>
#include <algorithm>

#include "beamfilm.h"
#include "material.h"

namespace {

const double BEAM_EPS = 1e-12;

bool finite_vec(const Vec &v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Affine function f(x,y) = a + b*x + c*y in screen coordinates.
struct Affine {
    double a, b, c;

    Affine(double a_=0.0, double b_=0.0, double c_=0.0) : a(a_), b(b_), c(c_) {}
    double eval(double x, double y) const { return a + b*x + c*y; }
    double scale() const { return 1.0 + fabs(a) + fabs(b) + fabs(c); }
    bool trivial() const {
        double s = scale();
        return fabs(a) <= BEAM_EPS*s && fabs(b) <= BEAM_EPS*s && fabs(c) <= BEAM_EPS*s;
    }
};

Affine operator-(const Affine &p, const Affine &q) {
    return Affine(p.a-q.a, p.b-q.b, p.c-q.c);
}

// Affine vector-valued function D(x,y) = d0 + dx*x + dy*y: the unnormalized
// ray direction family of one beam segment. Keeping the directions
// unnormalized makes the family affine under planar reflection; a positive
// per-ray scaling never changes the hit ordering or barycentric regions.
struct RayFamily {
    Vec eye;
    Vec d0, dx, dy;

    Vec dir(double x, double y) const {
        return Vec(d0.x + dx.x*x + dy.x*y,
                   d0.y + dx.y*x + dy.y*y,
                   d0.z + dx.z*x + dy.z*y);
    }
};

struct Poly2 {
    std::vector<ScreenPoint> v; // Convex polygon, CCW when area is positive
};

double polygon_area(const Poly2 &p) {
    size_t n = p.v.size();
    if (n < 3) return 0.0;
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const ScreenPoint &p1 = p.v[i];
        const ScreenPoint &p2 = p.v[(i+1) % n];
        s += p1.x * p2.y - p2.x * p1.y;
    }
    return 0.5 * fabs(s);
}

void dedupe(Poly2 &p) {
    Poly2 out;
    size_t n = p.v.size();
    for (size_t i = 0; i < n; ++i) {
        const ScreenPoint &a = p.v[i];
        const ScreenPoint &b = p.v[(i+1) % n];
        if (fabs(a.x-b.x) > BEAM_EPS || fabs(a.y-b.y) > BEAM_EPS) out.v.push_back(a);
    }
    p.v.swap(out.v);
}

// Clip a convex polygon against f(x,y) >= 0 (Sutherland-Hodgman).
Poly2 clip_halfplane(const Poly2 &poly, const Affine &f) {
    Poly2 out;
    size_t n = poly.v.size();
    if (n == 0) return out;
    for (size_t i = 0; i < n; ++i) {
        const ScreenPoint &s = poly.v[i];
        const ScreenPoint &e = poly.v[(i+1) % n];
        double fs = f.eval(s.x, s.y);
        double fe = f.eval(e.x, e.y);
        bool sin = fs >= -BEAM_EPS;
        bool ein = fe >= -BEAM_EPS;
        if (sin) out.v.push_back(s);
        if (sin != ein) {
            double denom = fe - fs;
            double t = denom == 0.0 ? 0.0 : (-fs) / denom;
            if (!std::isfinite(t)) t = 0.0;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            out.v.push_back(ScreenPoint(s.x + (e.x-s.x)*t, s.y + (e.y-s.y)*t));
        }
    }
    dedupe(out);
    return out;
}

// Oriented screen line; the interior side is the left hand side.
struct Line2 {
    ScreenPoint p;
    ScreenPoint d;
    bool valid;

    Line2() : valid(false) {}
    Line2(ScreenPoint p_, ScreenPoint d_) : p(p_), d(d_), valid(true) {
        double len = hypot(d.x, d.y);
        if (len > 0.0) { d.x /= len; d.y /= len; }
    }

    double side(const ScreenPoint &q) const {
        return d.x * (q.y - p.y) - d.y * (q.x - p.x);
    }
    Affine halfplane() const {
        // side >= 0  <=>  (-d.y)*x + d.x*y + (d.y*p.x - d.x*p.y) >= 0
        return Affine(d.y*p.x - d.x*p.y, -d.y, d.x);
    }
};

bool point_in_polygon(const Poly2 &poly, double x, double y) {
    size_t n = poly.v.size();
    for (size_t i = 0; i < n; ++i) {
        const ScreenPoint &a = poly.v[i];
        const ScreenPoint &b = poly.v[(i+1) % n];
        double cross = (b.x-a.x)*(y-a.y) - (b.y-a.y)*(x-a.x);
        if (cross < -1e-9) return false;
    }
    return true;
}

ScreenPoint polygon_centroid(const Poly2 &p) {
    double sx = 0.0, sy = 0.0;
    for (size_t i = 0; i < p.v.size(); ++i) {
        sx += p.v[i].x;
        sy += p.v[i].y;
    }
    double inv = 1.0 / (double)p.v.size();
    return ScreenPoint(sx*inv, sy*inv);
}

// Moller-Trumbore scalars for one concrete ray of the family.
struct MollerScalars {
    double det, U, V, T; // U = u*det, V = v*det, T = t*det
};

MollerScalars moller(const RayFamily &fam, const Triangle &tri, double x, double y) {
    Vec d = fam.dir(x, y);
    Vec pvec = d.cross(tri.e2);
    double det = tri.e1.dot(pvec);
    Vec tvec = fam.eye - tri.v0;
    double U = tvec.dot(pvec);
    Vec qvec = tvec.cross(tri.e1);
    double V = d.dot(qvec);
    double T = tri.e2.dot(qvec);
    MollerScalars ms;
    ms.det = det; ms.U = U; ms.V = V; ms.T = T;
    return ms;
}

Affine fit_affine(double f00, double f10, double f01) {
    return Affine(f00, f10-f00, f01-f00);
}

// Screen footprint of one triangle as seen by the beam, intersected with the
// current domain. The barycentric/depth conditions are affine half planes
// once the sign of det is fixed; both signs are tried because the det=0 line
// itself has zero area.
struct Footprint {
    Poly2 poly;
    std::vector<Line2> boundary; // Oriented footprint edges, interior left
    Triangle *tri;
    Affine K, L;                 // depth t(x,y) = K.eval / L.eval
};

Poly2 triangle_footprint_poly(const RayFamily &fam, const Triangle &tri, const Poly2 &domain) {
    MollerScalars m00 = moller(fam, tri, 0.0, 0.0);
    MollerScalars m10 = moller(fam, tri, 1.0, 0.0);
    MollerScalars m01 = moller(fam, tri, 0.0, 1.0);

    Affine det = fit_affine(m00.det, m10.det, m01.det);
    Affine U   = fit_affine(m00.U,   m10.U,   m01.U);
    Affine V   = fit_affine(m00.V,   m10.V,   m01.V);
    Affine T   = fit_affine(m00.T,   m10.T,   m01.T);
    Affine W   = det - U - V; // (1-u-v)*det

    Poly2 best;
    for (int sign = 0; sign < 2; ++sign) {
        double s = sign == 0 ? 1.0 : -1.0;
        Poly2 p = domain;
        p = clip_halfplane(p, Affine(s*U.a, s*U.b, s*U.c));
        if (p.v.empty()) continue;
        p = clip_halfplane(p, Affine(s*V.a, s*V.b, s*V.c));
        if (p.v.empty()) continue;
        p = clip_halfplane(p, Affine(s*W.a, s*W.b, s*W.c));
        if (p.v.empty()) continue;
        p = clip_halfplane(p, Affine(s*T.a, s*T.b, s*T.c));
        if (p.v.empty()) continue;
        if (polygon_area(p) > polygon_area(best)) best = p;
    }
    return best;
}

// Clip a world-space triangle against n.P >= d, returning a convex polygon
// of world points. Used for the mirror departure plane so triangles
// straddling the virtual eye keep only their valid portion. On-plane
// vertices are kept (they bound a valid region); a polygon lying wholly on
// the plane, such as the mirror itself, is rejected by the strict-side test
// applied afterwards.
std::vector<Vec> clip_triangle_world(const Triangle &tri, const Vec &n, double d) {
    std::vector<Vec> in;
    in.push_back(tri.v0);
    in.push_back(tri.v1);
    in.push_back(tri.v2);
    std::vector<Vec> out;
    size_t cnt = in.size();
    double scale = 1.0 + fabs(d);
    double tol = BEAM_EPS * scale;
    for (size_t i = 0; i < cnt; ++i) {
        const Vec &s = in[i];
        const Vec &e = in[(i+1) % cnt];
        double fs = n.dot(s) - d;
        double fe = n.dot(e) - d;
        bool sin = fs >= -tol;
        bool ein = fe >= -tol;
        if (sin) out.push_back(s);
        if (sin != ein) {
            double t = (-fs) / (fe - fs);
            if (!std::isfinite(t)) t = 0.0;
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            out.push_back(s + (e - s) * t);
        }
    }
    bool extends = false;
    for (size_t i = 0; i < out.size(); ++i) {
        if (n.dot(out[i]) - d > tol) { extends = true; break; }
    }
    if (!extends) out.clear(); // coplanar with the departure plane
    return out;
}

// Build the conservative frustum of a convex screen domain for the given
// beam. Every polygon edge sweeps a plane through the (possibly virtual)
// eye; degenerate/near parallel planes are skipped rather than made up.
BeamFrustum build_frustum(const RayFamily &fam, const Poly2 &domain,
                          bool has_near, const Vec &near_n, double near_d) {
    BeamFrustum fr;
    size_t n = domain.v.size();
    ScreenPoint c = polygon_centroid(domain);
    Vec dc = fam.dir(c.x, c.y);
    for (size_t i = 0; i < n; ++i) {
        const ScreenPoint &p1 = domain.v[i];
        const ScreenPoint &p2 = domain.v[(i+1) % n];
        Vec d1 = fam.dir(p1.x, p1.y);
        Vec d2 = fam.dir(p2.x, p2.y);
        Vec nrm = d1.cross(d2);
        double len = nrm.mag();
        double scale = d1.mag() * d2.mag();
        if (len <= BEAM_EPS * scale || scale == 0.0) {
            fr.sides.push_back(Plane()); // invalid: never cull on this plane
            continue;
        }
        nrm = nrm * (1.0 / len);
        if (nrm.dot(dc) < 0.0) nrm = nrm * -1.0;
        fr.sides.push_back(Plane(nrm, nrm.dot(fam.eye), true));
    }
    if (has_near) {
        Vec nrm = near_n;
        double len = nrm.mag();
        if (len > 0.0) {
            nrm = nrm * (1.0 / len);
            fr.near = Plane(nrm, near_d, true);
            fr.has_near = true;
        }
    }
    return fr;
}

struct Winner {
    long slot;
    long face;

    bool operator<(const Winner &o) const {
        return slot != o.slot ? slot < o.slot : face < o.face;
    }
};

double depth_at(const Footprint &f, double x, double y) {
    double L = f.L.eval(x, y);
    if (fabs(L) <= BEAM_EPS) return INFINITY;
    return f.K.eval(x, y) / L;
}

// Depth equality line f_a == f_b, i.e. K_a*L_b - K_b*L_a = 0. The product
// of affine functions is quadratic in general, but its zero set is still a
// straight line when the two triangle planes are distinct, so the line is
// reconstructed geometrically from two zero points instead of being
// approximated by an affine clip.
Line2 depth_contour(const Footprint &a, const Footprint &b) {
    Affine q(a.K.a*b.L.a - b.K.a*a.L.a,
             a.K.a*b.L.b - b.K.a*a.L.b,
             a.K.a*b.L.c - b.K.a*a.L.c);
    // Full quadratic coefficients of K_a*L_b - K_b*L_a (for the trivial
    // test the affine part is enough together with the probe below).
    if (q.trivial()) return Line2();

    // Bounding box of the two footprints; intersect q=0 with its edges.
    double xmin = INFINITY, xmax = -INFINITY, ymin = INFINITY, ymax = -INFINITY;
    for (int pass = 0; pass < 2; ++pass) {
        const Poly2 &pp = pass == 0 ? a.poly : b.poly;
        for (size_t i = 0; i < pp.v.size(); ++i) {
            xmin = std::min(xmin, pp.v[i].x);
            xmax = std::max(xmax, pp.v[i].x);
            ymin = std::min(ymin, pp.v[i].y);
            ymax = std::max(ymax, pp.v[i].y);
        }
    }
    if (!std::isfinite(xmin)) return Line2();
    // Pad the box so a contour touching a corner is still recovered.
    double pad = 1e-9 * (1.0 + fabs(xmax-xmin) + fabs(ymax-ymin));
    xmin -= pad; xmax += pad; ymin -= pad; ymax += pad;

    ScreenPoint edges[4][2] = {
        { ScreenPoint(xmin, ymin), ScreenPoint(xmax, ymin) },
        { ScreenPoint(xmax, ymin), ScreenPoint(xmax, ymax) },
        { ScreenPoint(xmax, ymax), ScreenPoint(xmin, ymax) },
        { ScreenPoint(xmin, ymax), ScreenPoint(xmin, ymin) }
    };
    std::vector<ScreenPoint> zeros;
    for (int e = 0; e < 4; ++e) {
        ScreenPoint s = edges[e][0];
        ScreenPoint en = edges[e][1];
        // q is quadratic; evaluate via the two depth fractions instead.
        double ts = depth_at(a, s.x, s.y) - depth_at(b, s.x, s.y);
        double te = depth_at(a, en.x, en.y) - depth_at(b, en.x, en.y);
        if (!std::isfinite(ts) || !std::isfinite(te)) continue;
        if (fabs(ts) <= BEAM_EPS) zeros.push_back(s);
        if (ts * te < 0.0) {
            double t = ts / (ts - te);
            zeros.push_back(ScreenPoint(s.x + (en.x-s.x)*t, s.y + (en.y-s.y)*t));
        }
    }
    if (zeros.size() < 2) return Line2();
    for (size_t i = 1; i < zeros.size(); ++i) {
        double dx = zeros[i].x - zeros[0].x;
        double dy = zeros[i].y - zeros[0].y;
        if (hypot(dx, dy) > BEAM_EPS) {
            return Line2(zeros[0], ScreenPoint(dx, dy));
        }
    }
    return Line2();
}

class BeamEngine {
public:
    Scene &scene;
    Camera &camera;
    int depth_limit;
    BeamTraversalStats stats;

    BeamEngine(Scene &s, Camera &c, int D) : scene(s), camera(c), depth_limit(D) {}

    // Trace one convex screen domain. weight is the accumulated mirror
    // reflectance; results land in the supplied accumulators.
    void trace(const RayFamily &fam, const Poly2 &domain, int bounce,
               const Vec &weight, bool has_near, const Vec &near_n, double near_d,
               std::vector<Vec> &by_bounce, std::vector<double> &emitter_by_bounce,
               double &miss_area, double &trunc_area) {
        if (polygon_area(domain) <= 0.0) return;

        BeamFrustum frustum = build_frustum(fam, domain, has_near, near_n, near_d);

        // Conservative KD candidate gathering across every scene mesh.
        std::vector<Triangle*> candidates;
        size_t objs = scene.object_count();
        for (size_t oi = 0; oi < objs; ++oi) {
            Mesh *mesh = dynamic_cast<Mesh*>(scene.get_object(oi));
            if (mesh == NULL) continue;
            KDNode *root = mesh->get_kd_root();
            if (root == NULL) continue;
            root->collect_beam(root, frustum, candidates, stats);
        }
        // KD leaves own disjoint triangle sets, but keep a guard anyway.
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        // Exact per-face clipping into screen footprints.
        std::vector<Footprint> prints;
        for (size_t ci = 0; ci < candidates.size(); ++ci) {
            Triangle *tri = candidates[ci];
            ++stats.clipped_faces;
            std::vector<Triangle> pieces;
            if (has_near) {
                std::vector<Vec> wp = clip_triangle_world(*tri, near_n, near_d);
                if (wp.size() < 3) continue;
                for (size_t k = 1; k + 1 < wp.size(); ++k) {
                    pieces.push_back(Triangle(wp[0], wp[k], wp[k+1],
                                              tri->t0, tri->t1, tri->t2, tri->m,
                                              tri->slot, tri->face));
                }
            } else {
                pieces.push_back(*tri);
            }
            for (size_t k = 0; k < pieces.size(); ++k) {
                Poly2 fp = triangle_footprint_poly(fam, pieces[k], domain);
                if (fp.v.size() < 3 || polygon_area(fp) <= 0.0) continue;
                Footprint f;
                f.poly = fp;
                f.tri = tri;
                Vec nrm = tri->n;
                f.K = Affine(nrm.dot(tri->v0 - fam.eye), 0.0, 0.0);
                f.L = Affine(nrm.dot(fam.d0), nrm.dot(fam.dx), nrm.dot(fam.dy));
                // Oriented polygon edges: interior of the footprint is left.
                size_t m = fp.v.size();
                for (size_t e = 0; e < m; ++e) {
                    ScreenPoint a = fp.v[e];
                    ScreenPoint b = fp.v[(e+1) % m];
                    Line2 line(a, ScreenPoint(b.x-a.x, b.y-a.y));
                    ScreenPoint probe = fp.v[(e+2) % m];
                    if (line.side(probe) < 0.0) {
                        line = Line2(b, ScreenPoint(a.x-b.x, a.y-b.y));
                    }
                    if (line.valid) f.boundary.push_back(line);
                }
                prints.push_back(f);
            }
        }

        if (prints.empty()) {
            miss_area += polygon_area(domain);
            return;
        }

        // Arrangement: split the domain by every footprint boundary and
        // every non-trivial depth equality contour, so each final cell has a
        // single constant winner. All cells stay convex.
        std::vector<Line2> splitters;
        for (size_t i = 0; i < prints.size(); ++i) {
            for (size_t e = 0; e < prints[i].boundary.size(); ++e) {
                splitters.push_back(prints[i].boundary[e]);
            }
        }
        for (size_t i = 0; i < prints.size(); ++i) {
            for (size_t j = i+1; j < prints.size(); ++j) {
                Line2 contour = depth_contour(prints[i], prints[j]);
                if (contour.valid) splitters.push_back(contour);
            }
        }

        std::vector<Poly2> cells;
        cells.push_back(domain);
        for (size_t s = 0; s < splitters.size(); ++s) {
            Affine pos = splitters[s].halfplane();
            Affine neg(-pos.a, -pos.b, -pos.c);
            std::vector<Poly2> next;
            for (size_t i = 0; i < cells.size(); ++i) {
                Poly2 a = clip_halfplane(cells[i], pos);
                Poly2 b = clip_halfplane(cells[i], neg);
                if (a.v.size() >= 3 && polygon_area(a) > 0.0) next.push_back(a);
                if (b.v.size() >= 3 && polygon_area(b) > 0.0) next.push_back(b);
            }
            cells.swap(next);
            if (cells.empty()) break;
        }

        for (size_t ci = 0; ci < cells.size(); ++ci) {
            const Poly2 &cell = cells[ci];
            double area = polygon_area(cell);
            if (area <= 0.0) continue;
            ScreenPoint pc = polygon_centroid(cell);

            int best = -1;
            double best_t = INFINITY;
            for (size_t i = 0; i < prints.size(); ++i) {
                if (!point_in_polygon(prints[i].poly, pc.x, pc.y)) continue;
                double t = depth_at(prints[i], pc.x, pc.y);
                if (!std::isfinite(t) || t <= BEAM_EPS) continue;
                Winner wi;
                wi.slot = prints[i].tri->slot;
                wi.face = prints[i].tri->face;
                bool take = false;
                if (best < 0) {
                    take = true;
                } else if (t < best_t - BEAM_EPS) {
                    take = true;
                } else if (fabs(t - best_t) <= BEAM_EPS) {
                    Winner wb;
                    wb.slot = prints[best].tri->slot;
                    wb.face = prints[best].tri->face;
                    if (wi < wb) take = true;
                }
                if (take) { best = (int)i; best_t = t; }
            }

            if (best < 0) {
                miss_area += area;
                continue;
            }

            Triangle *tri = prints[best].tri;
            MaterialType type = tri->m->get_type();
            if (type == EMIT) {
                Vec e = tri->m->get_emission();
                by_bounce[bounce].x += area * weight.x * e.x;
                by_bounce[bounce].y += area * weight.y * e.y;
                by_bounce[bounce].z += area * weight.z * e.z;
                emitter_by_bounce[bounce] += area;
            } else if (type == IDEAL_MIRROR) {
                if (bounce >= depth_limit) {
                    trunc_area += area;
                    continue;
                }
                reflect_segment(fam, cell, tri, bounce, weight, by_bounce,
                                emitter_by_bounce, miss_area, trunc_area);
            } else {
                // Validation guarantees this cannot happen.
                throw std::invalid_argument("beam film: unsupported material reached during transport");
            }
        }
    }

private:
    void reflect_segment(const RayFamily &fam, const Poly2 &cell, Triangle *mirror,
                         int bounce, const Vec &weight,
                         std::vector<Vec> &by_bounce,
                         std::vector<double> &emitter_by_bounce,
                         double &miss_area, double &trunc_area) {
        Vec nm = mirror->n; // unit geometric normal; reflection is sign invariant
        Vec q0 = mirror->v0;
        double nd = nm.dot(q0);

        // Virtual eye: mirror image of the beam eye across the mirror plane.
        double dist = nm.dot(fam.eye - q0);
        RayFamily nf;
        nf.eye = fam.eye - nm * (2.0 * dist);
        nf.d0 = reflect_dir(fam.d0, nm);
        nf.dx = reflect_dir(fam.dx, nm);
        nf.dy = reflect_dir(fam.dy, nm);

        // Near plane: accept the half space the reflected rays travel in,
        // i.e. the side opposite the virtual eye. Flipping the normal must
        // flip the plane constant as well.
        double eye_side = nm.dot(nf.eye - q0);
        Vec near_n;
        double near_d;
        if (eye_side >= 0.0) {
            near_n = nm * -1.0;
            near_d = -nd;
        } else {
            near_n = nm;
            near_d = nd;
        }

        Vec rho = mirror->m->get_colour();
        Vec nweight(weight.x*rho.x, weight.y*rho.y, weight.z*rho.z);

        trace(nf, cell, bounce + 1, nweight, true, near_n, near_d,
              by_bounce, emitter_by_bounce, miss_area, trunc_area);
    }

    static Vec reflect_dir(const Vec &d, const Vec &n) {
        return d - n * (2.0 * n.dot(d));
    }
};

// Unnormalized affine ray family of the camera, matching get_ray at integers.
RayFamily camera_family(const Camera &cam) {
    Vec d = cam.get_direction();
    Vec xd = cam.get_x_direction();
    Vec yd = cam.get_y_direction();
    double r = cam.get_ratio();
    double W = (double)cam.get_width();
    double H = (double)cam.get_height();

    RayFamily fam;
    fam.eye = cam.get_position();
    fam.d0 = d*2.0 - xd*r + yd;
    fam.dx = xd * (2.0*r/W);
    fam.dy = yd * (-2.0/H);
    return fam;
}

void validate_camera(const Camera &cam) {
    if (cam.get_width() <= 0 || cam.get_height() <= 0) {
        throw std::invalid_argument("beam film: image dimensions must be positive");
    }
    Vec d = cam.get_direction();
    Vec xd = cam.get_x_direction();
    Vec yd = cam.get_y_direction();
    Vec eye = cam.get_position();
    if (!finite_vec(d) || !finite_vec(xd) || !finite_vec(yd) || !finite_vec(eye)) {
        throw std::invalid_argument("beam film: camera basis contains non-finite data");
    }
    if (d.mag() <= BEAM_EPS || xd.mag() <= BEAM_EPS || yd.mag() <= BEAM_EPS) {
        throw std::invalid_argument("beam film: degenerate camera basis");
    }
}

void validate_depth(int depth) {
    if (depth < 0 || depth > 4) {
        throw std::invalid_argument("beam film: reflection depth limit must be in [0,4]");
    }
}

Poly2 make_polygon(const std::vector<ScreenPoint> &points) {
    Poly2 p;
    if (points.size() < 3) {
        throw std::invalid_argument("beam film: sub-domain needs at least three points");
    }
    for (size_t i = 0; i < points.size(); ++i) {
        if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y)) {
            throw std::invalid_argument("beam film: sub-domain contains non-finite coordinates");
        }
    }
    double sign = 0.0;
    size_t n = points.size();
    for (size_t i = 0; i < n; ++i) {
        const ScreenPoint &a = points[i];
        const ScreenPoint &b = points[(i+1) % n];
        double cr = (b.x-a.x)*(points[(i+2) % n].y-a.y)
                  - (b.y-a.y)*(points[(i+2) % n].x-a.x);
        if (fabs(cr) <= BEAM_EPS) continue;
        if (sign == 0.0) sign = cr > 0.0 ? 1.0 : -1.0;
        else if ((cr > 0.0) != (sign > 0.0)) {
            throw std::invalid_argument("beam film: sub-domain must be convex and consistently ordered");
        }
    }
    p.v = points;
    dedupe(p);
    if (p.v.size() < 3 || polygon_area(p) <= 0.0) {
        throw std::invalid_argument("beam film: sub-domain has zero area");
    }
    return p;
}

} // namespace

BeamTracer::BeamTracer(Scene &scene, Camera &camera)
    : m_scene(scene), m_camera(camera), m_has_film(false) {}

BeamFilm BeamTracer::render_frame(int depth) {
    validate_depth(depth);
    validate_camera(m_camera);
    m_scene.validate_for_beam();

    int W = m_camera.get_width();
    int H = m_camera.get_height();
    RayFamily fam = camera_family(m_camera);

    BeamFilm film;
    film.width = W;
    film.height = H;
    film.depth = depth;
    film.pixels.assign((size_t)W * (size_t)H, BeamPixel());
    for (size_t i = 0; i < film.pixels.size(); ++i) {
        film.pixels[i].by_bounce.assign((size_t)depth + 1, Vec());
        film.pixels[i].emitter_by_bounce.assign((size_t)depth + 1, 0.0);
    }

    BeamEngine engine(m_scene, m_camera, depth);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            Poly2 pixel;
            pixel.v.push_back(ScreenPoint((double)x, (double)y));
            pixel.v.push_back(ScreenPoint((double)x+1.0, (double)y));
            pixel.v.push_back(ScreenPoint((double)x+1.0, (double)y+1.0));
            pixel.v.push_back(ScreenPoint((double)x, (double)y+1.0));

            BeamPixel &bp = film.pixels[(size_t)y * (size_t)W + (size_t)x];
            engine.trace(fam, pixel, 0, Vec(1,1,1), false, Vec(), 0.0,
                         bp.by_bounce, bp.emitter_by_bounce,
                         bp.miss_area, bp.truncated_area);
            for (int k = 0; k <= depth; ++k) {
                bp.beauty = bp.beauty + bp.by_bounce[k];
                bp.emitter_area += bp.emitter_by_bounce[k];
            }
        }
    }

    film.nodes_visited = engine.stats.nodes_visited;
    film.candidate_faces = engine.stats.candidate_faces;
    film.clipped_faces = engine.stats.clipped_faces;

    // Commit only after the whole frame succeeded.
    m_last_film = film;
    m_has_film = true;
    return film;
}

BeamSubResult BeamTracer::query(const std::vector<ScreenPoint> &polygon, int depth) {
    validate_depth(depth);
    validate_camera(m_camera);
    m_scene.validate_for_beam();

    Poly2 domain = make_polygon(polygon);
    RayFamily fam = camera_family(m_camera);

    BeamSubResult res;
    res.area = polygon_area(domain);
    res.integral.assign((size_t)depth + 1, Vec());
    res.emitter_by_bounce.assign((size_t)depth + 1, 0.0);

    BeamEngine engine(m_scene, m_camera, depth);
    engine.trace(fam, domain, 0, Vec(1,1,1), false, Vec(), 0.0,
                 res.integral, res.emitter_by_bounce,
                 res.miss_area, res.truncated_area);
    for (int k = 0; k <= depth; ++k) res.emitter_area += res.emitter_by_bounce[k];
    res.nodes_visited = engine.stats.nodes_visited;
    res.candidate_faces = engine.stats.candidate_faces;
    res.clipped_faces = engine.stats.clipped_faces;
    return res;
}

void beamfilm_write_ppm(const BeamFilm &film, const std::string &path) {
    std::ofstream out(path.c_str(), std::ios::binary);
    if (!out) throw std::invalid_argument("beam film: cannot open output path: " + path);
    out << "P6\n" << film.width << " " << film.height << "\n255\n";
    for (size_t i = 0; i < film.pixels.size(); ++i) {
        const Vec &c = film.pixels[i].beauty;
        for (int ch = 0; ch < 3; ++ch) {
            double v = ch == 0 ? c.x : ch == 1 ? c.y : c.z;
            if (v < 0.0) v = 0.0;
            if (v > 1.0) v = 1.0;
            unsigned char b = (unsigned char)(v * 255.0 + 0.5);
            out.put((char)b);
        }
    }
}

void beamfilm_write_csv(const BeamFilm &film, const std::string &path) {
    std::ofstream out(path.c_str());
    if (!out) throw std::invalid_argument("beam film: cannot open output path: " + path);
    out << std::setprecision(17);
    for (int y = 0; y < film.height; ++y) {
        for (int x = 0; x < film.width; ++x) {
            const BeamPixel &bp = film.pixel(x, y);
            for (int k = 0; k <= film.depth; ++k) {
                const Vec &c = bp.by_bounce[k];
                out << y << "," << x << "," << k << ","
                    << c.x << "," << c.y << "," << c.z << ",";
                if (k == 0) {
                    out << bp.emitter_area << "," << bp.miss_area << "," << bp.truncated_area;
                } else {
                    out << "0,0,0";
                }
                out << "\n";
            }
        }
    }
}

unsigned long long kd_tree_node_count(KDNode *node) {
    if (node == NULL) return 0;
    unsigned long long n = 1;
    n += kd_tree_node_count(node->left);
    n += kd_tree_node_count(node->right);
    return n;
}
