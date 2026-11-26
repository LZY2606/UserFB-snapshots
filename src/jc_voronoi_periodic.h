// jc_voronoi_periodic.h
//
// Periodic (toroidal) Voronoi diagrams built on top of jc_voronoi.
//
// Usage:
//   #define JC_VORONOI_PERIODIC_IMPLEMENTATION
//   #include "jc_voronoi_periodic.h"
//
// Define JC_VORONOI_IMPLEMENTATION as well only if you want to control where
// the original jc_voronoi implementation is instantiated.  With just
// JC_VORONOI_PERIODIC_IMPLEMENTATION, this header also instantiates the
// original implementation in the same translation unit.

#ifndef JC_VORONOI_PERIODIC_H
#define JC_VORONOI_PERIODIC_H

#if defined(JC_VORONOI_PERIODIC_IMPLEMENTATION) && \
    !defined(JC_VORONOI_IMPLEMENTATION_DONE) && \
    !defined(JC_VORONOI_IMPLEMENTATION)
#define JC_VORONOI_IMPLEMENTATION
#endif

#include "jc_voronoi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jcv_periodic_edge
{
    jcv_point pos[2];
    int neighbor;
    int shift_x;
    int shift_y;
} jcv_periodic_edge;

typedef struct jcv_periodic_cell
{
    jcv_point point;
    jcv_point centroid;
    jcv_real area;
    int first_edge;
    int num_edges;
} jcv_periodic_cell;

typedef struct jcv_periodic_diagram
{
    jcv_rect rect;
    int numsites;
    int numedges;
    jcv_periodic_cell* cells;
    jcv_periodic_edge* edges;
} jcv_periodic_diagram;

// Generates a periodic Voronoi diagram.  Returns 1 on success and 0 if the
// input is invalid or allocation fails.  out must be zero-initialized.
int jcv_periodic_generate(int n, const jcv_point* points,
                          const jcv_rect* rect,
                          jcv_periodic_diagram* out);

// Releases arrays owned by diagram and clears the struct.  Safe on NULL and
// safe to call repeatedly.
void jcv_periodic_free(jcv_periodic_diagram* diagram);

// Simultaneously relaxes all seeds: out[i] = point + alpha*(centroid-point),
// followed by wrapping into the fundamental rectangle.  alpha must be finite
// and in [0,1].  Returns 1 on success; returns 0 without writing out_points
// for invalid arguments.
int jcv_periodic_relax(const jcv_periodic_diagram* diagram,
                       jcv_real alpha, jcv_point* out_points);

#ifdef __cplusplus
}
#endif

#endif // JC_VORONOI_PERIODIC_H

#ifdef JC_VORONOI_PERIODIC_IMPLEMENTATION
#ifndef JC_VORONOI_PERIODIC_IMPLEMENTATION_DONE
#define JC_VORONOI_PERIODIC_IMPLEMENTATION_DONE

/* Handle a first declarations-only include followed by the implementation
   macro and a second include of this header. */
#if !defined(JC_VORONOI_IMPLEMENTATION_DONE)
#define JC_VORONOI_IMPLEMENTATION
#include "jc_voronoi.h"
#endif

#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct jcvp_copy_info_
{
    int original;
    int shift_x;
    int shift_y;
} jcvp_copy_info;

static int jcvp_finite_real(jcv_real v)
{
    return isfinite((double)v) ? 1 : 0;
}

static jcv_real jcvp_wrap(jcv_real displacement, jcv_real period)
{
    jcv_real k = jcv_floor(displacement / period);
    jcv_real r = displacement - k * period;

    // Defensive normalization against floating-point boundary rounding.
    while (r < (jcv_real)0)
        r += period;
    while (r >= period)
        r -= period;
    return r;
}

