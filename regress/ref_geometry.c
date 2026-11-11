/*
 * regress/ref_geometry.c
 *
 * Independent analytic fill reference.  See ref_geometry.h for the contract.
 *
 * The implementation deliberately shares no rasterization code with the
 * product: horizontal-ray crossings of lines and cubic Beziers are solved
 * directly (cubics are split at their y-extrema, then bisected on each
 * y-monotone piece), winding/parity are accumulated from directed/transverse
 * crossings, and pixel coverage is area-integrated from the resulting fill
 * intervals.  Geometric error bands for flattened curves are derived by
 * replaying the product's fixed-point subdivision rules and measuring the
 * true curve against that chord polygon.
 */
#include "ref_geometry.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define R_EPS 1e-12

void rcontour_init(rcontour_t* c)
{
    c->data = NULL;
    c->size = 0;
    c->capacity = 0;
    c->closed = 0;
}

void rcontour_destroy(rcontour_t* c)
{
    free(c->data);
    rcontour_init(c);
}

static void rcontour_push(rcontour_t* c, const rseg_t* seg)
{
    if(c->size == c->capacity) {
        c->capacity = c->capacity ? c->capacity * 2 : 8;
        c->data = (rseg_t*)realloc(c->data, c->capacity * sizeof(rseg_t));
    }
    c->data[c->size++] = *seg;
}

void rcontour_move_to(rcontour_t* c, double x, double y)
{
    rseg_t seg;
    seg.type = RSEG_LINE;
    seg.p[0].x = seg.p[1].x = x;
    seg.p[0].y = seg.p[1].y = y;
    /* A zero-length marker records the sub-path origin; rseg_start_point()
       treats the first segment's p0 as the move-to position. */
    rcontour_push(c, &seg);
}

void rcontour_line_to(rcontour_t* c, double x, double y)
{
    rseg_t seg;
    seg.type = RSEG_LINE;
    seg.p[1].x = x;
    seg.p[1].y = y;
    seg.p[0] = c->data[c->size - 1].p[c->data[c->size - 1].type == RSEG_CUBIC ? 3 : 1];
    rcontour_push(c, &seg);
}

void rcontour_cubic_to(rcontour_t* c, double x1, double y1,
    double x2, double y2, double x3, double y3)
{
    rseg_t seg;
    seg.type = RSEG_CUBIC;
    seg.p[0] = c->data[c->size - 1].p[c->data[c->size - 1].type == RSEG_CUBIC ? 3 : 1];
    seg.p[1].x = x1; seg.p[1].y = y1;
    seg.p[2].x = x2; seg.p[2].y = y2;
    seg.p[3].x = x3; seg.p[3].y = y3;
    rcontour_push(c, &seg);
}

void rcontour_close(rcontour_t* c)
{
    c->closed = 1;
}

void rshape_init(rshape_t* s, int rule)
{
    s->contours = NULL;
    s->size = 0;
    s->capacity = 0;
    s->rule = rule;
    s->matrix.a = 1; s->matrix.b = 0; s->matrix.c = 0;
    s->matrix.d = 1; s->matrix.e = 0; s->matrix.f = 0;
}

void rshape_destroy(rshape_t* s)
{
    for(int i = 0; i < s->size; i++)
        rcontour_destroy(&s->contours[i]);
    free(s->contours);
    s->contours = NULL;
    s->size = s->capacity = 0;
}

rcontour_t* rshape_begin_contour(rshape_t* s)
{
    if(s->size == s->capacity) {
        s->capacity = s->capacity ? s->capacity * 2 : 8;
        s->contours = (rcontour_t*)realloc(s->contours,
            s->capacity * sizeof(rcontour_t));
    }
    rcontour_init(&s->contours[s->size]);
    return &s->contours[s->size++];
}

void rshape_set_matrix(rshape_t* s, double a, double b, double c,
    double d, double e, double f)
{
    s->matrix.a = a; s->matrix.b = b; s->matrix.c = c;
    s->matrix.d = d; s->matrix.e = e; s->matrix.f = f;
}

rpoint_t rmatrix_map(const rmatrix_t* m, rpoint_t p)
{
    rpoint_t r;
    r.x = p.x * m->a + p.y * m->c + m->e;
    r.y = p.x * m->b + p.y * m->d + m->f;
    return r;
}

