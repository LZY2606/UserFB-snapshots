#include "config.h"

#define JC_VORONOI_PERIODIC_IMPLEMENTATION
#include "jc_voronoi_periodic.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static unsigned long g_assertions = 0;

#define CHECK(cond)                                                          \
    do                                                                       \
    {                                                                        \
        ++g_assertions;                                                      \
        if (!(cond))                                                         \
        {                                                                    \
            fprintf(stderr, "periodic_test failure line %d: %s\n",           \
                    __LINE__, #cond);                                        \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

static void expect_close(double actual, double expected, double scale,
                         int line)
{
    double tol = 2.0e-7 * (scale > 0.0 ? scale : 1.0);
    ++g_assertions;
    if (!(fabs(actual - expected) <= tol))
    {
        fprintf(stderr,
                "periodic_test failure line %d: %.17g != %.17g "
                "(diff %.3g, tol %.3g)\n",
                line, actual, expected, fabs(actual - expected), tol);
        exit(1);
    }
}

#define CHECK_CLOSE(a, b, scale) expect_close((double)(a), (double)(b), (double)(scale), __LINE__)

static double point_distance(jcv_point a, jcv_point b)
{
    return hypot((double)a.x - (double)b.x, (double)a.y - (double)b.y);
}

static void expect_point_close(jcv_point a, jcv_point b, double scale, int line)
{
    double tol = 2.0e-7 * (scale > 0.0 ? scale : 1.0);
    double d = point_distance(a, b);
    ++g_assertions;
    if (!(d <= tol))
    {
        fprintf(stderr,
                "periodic_test failure line %d: point (%.17g,%.17g) != "
                "(%.17g,%.17g), dist %.3g tol %.3g\n",
                line, a.x, a.y, b.x, b.y, d, tol);
        exit(1);
    }
}

#define CHECK_POINT_CLOSE(a, b, scale) expect_point_close((a), (b), (scale), __LINE__)

static int has_shift(const jcv_periodic_diagram* d, int cell, int neighbor,
                     int sx, int sy)
{
    const jcv_periodic_cell* c = &d->cells[cell];
    int k;
    for (k = 0; k < c->num_edges; ++k)
    {
        const jcv_periodic_edge* e = &d->edges[c->first_edge + k];
        if (e->neighbor == neighbor && e->shift_x == sx &&
            e->shift_y == sy)
            return 1;
    }
    return 0;
}

static int point_inside_translated_cell(const jcv_periodic_diagram* d, int id,
                                        jcv_point q, int tx, int ty,
                                        double tolerance)
{
    const jcv_periodic_cell* c = &d->cells[id];
    double W = d->rect.max.x - d->rect.min.x;
    double H = d->rect.max.y - d->rect.min.y;
    double ox = (double)tx * W;
    double oy = (double)ty * H;
    int k;

    for (k = 0; k < c->num_edges; ++k)
    {
        const jcv_periodic_edge* e = &d->edges[c->first_edge + k];
        double ax = (double)e->pos[0].x + ox;
        double ay = (double)e->pos[0].y + oy;
        double bx = (double)e->pos[1].x + ox;
        double by = (double)e->pos[1].y + oy;
        double ex = bx - ax;
        double ey = by - ay;
        double cross = ex * ((double)q.y - ay) - ey * ((double)q.x - ax);
        double elen = hypot(ex, ey);
        if (cross < -tolerance * elen)
            return 0;
    }
    return 1;
}

static void check_sampling(const jcv_periodic_diagram* d, int samples_per_side)
{
    double W = d->rect.max.x - d->rect.min.x;
    double H = d->rect.max.y - d->rect.min.y;
    double L = W > H ? W : H;
    double near_tol = 2.0e-7 * L;
    double inside_tol = 1.0e-9 * L;
    int ix, iy, i, sx, sy;

    for (iy = 0; iy < samples_per_side; ++iy)
    {
        double fy = samples_per_side == 1 ? 0.0 : (double)iy / (samples_per_side - 1);
        for (ix = 0; ix < samples_per_side; ++ix)
        {
            double fx = samples_per_side == 1 ? 0.0 : (double)ix / (samples_per_side - 1);
            jcv_point q;
            double best = 1.0e300;
            int containers = 0;
            int nearest_containers = 0;

            q.x = d->rect.min.x + (jcv_real)(fx * W);
            q.y = d->rect.min.y + (jcv_real)(fy * H);

            /* Independent direct comparison against the 3x3 seed grid. */
            for (i = 0; i < d->numsites; ++i)
            {
                for (sy = -1; sy <= 1; ++sy)
                {
                    for (sx = -1; sx <= 1; ++sx)
                    {
                        jcv_point s;
                        double dist;
                        s.x = d->cells[i].point.x + (jcv_real)sx * W;
                        s.y = d->cells[i].point.y + (jcv_real)sy * H;
                        dist = point_distance(q, s);
                        if (dist < best)
                            best = dist;
                    }
                }
            }

            for (i = 0; i < d->numsites; ++i)
            {
                for (sy = -1; sy <= 1; ++sy)
                {
                    for (sx = -1; sx <= 1; ++sx)
                    {
                        if (!point_inside_translated_cell(
                                d, i, q, sx, sy, inside_tol))
                            continue;

                        ++containers;
                        {
                            jcv_point s;
                            double dist;
                            s.x = d->cells[i].point.x + (jcv_real)sx * W;
                            s.y = d->cells[i].point.y + (jcv_real)sy * H;
                            dist = point_distance(q, s);
                            CHECK(fabs(dist - best) <= near_tol);
                            if (fabs(dist - best) <= near_tol)
                                ++nearest_containers;
                        }
                    }
                }
            }

            CHECK(containers > 0);
            CHECK(nearest_containers > 0);
        }
    }
}

static void validate_diagram(const jcv_periodic_diagram* d, int n,
                             const jcv_point* normalized_expected,
                             int sample_grid)
{
    double W, H, L, area_sum = 0.0;
    int i, k;

    CHECK(d != NULL);
    CHECK(d->numsites == n);
    CHECK(d->numsites >= 1 && d->numsites <= 256);
    CHECK(d->cells != NULL);
    CHECK(d->numedges > 0);
    CHECK(d->edges != NULL);

    W = d->rect.max.x - d->rect.min.x;
    H = d->rect.max.y - d->rect.min.y;
    L = W > H ? W : H;
    {
        double eps_w = 1.0e-12 * (1.0 + fabs(d->rect.min.x) + fabs(d->rect.max.x));
        double eps_h = 1.0e-12 * (1.0 + fabs(d->rect.min.y) + fabs(d->rect.max.y));
        CHECK(W >= 0.01 - eps_w && W <= 100.0 + eps_w);
        CHECK(H >= 0.01 - eps_h && H <= 100.0 + eps_h);
    }

    for (i = 0; i < n; ++i)
    {
        const jcv_periodic_cell* c = &d->cells[i];
        CHECK(c->num_edges >= 3);
        CHECK(c->first_edge >= 0);
        CHECK(c->first_edge + c->num_edges <= d->numedges);
        if (i + 1 < n)
            CHECK(d->cells[i + 1].first_edge ==
                  c->first_edge + c->num_edges);
        CHECK(isfinite(c->point.x) && isfinite(c->point.y));
        CHECK(c->point.x >= d->rect.min.x);
        CHECK(c->point.x < d->rect.max.x);
        CHECK(c->point.y >= d->rect.min.y);
        CHECK(c->point.y < d->rect.max.y);
        if (normalized_expected)
            CHECK_POINT_CLOSE(c->point, normalized_expected[i], L);

        for (k = 0; k < c->num_edges; ++k)
        {
            int ei = c->first_edge + k;
            int en = c->first_edge + ((k + 1) % c->num_edges);
            const jcv_periodic_edge* e = &d->edges[ei];
            const jcv_periodic_edge* next = &d->edges[en];
            jcv_point neighbor_point;
            jcv_point shifted[2];
            jcv_point midpoint;
            jcv_point interior_point;
            double len = point_distance(e->pos[0], e->pos[1]);

            CHECK(ei >= 0 && ei < d->numedges);
            CHECK(len > 0.0);
            CHECK_POINT_CLOSE(e->pos[1], next->pos[0], L);
            CHECK(e->neighbor >= 0 && e->neighbor < n);
            CHECK(e->shift_x >= -1 && e->shift_x <= 1);
            CHECK(e->shift_y >= -1 && e->shift_y <= 1);

            neighbor_point.x =
                d->cells[e->neighbor].point.x + (jcv_real)e->shift_x * W;
            neighbor_point.y =
                d->cells[e->neighbor].point.y + (jcv_real)e->shift_y * H;

            shifted[0] = e->pos[0];
            shifted[1] = e->pos[1];
            CHECK_CLOSE(point_distance(shifted[0], c->point),
                        point_distance(shifted[0], neighbor_point), L);
            CHECK_CLOSE(point_distance(shifted[1], c->point),
                        point_distance(shifted[1], neighbor_point), L);

            midpoint.x = (e->pos[0].x + e->pos[1].x) * 0.5;
            midpoint.y = (e->pos[0].y + e->pos[1].y) * 0.5;
            CHECK_CLOSE(point_distance(midpoint, c->point),
                        point_distance(midpoint, neighbor_point), L);

            interior_point.x = 0.31 * e->pos[0].x + 0.69 * e->pos[1].x;
            interior_point.y = 0.31 * e->pos[0].y + 0.69 * e->pos[1].y;
            CHECK_CLOSE(point_distance(interior_point, c->point),
                        point_distance(interior_point, neighbor_point), L);
        }

        /* Independent polygon moments in coordinates local to rect.min. */
        {
            double area2 = 0.0, cx6 = 0.0, cy6 = 0.0;
            double area, cx, cy;
            for (k = 0; k < c->num_edges; ++k)
            {
                const jcv_periodic_edge* e = &d->edges[c->first_edge + k];
                double ax = (double)e->pos[0].x - d->rect.min.x;
                double ay = (double)e->pos[0].y - d->rect.min.y;
                double bx = (double)e->pos[1].x - d->rect.min.x;
                double by = (double)e->pos[1].y - d->rect.min.y;
                double cross = ax * by - bx * ay;
                area2 += cross;
                cx6 += (ax + bx) * cross;
                cy6 += (ay + by) * cross;
            }
            CHECK(area2 > 0.0);
            area = area2 * 0.5;
            cx = d->rect.min.x + cx6 / (3.0 * area2);
            cy = d->rect.min.y + cy6 / (3.0 * area2);
            CHECK_CLOSE(c->area, area, W * H);
            CHECK_CLOSE(c->centroid.x, cx, L);
            CHECK_CLOSE(c->centroid.y, cy, L);
            area_sum += c->area;
        }
    }

    CHECK_CLOSE(area_sum, W * H, W * H);

    /* Reverse adjacency after translating the current edge by its shift. */
    for (i = 0; i < n; ++i)
    {
        int k;
        const jcv_periodic_cell* c = &d->cells[i];
        for (k = 0; k < c->num_edges; ++k)
        {
            const jcv_periodic_edge* e = &d->edges[c->first_edge + k];
            const jcv_periodic_cell* nc = &d->cells[e->neighbor];
            double shift_x = (double)e->shift_x * W;
            double shift_y = (double)e->shift_y * H;
            double best = 1.0e300;
            int m;

            for (m = 0; m < nc->num_edges; ++m)
            {
                const jcv_periodic_edge* f =
                    &d->edges[nc->first_edge + m];
                jcv_point expected_f0, expected_f1;
                double err;

                if (f->neighbor != i ||
                    f->shift_x != -e->shift_x ||
                    f->shift_y != -e->shift_y)
                    continue;

                expected_f0.x = e->pos[1].x - (jcv_real)shift_x;
                expected_f0.y = e->pos[1].y - (jcv_real)shift_y;
                expected_f1.x = e->pos[0].x - (jcv_real)shift_x;
                expected_f1.y = e->pos[0].y - (jcv_real)shift_y;
                err = point_distance(f->pos[0], expected_f0) +
                      point_distance(f->pos[1], expected_f1);
                if (err < best)
                    best = err;
            }
            CHECK(best <= 2.0e-7 * L);
        }
    }

    if (sample_grid > 0)
        check_sampling(d, sample_grid);
}

static void expect_same_diagram(const jcv_periodic_diagram* a,
                               const jcv_periodic_diagram* b, int exact)
{
    int i;
    double W = a->rect.max.x - a->rect.min.x;
    double H = a->rect.max.y - a->rect.min.y;
    double L = W > H ? W : H;
    CHECK(a->numsites == b->numsites);
    CHECK(a->numedges == b->numedges);
    for (i = 0; i < a->numsites; ++i)
    {
        CHECK(a->cells[i].first_edge == b->cells[i].first_edge);
        CHECK(a->cells[i].num_edges == b->cells[i].num_edges);
        if (exact)
        {
            CHECK(a->cells[i].point.x == b->cells[i].point.x);
            CHECK(a->cells[i].point.y == b->cells[i].point.y);
            CHECK(a->cells[i].area == b->cells[i].area);
            CHECK(a->cells[i].centroid.x == b->cells[i].centroid.x);
            CHECK(a->cells[i].centroid.y == b->cells[i].centroid.y);
        }
        else
        {
            CHECK_POINT_CLOSE(a->cells[i].point, b->cells[i].point, L);
            CHECK_CLOSE(a->cells[i].area, b->cells[i].area, W * H);
            CHECK_POINT_CLOSE(a->cells[i].centroid, b->cells[i].centroid, L);
        }
    }
    for (i = 0; i < a->numedges; ++i)
    {
        if (exact)
        {
            CHECK(a->edges[i].pos[0].x == b->edges[i].pos[0].x);
            CHECK(a->edges[i].pos[0].y == b->edges[i].pos[0].y);
            CHECK(a->edges[i].pos[1].x == b->edges[i].pos[1].x);
            CHECK(a->edges[i].pos[1].y == b->edges[i].pos[1].y);
        }
        else
        {
            CHECK_POINT_CLOSE(a->edges[i].pos[0], b->edges[i].pos[0], L);
            CHECK_POINT_CLOSE(a->edges[i].pos[1], b->edges[i].pos[1], L);
        }
        CHECK(a->edges[i].neighbor == b->edges[i].neighbor);
        CHECK(a->edges[i].shift_x == b->edges[i].shift_x);
        CHECK(a->edges[i].shift_y == b->edges[i].shift_y);
    }
}

typedef struct diagram_snapshot_
{
    int numsites;
    int numedges;
    jcv_periodic_cell* cells;
    jcv_periodic_edge* edges;
} diagram_snapshot;

static void snapshot_make(diagram_snapshot* s, const jcv_periodic_diagram* d)
{
    s->numsites = d->numsites;
    s->numedges = d->numedges;
    s->cells = (jcv_periodic_cell*)malloc((size_t)d->numsites * sizeof(*s->cells));
    s->edges = (jcv_periodic_edge*)malloc((size_t)d->numedges * sizeof(*s->edges));
    CHECK(s->cells != NULL);
    CHECK(s->edges != NULL);
    memcpy(s->cells, d->cells, (size_t)d->numsites * sizeof(*s->cells));
    memcpy(s->edges, d->edges, (size_t)d->numedges * sizeof(*s->edges));
}

static void snapshot_check(const diagram_snapshot* s,
                           const jcv_periodic_diagram* d)
{
    CHECK(d->numsites == s->numsites);
    CHECK(d->numedges == s->numedges);
    CHECK(memcmp(d->cells, s->cells,
                 (size_t)s->numsites * sizeof(*s->cells)) == 0);
    CHECK(memcmp(d->edges, s->edges,
                 (size_t)s->numedges * sizeof(*s->edges)) == 0);
}

static void snapshot_free(diagram_snapshot* s)
{
    free(s->cells);
    free(s->edges);
    s->cells = NULL;
    s->edges = NULL;
}

int main(void)
{
    jcv_rect unit = {{0.0, 0.0}, {1.0, 1.0}};

    /* One off-center seed: one rectangular expanded cell, four self edges. */
    {
        jcv_point p[1] = {{0.13, 0.37}};
        jcv_periodic_diagram d, d2;
        memset(&d, 0, sizeof(d));
        CHECK(jcv_periodic_generate(1, p, &unit, &d) == 1);
        validate_diagram(&d, 1, p, 65);
        CHECK(d.cells[0].num_edges == 4);
        CHECK_CLOSE(d.cells[0].area, 1.0, 1.0);
        CHECK_POINT_CLOSE(d.cells[0].centroid, p[0], 1.0);
        CHECK(has_shift(&d, 0, 0, 1, 0));
        CHECK(has_shift(&d, 0, 0, -1, 0));
        CHECK(has_shift(&d, 0, 0, 0, 1));
        CHECK(has_shift(&d, 0, 0, 0, -1));
        CHECK(!has_shift(&d, 0, 0, 1, 1));
        {
            int k;
            for (k = 0; k < d.cells[0].num_edges; ++k)
            {
                double len = point_distance(d.edges[k].pos[0], d.edges[k].pos[1]);
                CHECK_CLOSE(len, 1.0, 1.0);
            }
        }

        memset(&d2, 0, sizeof(d2));
        CHECK(jcv_periodic_generate(1, p, &unit, &d2) == 1);
        expect_same_diagram(&d, &d2, 1);
        jcv_periodic_free(&d2);
        jcv_periodic_free(&d);
    }

    /* The explicit two-seed case: two horizontal images and vertical self. */
    {
        jcv_point p[2] = {{0.1, 0.2}, {0.6, 0.2}};
        jcv_periodic_diagram d;
        memset(&d, 0, sizeof(d));
        CHECK(jcv_periodic_generate(2, p, &unit, &d) == 1);
        validate_diagram(&d, 2, p, 81);
        CHECK(d.cells[0].num_edges == 4);
        CHECK(d.cells[1].num_edges == 4);
        CHECK(has_shift(&d, 0, 1, 0, 0));
        CHECK(has_shift(&d, 0, 1, -1, 0));
        CHECK(has_shift(&d, 0, 0, 0, 1));
        CHECK(has_shift(&d, 0, 0, 0, -1));
        CHECK(has_shift(&d, 1, 0, 0, 0));
        CHECK(has_shift(&d, 1, 0, 1, 0));
        CHECK(has_shift(&d, 1, 1, 0, 1));
        CHECK(has_shift(&d, 1, 1, 0, -1));
        CHECK_CLOSE(d.cells[0].area, 0.5, 1.0);
        CHECK_CLOSE(d.cells[1].area, 0.5, 1.0);
        jcv_periodic_free(&d);
    }

    /* Required asymmetric four-point set. */
    {
        jcv_point p[4] = {
            {0.08, 0.17}, {0.58, 0.22}, {0.33, 0.81}, {0.87, 0.69}
        };
        jcv_periodic_diagram d, repeat;
        memset(&d, 0, sizeof(d));
        CHECK(jcv_periodic_generate(4, p, &unit, &d) == 1);
        validate_diagram(&d, 4, p, 97);
        memset(&repeat, 0, sizeof(repeat));
        CHECK(jcv_periodic_generate(4, p, &unit, &repeat) == 1);
        expect_same_diagram(&d, &repeat, 1);
        jcv_periodic_free(&repeat);
        CHECK(d.cells[0].num_edges == 6);
        CHECK(d.cells[1].num_edges == 6);
        CHECK(d.cells[2].num_edges == 6);
        CHECK(d.cells[3].num_edges == 6);
        /* This set uses both directions of the seam. */
        CHECK(has_shift(&d, 0, 3, -1, -1));
        CHECK(has_shift(&d, 3, 0, 1, 1));
        jcv_periodic_free(&d);
    }

    /* Explicit biaxial seam-crossing set. */
    {
        jcv_point p[4] = {
            {0.03, 0.04}, {0.97, 0.05}, {0.04, 0.96}, {0.96, 0.95}
        };
        jcv_periodic_diagram d;
        memset(&d, 0, sizeof(d));
        CHECK(jcv_periodic_generate(4, p, &unit, &d) == 1);
        validate_diagram(&d, 4, p, 65);
        jcv_periodic_free(&d);
    }

    /* Non-square rectangle with a nonzero origin. */
    {
        jcv_rect r = {{3.0, -2.0}, {5.0, -1.0}}; /* W=2, H=1 */
        jcv_point p[4] = {
            {3.16, -1.83}, {4.16, -1.78}, {3.66, -1.19}, {4.74, -1.31}
        };
        jcv_periodic_diagram d;
        jcv_point expected[4];
        int i;
        for (i = 0; i < 4; ++i)
            expected[i] = p[i];
        memset(&d, 0, sizeof(d));
        CHECK(jcv_periodic_generate(4, p, &r, &d) == 1);
        validate_diagram(&d, 4, expected, 49);
        jcv_periodic_free(&d);
    }

    /* Maximum permitted aspect ratio. */
    {
        jcv_rect r = {{-0.4, 7.0}, {1.2, 7.1}}; /* W=1.6, H=.1, ratio 16 */
        jcv_point p[3] = {
            {-0.25, 7.03}, {0.35, 7.08}, {0.95, 7.04}
        };
        jcv_periodic_diagram d;
        memset(&d, 0, sizeof(d));
        CHECK(jcv_periodic_generate(3, p, &r, &d) == 1);
        validate_diagram(&d, 3, NULL, 33);
        jcv_periodic_free(&d);
    }

    /* Minimum side length at the maximum aspect ratio and large origin. */
    {
        jcv_rect r = {{-1000.0, 1000.0}, {-999.84, 1000.01}};
        jcv_point p[3] = {
            {-999.98, 1000.003}, {-999.92, 1000.007}, {-999.86, 1000.004}
        };
        jcv_periodic_diagram d;
        memset(&d, 0, sizeof(d));
        CHECK(jcv_periodic_generate(3, p, &r, &d) == 1);
        validate_diagram(&d, 3, NULL, 25);
        jcv_periodic_free(&d);
    }

    /* Collinear horizontal and vertical seeds. */
    {
        jcv_point ph[4] = {{0.1, 0.5}, {0.35, 0.5}, {0.6, 0.5}, {0.9, 0.5}};
        jcv_point pv[3] = {{0.4, 0.12}, {0.4, 0.48}, {0.4, 0.83}};
        jcv_point pd[5] = {
            {0.10, 0.10}, {0.27, 0.27}, {0.50, 0.50},
            {0.73, 0.73}, {0.90, 0.90}
        };
        jcv_point many_col[64];
        jcv_periodic_diagram dh, dv, dd, dcol;
        int ci;
        for (ci = 0; ci < 64; ++ci)
            many_col[ci] = (jcv_point){(ci + 0.5) / 64.0, 0.5};
        memset(&dh, 0, sizeof(dh));
        memset(&dv, 0, sizeof(dv));
        memset(&dd, 0, sizeof(dd));
        memset(&dcol, 0, sizeof(dcol));
        CHECK(jcv_periodic_generate(4, ph, &unit, &dh) == 1);
        validate_diagram(&dh, 4, ph, 41);
        CHECK(jcv_periodic_generate(3, pv, &unit, &dv) == 1);
        validate_diagram(&dv, 3, pv, 41);
        CHECK(jcv_periodic_generate(5, pd, &unit, &dd) == 1);
        validate_diagram(&dd, 5, pd, 33);
        CHECK(jcv_periodic_generate(64, many_col, &unit, &dcol) == 1);
        validate_diagram(&dcol, 64, many_col, 0);
        jcv_periodic_free(&dd);
        jcv_periodic_free(&dcol);
        jcv_periodic_free(&dh);
        jcv_periodic_free(&dv);
    }

    /* Exact 2x2 grid. */
    {
        jcv_point p[4] = {
            {0.25, 0.25}, {0.75, 0.25}, {0.25, 0.75}, {0.75, 0.75}
        };
        jcv_periodic_diagram d;
        memset(&d, 0, sizeof(d));
        CHECK(jcv_periodic_generate(4, p, &unit, &d) == 1);
        validate_diagram(&d, 4, p, 65);
        CHECK(d.cells[0].num_edges == 4);
        CHECK_CLOSE(d.cells[0].area, 0.25, 1.0);
        CHECK_POINT_CLOSE(d.cells[0].centroid, p[0], 1.0);
        jcv_periodic_free(&d);
    }

    /* Maximum supported seed count on a regular 16x16 torus grid. */
    {
        jcv_point p[256];
        int y, x, n = 0;
        jcv_periodic_diagram d;
        for (y = 0; y < 16; ++y)
            for (x = 0; x < 16; ++x)
                p[n++] = (jcv_point){(x + 0.5) / 16.0, (y + 0.5) / 16.0};
        memset(&d, 0, sizeof(d));
        CHECK(jcv_periodic_generate(n, p, &unit, &d) == 1);
        validate_diagram(&d, 256, p, 17);
        jcv_periodic_free(&d);
    }

    /* Translation covariance, permutation, synchronized relaxation, and
       preservation of an old diagram while newer ones are generated. */
    {
        jcv_point base[4] = {
            {0.08, 0.17}, {0.58, 0.22}, {0.33, 0.81}, {0.87, 0.69}
        };
        int permutation[4] = {2, 0, 3, 1};
        jcv_periodic_diagram old_d, perm_d, translated_d, frame_d;
        diagram_snapshot snapshot;
        jcv_point current[4], next_points[4];
        int round, i;

        memset(&old_d, 0, sizeof(old_d));
        memset(&perm_d, 0, sizeof(perm_d));
        memset(&translated_d, 0, sizeof(translated_d));
        memset(&frame_d, 0, sizeof(frame_d));

        CHECK(jcv_periodic_generate(4, base, &unit, &old_d) == 1);
        snapshot_make(&snapshot, &old_d);

        /* alpha=0 preserves normalized seeds. */
        CHECK(jcv_periodic_relax(&old_d, 0.0, next_points) == 1);
        for (i = 0; i < 4; ++i)
        {
            CHECK(next_points[i].x == old_d.cells[i].point.x);
            CHECK(next_points[i].y == old_d.cells[i].point.y);
        }

        /* alpha=0.5 is exactly point + half centroid displacement. */
        CHECK(jcv_periodic_relax(&old_d, 0.5, next_points) == 1);
        for (i = 0; i < 4; ++i)
        {
            jcv_point expected;
            expected.x = old_d.cells[i].point.x +
                         0.5 * (old_d.cells[i].centroid.x - old_d.cells[i].point.x);
            expected.y = old_d.cells[i].point.y +
                         0.5 * (old_d.cells[i].centroid.y - old_d.cells[i].point.y);
            expected.x = expected.x < 1.0 ? expected.x : expected.x - 1.0;
            expected.y = expected.y < 1.0 ? expected.y : expected.y - 1.0;
            CHECK_POINT_CLOSE(next_points[i], expected, 1.0);
        }

        /* alpha=1 wraps the area centroid back into the rectangle. */
        CHECK(jcv_periodic_relax(&old_d, 1.0, next_points) == 1);
        for (i = 0; i < 4; ++i)
        {
            jcv_point expected = old_d.cells[i].centroid;
            if (expected.x >= 1.0) expected.x -= 1.0;
            if (expected.y >= 1.0) expected.y -= 1.0;
            if (expected.x < 0.0) expected.x += 1.0;
            if (expected.y < 0.0) expected.y += 1.0;
            CHECK_POINT_CLOSE(next_points[i], expected, 1.0);
        }

        /* Three synchronized relax-and-regenerate rounds. */
        for (i = 0; i < 4; ++i)
            current[i] = base[i];
        for (round = 0; round < 3; ++round)
        {
            jcv_periodic_diagram rd;
            jcv_point expected[4];
            memset(&rd, 0, sizeof(rd));
            CHECK(jcv_periodic_generate(4, current, &unit, &rd) == 1);
            for (i = 0; i < 4; ++i)
            {
                expected[i].x = rd.cells[i].point.x +
                    0.5 * (rd.cells[i].centroid.x - rd.cells[i].point.x);
                expected[i].y = rd.cells[i].point.y +
                    0.5 * (rd.cells[i].centroid.y - rd.cells[i].point.y);
                expected[i].x -= floor(expected[i].x);
                expected[i].y -= floor(expected[i].y);
            }
            CHECK(jcv_periodic_relax(&rd, 0.5, next_points) == 1);
            for (i = 0; i < 4; ++i)
                CHECK_POINT_CLOSE(next_points[i], expected[i], 1.0);
            for (i = 0; i < 4; ++i)
                current[i] = next_points[i];
            jcv_periodic_free(&rd);
        }
        {
            jcv_periodic_diagram final_d;
            memset(&final_d, 0, sizeof(final_d));
            CHECK(jcv_periodic_generate(4, current, &unit, &final_d) == 1);
            validate_diagram(&final_d, 4, NULL, 49);
            jcv_periodic_free(&final_d);
        }

        /* Input permutation with no equal y/x sorting keys. */
        {
            jcv_point permuted[4];
            for (i = 0; i < 4; ++i)
                permuted[i] = base[permutation[i]];
            CHECK(jcv_periodic_generate(4, permuted, &unit, &perm_d) == 1);
            validate_diagram(&perm_d, 4, permuted, 33);
            for (i = 0; i < 4; ++i)
            {
                const jcv_periodic_cell* a = &old_d.cells[permutation[i]];
                const jcv_periodic_cell* b = &perm_d.cells[i];
                CHECK_CLOSE(b->area, a->area, 1.0);
                CHECK_POINT_CLOSE(b->centroid, a->centroid, 1.0);
                CHECK(b->num_edges == a->num_edges);
            }
        }

        /* Whole-period point translation is normalization-invariant. */
        {
            jcv_point shifted[4];
            for (i = 0; i < 4; ++i)
                shifted[i] = (jcv_point){base[i].x + 3.0, base[i].y - 2.0};
            CHECK(jcv_periodic_generate(4, shifted, &unit, &translated_d) == 1);
            validate_diagram(&translated_d, 4, base, 0);
            expect_same_diagram(&old_d, &translated_d, 0);
        }

        /* Translating the rectangle and points together translates output. */
        {
            jcv_rect tr = {{12.5, -7.25}, {13.5, -6.25}};
            jcv_point shifted[4];
            for (i = 0; i < 4; ++i)
                shifted[i] = (jcv_point){
                    base[i].x + tr.min.x, base[i].y + tr.min.y};
            CHECK(jcv_periodic_generate(4, shifted, &tr, &frame_d) == 1);
            CHECK(frame_d.cells[0].point.x >= tr.min.x);
            for (i = 0; i < 4; ++i)
            {
                jcv_point expected_point = {
                    old_d.cells[i].point.x + tr.min.x,
                    old_d.cells[i].point.y + tr.min.y};
                jcv_point expected_centroid = {
                    old_d.cells[i].centroid.x + tr.min.x,
                    old_d.cells[i].centroid.y + tr.min.y};
                CHECK_POINT_CLOSE(frame_d.cells[i].point, expected_point, 1.0);
                CHECK_POINT_CLOSE(frame_d.cells[i].centroid,
                                  expected_centroid, 1.0);
            }
            validate_diagram(&frame_d, 4, NULL, 0);
        }

        /* Arbitrary common translation: each unwrapped cell follows its
           lattice copy, rather than jumping toward the rectangle center. */
        {
            jcv_point moved_input[4];
            jcv_real tx = (jcv_real)0.73;
            jcv_real ty = (jcv_real)0.81;
            jcv_periodic_diagram moved_d;
            memset(&moved_d, 0, sizeof(moved_d));
            for (i = 0; i < 4; ++i)
                moved_input[i] = (jcv_point){base[i].x + tx, base[i].y + ty};
            CHECK(jcv_periodic_generate(4, moved_input, &unit, &moved_d) == 1);
            validate_diagram(&moved_d, 4, NULL, 33);
            for (i = 0; i < 4; ++i)
            {
                jcv_real dx = moved_d.cells[i].point.x - old_d.cells[i].point.x;
                jcv_real dy = moved_d.cells[i].point.y - old_d.cells[i].point.y;
                jcv_point expected_centroid = {
                    old_d.cells[i].centroid.x + dx,
                    old_d.cells[i].centroid.y + dy};
                CHECK_CLOSE(moved_d.cells[i].area, old_d.cells[i].area, 1.0);
                CHECK_POINT_CLOSE(moved_d.cells[i].centroid,
                                  expected_centroid, 1.0);
            }
            jcv_periodic_free(&moved_d);
        }

        snapshot_check(&snapshot, &old_d);
        jcv_periodic_free(&translated_d);
        jcv_periodic_free(&frame_d);
        jcv_periodic_free(&perm_d);
        snapshot_check(&snapshot, &old_d);
        snapshot_free(&snapshot);
        jcv_periodic_free(&old_d);
    }

    /* Exact ten-period displacement is an accepted boundary. */
    {
        jcv_point boundary[1] = {{10.0, -10.0}};
        jcv_periodic_diagram d;
        memset(&d, 0, sizeof(d));
        CHECK(jcv_periodic_generate(1, boundary, &unit, &d) == 1);
        CHECK_POINT_CLOSE(d.cells[0].point, ((jcv_point){0.0, 0.0}), 1.0);
        jcv_periodic_free(&d);
    }

    /* Invalid generate inputs leave an all-zero output. */
    {
        jcv_point good[2] = {{0.2, 0.2}, {0.7, 0.8}};
        jcv_point bad_nan[2] = {{NAN, 0.2}, {0.7, 0.8}};
        jcv_point bad_inf[2] = {{INFINITY, 0.2}, {0.7, 0.8}};
        jcv_point same[2] = {{0.2, 0.2}, {0.2, 0.2}};
        jcv_point periodic_same[2] = {{0.2, 0.2}, {1.2, 0.2}};
        jcv_point seam_close[2] = {{1.0e-12, 0.5}, {1.0 - 1.0e-12, 0.5}};
        jcv_point threshold_close[2] = {{0.0, 0.0}, {1.0e-10, 0.0}};
        jcv_rect origin_bad = {{1000.5, 0}, {1001.5, 1}};
        jcv_rect small_w = {{0, 0}, {0.009, 0.01}};
        jcv_rect large_h = {{0, 0}, {1, 101}};
        jcv_rect aspect_bad = {{0, 0}, {1.7, 0.1}};
        jcv_rect nan_rect = {{0, 0}, {NAN, 1}};
        jcv_point far[1] = {{10.5, 0.5}};
        jcv_periodic_diagram out;

        memset(&out, 0, sizeof(out));
        CHECK(jcv_periodic_generate(0, good, &unit, &out) == 0);
        CHECK(out.cells == NULL && out.edges == NULL && out.numsites == 0);

        CHECK(jcv_periodic_generate(-1, good, &unit, &out) == 0);
        CHECK(out.cells == NULL && out.edges == NULL);

        CHECK(jcv_periodic_generate(257, good, &unit, &out) == 0);
        CHECK(out.cells == NULL && out.edges == NULL);

        CHECK(jcv_periodic_generate(2, NULL, &unit, &out) == 0);
        CHECK(out.cells == NULL && out.edges == NULL);

        CHECK(jcv_periodic_generate(2, good, NULL, &out) == 0);
        CHECK(out.cells == NULL && out.edges == NULL);

        CHECK(jcv_periodic_generate(2, good, &unit, NULL) == 0);

        CHECK(jcv_periodic_generate(2, bad_nan, &unit, &out) == 0);
        CHECK(out.cells == NULL && out.edges == NULL);
        CHECK(jcv_periodic_generate(2, bad_inf, &unit, &out) == 0);
        CHECK(out.cells == NULL && out.edges == NULL);

        CHECK(jcv_periodic_generate(2, good, &origin_bad, &out) == 0);
        CHECK(jcv_periodic_generate(2, good, &small_w, &out) == 0);
        CHECK(jcv_periodic_generate(2, good, &large_h, &out) == 0);
        CHECK(jcv_periodic_generate(2, good, &aspect_bad, &out) == 0);
        CHECK(jcv_periodic_generate(2, good, &nan_rect, &out) == 0);
        CHECK(out.cells == NULL && out.edges == NULL);

        CHECK(jcv_periodic_generate(1, far, &unit, &out) == 0);
        CHECK(out.cells == NULL && out.edges == NULL);

        CHECK(jcv_periodic_generate(2, same, &unit, &out) == 0);
        CHECK(out.cells == NULL && out.edges == NULL);
        CHECK(jcv_periodic_generate(2, periodic_same, &unit, &out) == 0);
        CHECK(out.cells == NULL && out.edges == NULL);
        CHECK(jcv_periodic_generate(2, seam_close, &unit, &out) == 0);
        CHECK(out.cells == NULL && out.edges == NULL);
        CHECK(jcv_periodic_generate(2, threshold_close, &unit, &out) == 0);
        CHECK(out.cells == NULL && out.edges == NULL);
    }

    /* Invalid relax arguments do not touch caller output. */
    {
        jcv_point p[2] = {{0.2, 0.2}, {0.7, 0.8}};
        jcv_periodic_diagram d;
        jcv_point out[3] = {{-42.0, -43.0}, {-44.0, -45.0}, {-46.0, -47.0}};
        jcv_point untouched[3];
        memcpy(untouched, out, sizeof(out));
        memset(&d, 0, sizeof(d));
        CHECK(jcv_periodic_generate(2, p, &unit, &d) == 1);

        CHECK(jcv_periodic_relax(NULL, 0.5, out) == 0);
        CHECK(memcmp(out, untouched, sizeof(out)) == 0);
        CHECK(jcv_periodic_relax(&d, 0.5, NULL) == 0);
        CHECK(memcmp(out, untouched, sizeof(out)) == 0);
        CHECK(jcv_periodic_relax(&d, -0.1, out) == 0);
        CHECK(memcmp(out, untouched, sizeof(out)) == 0);
        CHECK(jcv_periodic_relax(&d, 1.1, out) == 0);
        CHECK(memcmp(out, untouched, sizeof(out)) == 0);
        CHECK(jcv_periodic_relax(&d, NAN, out) == 0);
        CHECK(memcmp(out, untouched, sizeof(out)) == 0);
        CHECK(jcv_periodic_relax(&d, INFINITY, out) == 0);
        CHECK(memcmp(out, untouched, sizeof(out)) == 0);

        jcv_periodic_free(&d);
    }

    /* Repeated free and NULL free. */
    {
        jcv_point p[1] = {{0.3, 0.6}};
        jcv_periodic_diagram d;
        memset(&d, 0, sizeof(d));
        CHECK(jcv_periodic_generate(1, p, &unit, &d) == 1);
        CHECK(d.cells != NULL && d.edges != NULL);
        jcv_periodic_free(&d);
        CHECK(d.cells == NULL && d.edges == NULL);
        CHECK(d.numsites == 0 && d.numedges == 0);
        jcv_periodic_free(&d);
        jcv_periodic_free(NULL);
    }

    printf("PERIODIC_OK assertions=%lu\n", g_assertions);
    return 0;
}