// Addition of a wrapped local coordinate to a large rect origin can round to
// rect.max.  Half-open output contracts require the representable predecessor.
static jcv_real jcvp_half_open(jcv_real value, const jcv_rect* rect, int x_axis)
{
    jcv_real minimum = x_axis ? rect->min.x : rect->min.y;
    jcv_real maximum = x_axis ? rect->max.x : rect->max.y;
    if (value >= maximum)
    {
        value = (jcv_real)nextafter((double)maximum, (double)minimum);
        if (value < minimum)
            value = minimum;
    }
    return value;
}

static jcv_real jcvp_len2(const jcv_point* a, const jcv_point* b)
{
    jcv_real dx = a->x - b->x;
    jcv_real dy = a->y - b->y;
    return dx * dx + dy * dy;
}

// Cocircular periodic configurations can produce edges whose endpoints differ
// only by a few ulps.  They are zero-length degeneracies for all geometric
// purposes here.  The threshold is still far below the 2e-7 test scale.
static int jcvp_is_degenerate(const jcv_point* a, const jcv_point* b,
                              jcv_real scale)
{
    jcv_real tol = (jcv_real)64.0 * (jcv_real)JCV_REAL_TYPE_EPSILON * scale;
    if (!(tol > (jcv_real)0))
        tol = (jcv_real)1e-14;
    return jcvp_len2(a, b) <= tol * tol;
}

static void jcvp_clear_output(jcv_periodic_diagram* d)
{
    if (!d)
        return;
    free(d->cells);
    free(d->edges);
    memset(d, 0, sizeof(*d));
}