int rcoverage_combine(int a, int b)
{
    return (a * b) / 255;
}

/* ------------------------------------------------------------------ */
/* Cubic algebra                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    double ax, bx, cx, dx; /* x(t) = ax t^3 + bx t^2 + cx t + dx */
    double ay, by, cy, dy;
} cubic_t;

static cubic_t cubic_make(const rpoint_t p[4])
{
    cubic_t q;
    q.dx = p[0].x;
    q.cx = 3.0 * (p[1].x - p[0].x);
    q.bx = 3.0 * (p[0].x - 2.0 * p[1].x + p[2].x);
    q.ax = -p[0].x + 3.0 * p[1].x - 3.0 * p[2].x + p[3].x;
    q.dy = p[0].y;
    q.cy = 3.0 * (p[1].y - p[0].y);
    q.by = 3.0 * (p[0].y - 2.0 * p[1].y + p[2].y);
    q.ay = -p[0].y + 3.0 * p[1].y - 3.0 * p[2].y + p[3].y;
    return q;
}

static double cubic_eval(const cubic_t* q, double t)
{
    return ((q->ax * t + q->bx) * t + q->cx) * t + q->dx;
}

static double cubic_evaly(const cubic_t* q, double t)
{
    return ((q->ay * t + q->by) * t + q->cy) * t + q->dy;
}

/* y-extrema parameters in (0,1), appended sorted and de-duplicated. */
static void cubic_y_extrema(const cubic_t* q, double ts[2], int* n)
{
    *n = 0;
    const double A = 3.0 * q->ay;
    const double B = 2.0 * q->by;
    const double C = q->cy;
    if(fabs(A) < R_EPS) {
        if(fabs(B) > R_EPS) {
            double t = -C / B;
            if(t > R_EPS && t < 1.0 - R_EPS) ts[(*n)++] = t;
        }
        return;
    }
    double disc = B * B - 4.0 * A * C;
    if(disc < 0) return;
    double sq = sqrt(disc);
    double t1 = (-B - sq) / (2.0 * A);
    double t2 = (-B + sq) / (2.0 * A);
    double buf[2];
    int k = 0;
    if(t1 > R_EPS && t1 < 1.0 - R_EPS) buf[k++] = t1;
    if(t2 > R_EPS && t2 < 1.0 - R_EPS && (k == 0 || fabs(t2 - buf[0]) > 1e-9)) buf[k++] = t2;
    /* sort */
    for(int i = 0; i < k; i++)
        for(int j = i + 1; j < k; j++)
            if(buf[j] < buf[i]) { double tmp = buf[i]; buf[i] = buf[j]; buf[j] = tmp; }
    for(int i = 0; i < k; i++) ts[(*n)++] = buf[i];
}

/* ------------------------------------------------------------------ */
/* Ray crossings                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    double x;
    int dir; /* +1 for increasing y, -1 for decreasing y */
} crossing_t;

typedef struct {
    crossing_t* data;
    int size;
    int capacity;
} crossing_list_t;

static void cl_push(crossing_list_t* cl, double x, int dir)
{
    if(cl->size == cl->capacity) {
        cl->capacity = cl->capacity ? cl->capacity * 2 : 16;
        cl->data = (crossing_t*)realloc(cl->data,
            cl->capacity * sizeof(crossing_t));
    }
    cl->data[cl->size].x = x;
    cl->data[cl->size].dir = dir;
    cl->size++;
}

static void cl_reset(crossing_list_t* cl) { cl->size = 0; }

static void line_crossings(const rpoint_t* a, const rpoint_t* b,
    double y, crossing_list_t* cl)
{
    int above_a = a->y > y;
    int above_b = b->y > y;
    if(above_a == above_b)
        return; /* half-open rule: on-vertex and tangency cancel naturally */
    double t = (y - a->y) / (b->y - a->y);
    double x = a->x + t * (b->x - a->x);
    cl_push(cl, x, b->y > a->y ? 1 : -1);
}