int jcv_periodic_generate(int n, const jcv_point* points,
                          const jcv_rect* rect,
                          jcv_periodic_diagram* out)
{
    enum { COPIES_PER_AXIS = 3 };
    const int copy_count_factor = COPIES_PER_AXIS * COPIES_PER_AXIS;

    jcv_point* local_points = 0;
    jcv_point* copy_points = 0;
    jcvp_copy_info* info = 0;
    const jcv_site** central_sites = 0;
    int* edge_counts = 0;
    jcv_periodic_edge* edges = 0;
    jcv_periodic_cell* cells = 0;
    jcv_diagram gen;
    int generated = 0;
    int result = 0;
    int total_edges = 0;
    int copy_count = 0;
    int k = 0;
    int i = 0;

    if (!out)
        return 0;

    // The documented input state is all-zero.  Clearing here also gives the
    // requested all-zero guarantee after rejected calls.
    memset(out, 0, sizeof(*out));

    if (!points || !rect)
        return 0;
    if (n < 1 || n > 256)
        return 0;

    if (!jcvp_finite_real(rect->min.x) || !jcvp_finite_real(rect->min.y) ||
        !jcvp_finite_real(rect->max.x) || !jcvp_finite_real(rect->max.y))
        return 0;

    {
        jcv_real W = rect->max.x - rect->min.x;
        jcv_real H = rect->max.y - rect->min.y;
        jcv_real eps_x = (jcv_real)128.0 * (jcv_real)JCV_REAL_TYPE_EPSILON *
            ((jcv_real)1.0 + jcv_abs(rect->min.x) + jcv_abs(rect->max.x));
        jcv_real eps_y = (jcv_real)128.0 * (jcv_real)JCV_REAL_TYPE_EPSILON *
            ((jcv_real)1.0 + jcv_abs(rect->min.y) + jcv_abs(rect->max.y));
        jcv_real short_side;
        jcv_real long_side;

        if (!jcvp_finite_real(W) || !jcvp_finite_real(H))
            return 0;
        if (jcv_abs(rect->min.x) > (jcv_real)1000.0 ||
            jcv_abs(rect->min.y) > (jcv_real)1000.0)
            return 0;
        /* Endpoint subtraction near coordinate magnitude 1000 can move a
           nominal 0.01 side by a few ulps below the decimal literal. */
        if (W < (jcv_real)0.01 - eps_x || W > (jcv_real)100.0 + eps_x ||
            H < (jcv_real)0.01 - eps_y || H > (jcv_real)100.0 + eps_y)
            return 0;

        short_side = W < H ? W : H;
        long_side = W > H ? W : H;
        /* Decimal rect endpoints can make a nominal 16:1 limit round over. */
        if (long_side > (jcv_real)16.0 * short_side *
                            ((jcv_real)1.0 + (jcv_real)1e-12))
            return 0;

        for (i = 0; i < n; ++i)
        {
            jcv_real dx, dy;
            if (!jcvp_finite_real(points[i].x) ||
                !jcvp_finite_real(points[i].y))
                return 0;

            dx = points[i].x - rect->min.x;
            dy = points[i].y - rect->min.y;
            /* Absorb one ulp when a boundary point is formed as min+10*W. */
            if (jcv_abs(dx) > (jcv_real)10.0 * W + (jcv_real)1e-10 * W ||
                jcv_abs(dy) > (jcv_real)10.0 * H + (jcv_real)1e-10 * H)
                return 0;
        }

        local_points = (jcv_point*)calloc((size_t)n, sizeof(jcv_point));
        if (!local_points)
            return 0;

        for (i = 0; i < n; ++i)
        {
            jcv_real dx = points[i].x - rect->min.x;
            jcv_real dy = points[i].y - rect->min.y;
            local_points[i].x = jcvp_wrap(dx, W);
            local_points[i].y = jcvp_wrap(dy, H);
        }

        // Periodic coincidence test using the shortest torus displacement.
        for (i = 0; i < n; ++i)
        {
            int j;
            for (j = i + 1; j < n; ++j)
            {
                jcv_real dx = jcv_abs(local_points[i].x - local_points[j].x);
                jcv_real dy = jcv_abs(local_points[i].y - local_points[j].y);
                jcv_real d;
                jcv_real threshold;

                dx = dx < W - dx ? dx : W - dx;
                dy = dy < H - dy ? dy : H - dy;
                d = (jcv_real)sqrt((double)(dx * dx + dy * dy));
                threshold = (jcv_real)1e-10 * short_side;
                /* A few ulps of slack for decimal endpoint construction. */
                if (d <= threshold * ((jcv_real)1.0 + (jcv_real)1e-12))
                    goto cleanup;
            }
        }

        copy_count = n * copy_count_factor;
        copy_points = (jcv_point*)calloc((size_t)copy_count, sizeof(jcv_point));
        info = (jcvp_copy_info*)calloc((size_t)copy_count, sizeof(jcvp_copy_info));
        central_sites = (const jcv_site**)calloc((size_t)n, sizeof(jcv_site*));
        edge_counts = (int*)calloc((size_t)n, sizeof(int));
        if (!copy_points || !info || !central_sites || !edge_counts)
            goto cleanup;

        // Three copies in each direction are enough: a shift +/-2 copy is
        // always dominated by the intervening +/-1 copy.  The generation
        // rectangle has another period of margin around those copies.
        k = 0;
        {
            int sy, sx, ii;
            for (sy = -1; sy <= 1; ++sy)
            {
                for (sx = -1; sx <= 1; ++sx)
                {
                    for (ii = 0; ii < n; ++ii)
                    {
                        copy_points[k].x =
                            local_points[ii].x + (jcv_real)sx * W;
                        copy_points[k].y =
                            local_points[ii].y + (jcv_real)sy * H;
                        info[k].original = ii;
                        info[k].shift_x = sx;
                        info[k].shift_y = sy;
                        ++k;
                    }
                }
            }
        }

        memset(&gen, 0, sizeof(gen));
        {
            jcv_rect generation_rect;
            generation_rect.min.x = (jcv_real)-2.0 * W;
            generation_rect.min.y = (jcv_real)-2.0 * H;
            generation_rect.max.x = (jcv_real)3.0 * W;
            generation_rect.max.y = (jcv_real)3.0 * H;

            jcv_diagram_generate(copy_count, copy_points,
                                 &generation_rect, 0, &gen);
            generated = 1;
        }

        {
            const jcv_site* sites = jcv_diagram_get_sites(&gen);
            int s;
            if (!sites || gen.numsites <= 0)
                goto cleanup;

            for (s = 0; s < gen.numsites; ++s)
            {
                unsigned int copy_index = sites[s].index;
                int original;
                if (copy_index >= (unsigned int)copy_count)
                    goto cleanup;
                original = info[copy_index].original;
                if (info[copy_index].shift_x == 0 &&
                    info[copy_index].shift_y == 0)
                    central_sites[original] = &sites[s];
            }
        }

        for (i = 0; i < n; ++i)
        {
            jcv_edge_iter it;
            jcv_edge e;
            jcv_real scale = long_side;

            if (!central_sites[i])
                goto cleanup;

            jcv_site_get_edges(&gen, central_sites[i], &it);
            while (jcv_edge_next(&it, &e))
            {
                if (!e.sites[1])
                    goto cleanup; // A central periodic cell cannot hit a wall.
                if (jcvp_is_degenerate(&e.pos[0], &e.pos[1], scale))
                    continue;
                ++edge_counts[i];
                ++total_edges;
            }
            if (edge_counts[i] < 3)
                goto cleanup;
        }

        cells = (jcv_periodic_cell*)calloc((size_t)n, sizeof(*cells));
        edges = (jcv_periodic_edge*)calloc((size_t)total_edges > 0 ?
                                           (size_t)total_edges : 1u,
                                           sizeof(*edges));
        if (!cells || !edges)
            goto cleanup;

        out->cells = cells;
        out->edges = edges;
        out->numsites = n;
        out->numedges = total_edges;
        out->rect = *rect;

        k = 0;
        for (i = 0; i < n; ++i)
        {
            jcv_edge_iter it;
            jcv_edge e;
            jcv_real scale = long_side;

            cells[i].point.x = jcvp_half_open(
                rect->min.x + local_points[i].x, rect, 1);
            cells[i].point.y = jcvp_half_open(
                rect->min.y + local_points[i].y, rect, 0);
            cells[i].first_edge = k;
            cells[i].num_edges = edge_counts[i];

            jcv_site_get_edges(&gen, central_sites[i], &it);
            while (jcv_edge_next(&it, &e))
            {
                const jcv_site* neighbor;
                unsigned int copy_index;
                int ci;

                if (!e.sites[1])
                    goto cleanup;
                if (jcvp_is_degenerate(&e.pos[0], &e.pos[1], scale))
                    continue;

                neighbor = e.sites[1];
                copy_index = neighbor->index;
                if (copy_index >= (unsigned int)copy_count)
                    goto cleanup;
                ci = (int)copy_index;

                edges[k].pos[0] = e.pos[0];
                edges[k].pos[1] = e.pos[1];
                edges[k].neighbor = info[ci].original;
                edges[k].shift_x = info[ci].shift_x;
                edges[k].shift_y = info[ci].shift_y;
                ++k;
            }

            if (k != cells[i].first_edge + cells[i].num_edges)
                goto cleanup;
        }

        // Make ring closure exact despite a few ulps between independently
        // stored incident endpoints.  V[k] is shared by edge k-1 and edge k.
        for (i = 0; i < n; ++i)
        {
            int start = cells[i].first_edge;
            int m = cells[i].num_edges;
            int j;

            for (j = 0; j < m; ++j)
            {
                const jcv_periodic_edge* prev =
                    &edges[start + ((j + m - 1) % m)];
                jcv_periodic_edge* cur = &edges[start + j];
                jcv_point shared;
                shared.x = (prev->pos[1].x + cur->pos[0].x) * (jcv_real)0.5;
                shared.y = (prev->pos[1].y + cur->pos[0].y) * (jcv_real)0.5;
                cur->pos[0] = shared;
            }
            for (j = 0; j < m; ++j)
            {
                jcv_periodic_edge* cur = &edges[start + j];
                const jcv_periodic_edge* next =
                    &edges[start + ((j + 1) % m)];
                cur->pos[1] = next->pos[0];
            }
        }

        // Polygon moments in local coordinates, then translate results back.
        for (i = 0; i < n; ++i)
        {
            int start = cells[i].first_edge;
            int m = cells[i].num_edges;
            jcv_real area2 = 0;
            jcv_real cx6 = 0;
            jcv_real cy6 = 0;
            jcv_real signed_area;
            int j;

            for (j = 0; j < m; ++j)
            {
                const jcv_point* a = &edges[start + j].pos[0];
                const jcv_point* b = &edges[start + j].pos[1];
                jcv_real cross = a->x * b->y - b->x * a->y;
                area2 += cross;
                cx6 += (a->x + b->x) * cross;
                cy6 += (a->y + b->y) * cross;
            }

            signed_area = area2 * (jcv_real)0.5;
            if (!(signed_area > (jcv_real)0))
                goto cleanup;

            cells[i].area = signed_area;
            cells[i].centroid.x =
                rect->min.x + cx6 / ((jcv_real)3.0 * area2);
            cells[i].centroid.y =
                rect->min.y + cy6 / ((jcv_real)3.0 * area2);

            for (j = 0; j < m; ++j)
            {
                edges[start + j].pos[0].x += rect->min.x;
                edges[start + j].pos[0].y += rect->min.y;
                edges[start + j].pos[1].x += rect->min.x;
                edges[start + j].pos[1].y += rect->min.y;
            }
        }

        result = 1;
    }

cleanup:
    if (generated)
        jcv_diagram_free(&gen);
    free(local_points);
    free(copy_points);
    free(info);
    free((void*)central_sites);
    free(edge_counts);

    if (!result)
    {
        // cells/edges are either null or owned partial allocations.
        out->cells = cells;
        out->edges = edges;
        jcvp_clear_output(out);
    }
    return result;
}