static void cubic_crossings(const cubic_t* q, double y, crossing_list_t* cl)
{
    double ext[2];
    int next;
    cubic_y_extrema(q, ext, &next);

    double bounds[4];
    int nb = 0;
    bounds[nb++] = 0.0;
    for(int i = 0; i < next; i++) bounds[nb++] = ext[i];
    bounds[nb++] = 1.0;

    for(int i = 0; i + 1 < nb; i++) {
        double ta = bounds[i], tb = bounds[i + 1];
        double ya = cubic_evaly(q, ta);
        double yb = cubic_evaly(q, tb);
        int above_a = ya > y;
        int above_b = yb > y;
        if(above_a == above_b)
            continue; /* tangent at an extremum row counts nothing */
        /* bisection on the y-monotone piece */
        double lo = ta, hi = tb;
        for(int it = 0; it < 80; it++) {
            double mid = 0.5 * (lo + hi);
            if((cubic_evaly(q, mid) > y) == above_a)
                lo = mid;
            else
                hi = mid;
        }
        double t = 0.5 * (lo + hi);
        cl_push(cl, cubic_eval(q, t), yb > ya ? 1 : -1);
    }
}

static int crossing_cmp(const void* pa, const void* pb)
{
    const crossing_t* a = (const crossing_t*)pa;
    const crossing_t* b = (const crossing_t*)pb;
    if(a->x < b->x) return -1;
    if(a->x > b->x) return 1;
    return 0;
}

/* Collects every crossing of the device-space shape at scanline y. */
static void shape_crossings(const rshape_t* s, double y, crossing_list_t* cl)
{
    cl_reset(cl);
    for(int ci = 0; ci < s->size; ci++) {
        const rcontour_t* c = &s->contours[ci];
        if(c->size == 0) continue;
        rpoint_t start = rmatrix_map(&s->matrix, c->data[0].p[0]);
        rpoint_t cur = start;
        for(int i = 1; i < c->size; i++) {
            const rseg_t* seg = &c->data[i];
            if(seg->type == RSEG_LINE) {
                rpoint_t a = cur;
                rpoint_t b = rmatrix_map(&s->matrix, seg->p[1]);
                line_crossings(&a, &b, y, cl);
                cur = b;
            } else {
                rpoint_t dp[4];
                dp[0] = cur;
                for(int k = 1; k < 4; k++)
                    dp[k] = rmatrix_map(&s->matrix, seg->p[k]);
                cubic_t q = cubic_make(dp);
                cubic_crossings(&q, y, cl);
                cur = dp[3];
            }
        }
        /* Fill always closes a sub-path implicitly, exactly as the FT
           decompose emits a closing line back to the move-to point. */
        {
            rpoint_t a = cur;
            rpoint_t b = start;
            line_crossings(&a, &b, y, cl);
        }
    }
    qsort(cl->data, cl->size, sizeof(crossing_t), crossing_cmp);
}

/* Winding and parity just to the right of probe x on scanline y. */
void rshape_ray_state(const rshape_t* s, double y, double x,
    int* winding, int* parity)
{
    crossing_list_t cl;
    cl.data = NULL; cl.size = cl.capacity = 0;
    shape_crossings(s, y, &cl);
    int w = 0, p = 0;
    for(int i = 0; i < cl.size; i++) {
        if(cl.data[i].x >= x) {
            w += cl.data[i].dir;
            p ^= 1;
        }
    }
    *winding = w;
    *parity = p;
    free(cl.data);
}

/* ------------------------------------------------------------------ */
/* Fill intervals and per-pixel area integration                       */
/* ------------------------------------------------------------------ */

typedef struct {
    double lo, hi;
} interval_t;

static int build_intervals(const rshape_t* s, const crossing_list_t* cl,
    interval_t* out, int maxout)
{
    int n = 0;
    int winding = 0;
    int parity = 0;
    double seg_start = 0;
    int inside = 0;
    int i = 0;
    while(i < cl->size) {
        int j = i;
        double x = cl->data[i].x;
        int dir_sum = 0;
        int transverse = 0;
        while(j < cl->size && cl->data[j].x == x) {
            dir_sum += cl->data[j].dir;
            transverse++;
            j++;
        }
        if(inside) {
            if(n < maxout) out[n].lo = seg_start, out[n].hi = x, n++;
            inside = 0;
        }
        winding += dir_sum;
        parity ^= (transverse & 1);
        int now_inside = s->rule == RRULE_EVEN_ODD ? parity : (winding != 0);
        if(now_inside) {
            seg_start = x;
            inside = 1;
        }
        i = j;
    }
    return n;
}

static double interval_pixel_measure(const interval_t* iv, int niv,
    double px)
{
    double m = 0;
    double lo = px, hi = px + 1.0;
    for(int i = 0; i < niv; i++) {
        double a = iv[i].lo > lo ? iv[i].lo : lo;
        double b = iv[i].hi < hi ? iv[i].hi : hi;
        if(b > a) m += b - a;
    }
    if(m < 0) m = 0;
    if(m > 1) m = 1;
    return m;
}

static double shape_coverage_row(const rshape_t* s,
    crossing_list_t* cl, interval_t* iv, int maxiv, int px, double y)
{
    shape_crossings(s, y, cl);
    int niv = build_intervals(s, cl, iv, maxiv);
    return interval_pixel_measure(iv, niv, (double)px);
}

void rshape_coverage(const rshape_t* s, int W, int H, int subrows,
    double band_width, rcov_t* grid)
{
    crossing_list_t cl;
    cl.data = NULL; cl.size = cl.capacity = 0;
    int maxiv = 64;
    interval_t* iv = (interval_t*)malloc(maxiv * sizeof(interval_t));

    for(int py = 0; py < H; py++) {
        for(int px = 0; px < W; px++) {
            double acc = 0;
            for(int j = 0; j < subrows; j++) {
                double y = (double)py + ((double)j + 0.5) / (double)subrows;
                acc += shape_coverage_row(s, &cl, iv, maxiv, px, y);
            }
            double exact = acc / (double)subrows;
            rcov_t* cell = &grid[py * W + px];
            cell->exact = exact;
            cell->lo = exact;
            cell->hi = exact;
            (void)band_width;
        }
    }
    free(iv);
    free(cl.data);
}

static int intervals_contain(const interval_t* iv, int niv, double x)
{
    for(int i = 0; i < niv; i++)
        if(x >= iv[i].lo && x < iv[i].hi)
            return 1;
    return 0;
}

typedef struct {
    double y;
    interval_t iv[64];
    int niv;
} row_cache_t;