void jcv_periodic_free(jcv_periodic_diagram* diagram)
{
    if (!diagram)
        return;
    jcvp_clear_output(diagram);
}

int jcv_periodic_relax(const jcv_periodic_diagram* diagram,
                       jcv_real alpha, jcv_point* out_points)
{
    int i;
    jcv_real W, H;

    if (!diagram || !out_points)
        return 0;
    if (!jcvp_finite_real(alpha) || alpha < (jcv_real)0 ||
        alpha > (jcv_real)1)
        return 0;
    if (diagram->numsites < 1 || diagram->numsites > 256 ||
        !diagram->cells)
        return 0;

    W = diagram->rect.max.x - diagram->rect.min.x;
    H = diagram->rect.max.y - diagram->rect.min.y;
    if (!jcvp_finite_real(W) || !jcvp_finite_real(H) ||
        W <= (jcv_real)0 || H <= (jcv_real)0)
        return 0;

    for (i = 0; i < diagram->numsites; ++i)
    {
        const jcv_periodic_cell* c = &diagram->cells[i];
        jcv_real x, y;

        if (alpha == (jcv_real)0)
        {
            out_points[i] = c->point;
            continue;
        }

        x = c->point.x + alpha * (c->centroid.x - c->point.x);
        y = c->point.y + alpha * (c->centroid.y - c->point.y);

        // Avoid a needless modulo round-trip for coordinates already in the
        // half-open fundamental rectangle.
        if (x < diagram->rect.min.x || x >= diagram->rect.max.x)
            x = diagram->rect.min.x +
                jcvp_wrap(x - diagram->rect.min.x, W);
        if (y < diagram->rect.min.y || y >= diagram->rect.max.y)
            y = diagram->rect.min.y +
                jcvp_wrap(y - diagram->rect.min.y, H);

        out_points[i].x = jcvp_half_open(x, &diagram->rect, 1);
        out_points[i].y = jcvp_half_open(y, &diagram->rect, 0);
    }
    return 1;
}

#endif // JC_VORONOI_PERIODIC_IMPLEMENTATION_DONE
#endif // JC_VORONOI_PERIODIC_IMPLEMENTATION