static int double_cmp(const void* pa, const void* pb)
{
    double a = *(const double*)pa;
    double b = *(const double*)pb;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static const row_cache_t* row_cache_find(const row_cache_t* rows, int n,
    double y)
{
    int lo = 0, hi = n - 1;
    while(lo <= hi) {
        int mid = (lo + hi) / 2;
        if(fabs(rows[mid].y - y) < 1e-12)
            return &rows[mid];
        if(rows[mid].y < y) lo = mid + 1;
        else hi = mid - 1;
    }
    return NULL;
}

void rshape_coverage_banded(const rshape_t* s, int W, int H, int N,
    double band_width, rcov_t* grid, double* discretization)
{
    double h = 0.5 / (double)N;
    memset(grid, 0, (size_t)(W * H) * sizeof(rcov_t));

    /* Every probe row needed: sample centers plus the two band offsets. */
    int cap_rows = 3 * N * H + 4;
    double* ys = (double*)malloc((size_t)cap_rows * sizeof(double));
    int nys = 0;
    for(int py = 0; py < H; py++) {
        for(int iy = 0; iy < N; iy++) {
            double sy = (double)py + ((double)iy + 0.5) / (double)N;
            ys[nys++] = sy - band_width;
            ys[nys++] = sy;
            ys[nys++] = sy + band_width;
        }
    }
    qsort(ys, (size_t)nys, sizeof(double), double_cmp);

    row_cache_t* rows = (row_cache_t*)malloc(
        (size_t)nys * sizeof(row_cache_t));
    int nrows = 0;
    crossing_list_t cl;
    cl.data = NULL; cl.size = cl.capacity = 0;
    for(int i = 0; i < nys; i++) {
        if(i > 0 && fabs(ys[i] - ys[i - 1]) < 1e-12)
            continue;
        shape_crossings(s, ys[i], &cl);
        row_cache_t* rc = &rows[nrows++];
        rc->y = ys[i];
        rc->niv = build_intervals(s, &cl, rc->iv, 64);
    }
    free(ys);
    free(cl.data);

    for(int py = 0; py < H; py++) {
        for(int iy = 0; iy < N; iy++) {
            double sy = (double)py + ((double)iy + 0.5) / (double)N;
            const row_cache_t* r_lo = row_cache_find(rows, nrows,
                sy - band_width);
            const row_cache_t* r_mid = row_cache_find(rows, nrows, sy);
            const row_cache_t* r_hi = row_cache_find(rows, nrows,
                sy + band_width);
            for(int px = 0; px < W; px++) {
                rcov_t* cell = &grid[py * W + px];
                for(int ix = 0; ix < N; ix++) {
                    double sx = (double)px +
                        ((double)ix + 0.5) / (double)N;
                    int center = intervals_contain(r_mid->iv,
                        r_mid->niv, sx);
                    int in_lo = 1, in_hi = 0;
                    static const double sxs[4] = { -1, -1, 1, 1 };
                    static const int use_hi_row[4] = { 0, 1, 0, 1 };
                    for(int k = 0; k < 4; k++) {
                        double qx = sx + sxs[k] * band_width;
                        const row_cache_t* rc =
                            use_hi_row[k] ? r_hi : r_lo;
                        int inside = intervals_contain(rc->iv,
                            rc->niv, qx);
                        if(!inside) in_lo = 0;
                        if(inside) in_hi = 1;
                    }
                    if(center) cell->exact += 1.0;
                    if(in_lo) cell->lo += 1.0;
                    if(in_hi) cell->hi += 1.0;
                }
            }
        }
    }

    double scale = 1.0 / (double)(N * N);
    for(int i = 0; i < W * H; i++) {
        grid[i].exact *= scale;
        grid[i].lo *= scale;
        grid[i].hi *= scale;
    }
    if(discretization) *discretization = h;
    free(rows);
}

/* ------------------------------------------------------------------ */
/* Forward-derived curve error band                                    */
/* ------------------------------------------------------------------ */

static long ft_hypot(long x, long y)
{
    x = x < 0 ? -x : x;
    y = y < 0 ? -y : y;
    return x > y ? x + (3 * y >> 3) : y + (3 * x >> 3);
}

#define FT_ONE_PIXEL 256L

/* Replays gray_render_cubic's acceptance test on 28.8 integer points. */
static int ft_cubic_accepted(long arc[8])
{
    long dx = arc[6] - arc[0];
    long dy = arc[7] - arc[1];
    long L = ft_hypot(dx, dy);
    if(L >= (1L << 23))
        return 0;
    long s_limit = L * (FT_ONE_PIXEL / 6);

    long dx1 = arc[2] - arc[0];
    long dy1 = arc[3] - arc[1];
    long s = dy * dx1 - dx * dy1;
    if(s < 0) s = -s;
    if(s > s_limit) return 0;

    long dx2 = arc[4] - arc[0];
    long dy2 = arc[5] - arc[1];
    s = dy * dx2 - dx * dy2;
    if(s < 0) s = -s;
    if(s > s_limit) return 0;

    if(dx1 * (dx1 - dx) + dy1 * (dy1 - dy) > 0 ||
        dx2 * (dx2 - dx) + dy2 * (dy2 - dy) > 0)
        return 0;
    return 1;
}

static void ft_split_cubic(long arc[8])
{
    long x[4] = { arc[0], arc[2], arc[4], arc[6] };
    long y[4] = { arc[1], arc[3], arc[5], arc[7] };
    long a, b, c;
    /* x */
    long nx[7], ny[7];
    nx[6] = x[3];
    c = x[1]; b = x[2];
    a = (x[0] + c) / 2; nx[1] = a;
    b = (x[3] + b) / 2; nx[5] = b;
    c = (c + x[2]) / 2;
    a = (a + c) / 2; nx[2] = a;
    b = (b + c) / 2; nx[4] = b;
    nx[3] = (a + b) / 2;
    nx[0] = x[0];
    /* y */
    ny[6] = y[3];
    c = y[1]; b = y[2];
    a = (y[0] + c) / 2; ny[1] = a;
    b = (y[3] + b) / 2; ny[5] = b;
    c = (c + y[2]) / 2;
    a = (a + c) / 2; ny[2] = a;
    b = (b + c) / 2; ny[4] = b;
    ny[3] = (a + b) / 2;
    ny[0] = y[0];
    for(int i = 0; i < 7; i++) {
        arc[2 * i] = nx[i];
        arc[2 * i + 1] = ny[i];
    }
}

typedef struct {
    long* x;
    long* y;
    int size;
    int capacity;
} poly_t;

static void poly_push(poly_t* p, long x, long y)
{
    if(p->size == p->capacity) {
        p->capacity = p->capacity ? p->capacity * 2 : 32;
        p->x = (long*)realloc(p->x, p->capacity * sizeof(long));
        p->y = (long*)realloc(p->y, p->capacity * sizeof(long));
    }
    p->x[p->size] = x;
    p->y[p->size] = y;
    p->size++;
}

static void flatten_integer(long arc[8], int depth, poly_t* poly)
{
    if(ft_cubic_accepted(arc) || depth >= 15) {
        poly_push(poly, arc[6], arc[7]);
        return;
    }
    long left[8], right[8];
    memcpy(left, arc, sizeof(left));
    ft_split_cubic(left);
    /* after split, left occupies indices 0..3 (points 0,1,2,3) */
    for(int i = 0; i < 4; i++) {
        right[2 * i] = left[2 * (i + 3)];
        right[2 * i + 1] = left[2 * (i + 3) + 1];
    }
    flatten_integer(left, depth + 1, poly);
    flatten_integer(right, depth + 1, poly);
}

static double linfty_point_segment(double px, double py,
    double ax, double ay, double bx, double by)
{
    double best = 1e300;
    const int M = 64;
    for(int i = 0; i <= M; i++) {
        double t = (double)i / (double)M;
        double sx = ax + t * (bx - ax);
        double sy = ay + t * (by - ay);
        double dx = fabs(px - sx), dy = fabs(py - sy);
        double d = dx > dy ? dx : dy;
        if(d < best) best = d;
    }
    return best;
}

static double cubic_band_one(const rshape_t* s, const rpoint_t dp[4])
{
    rpoint_t q[4];
    for(int k = 0; k < 4; k++)
        q[k] = rmatrix_map(&s->matrix, dp[k]);

    /* product quantization: roundf(x*64) in 26.6, upscaled x4 to 28.8 */
    long arc[8];
    for(int k = 0; k < 4; k++) {
        long qx = (long)lround(q[k].x * 64.0) * 4;
        long qy = (long)lround(q[k].y * 64.0) * 4;
        arc[2 * k] = qx;
        arc[2 * k + 1] = qy;
    }

    poly_t poly;
    poly.x = poly.y = NULL;
    poly.size = poly.capacity = 0;
    poly_push(&poly, arc[0], arc[1]);
    flatten_integer(arc, 0, &poly);

    cubic_t true_curve = cubic_make(q);
    double worst = 0;
    const int SAMPLES = 2048;
    for(int i = 0; i <= SAMPLES; i++) {
        double t = (double)i / (double)SAMPLES;
        double tx = cubic_eval(&true_curve, t);
        double ty = cubic_evaly(&true_curve, t);
        double best = 1e300;
        for(int j = 0; j + 1 < poly.size; j++) {
            double d = linfty_point_segment(tx, ty,
                poly.x[j] / 256.0, poly.y[j] / 256.0,
                poly.x[j + 1] / 256.0, poly.y[j + 1] / 256.0);
            if(d < best) best = d;
        }
        if(best > worst) worst = best;
    }
    free(poly.x);
    free(poly.y);
    /* The measured distance already covers chord flattening, integer
       subdivision truncation and the 26.6 grid rounding of every on/off
       curve point.  The additive terms are conservative slack: 1/128 px is
       the L-infinity bound of roundf(x*64) grid quantization, and 1/2048 px
       bounds the dense t-sampling residual of this measurement. */
    return worst + 1.0 / 128.0 + 1.0 / 2048.0;
}

double rshape_curve_band(const rshape_t* s)
{
    double worst = 0;
    for(int ci = 0; ci < s->size; ci++) {
        const rcontour_t* c = &s->contours[ci];
        rpoint_t cur = c->data[0].p[0];
        for(int i = 1; i < c->size; i++) {
            const rseg_t* seg = &c->data[i];
            if(seg->type == RSEG_CUBIC) {
                rpoint_t dp[4];
                dp[0] = cur;
                dp[1] = seg->p[1]; dp[2] = seg->p[2]; dp[3] = seg->p[3];
                double b = cubic_band_one(s, dp);
                if(b > worst) worst = b;
                cur = seg->p[3];
            } else {
                cur = seg->p[1];
            }
        }
    }
    return worst;
}
