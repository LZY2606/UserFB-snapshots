// Periodic (toroidal) Voronoi extension for jc_voronoi.
//
// The basic rectangle of the input jcv_rect is interpreted as a flat torus of
// width W and height H.  Every input seed is lifted to a rectangular lattice of
// replicas, the ordinary sweep-line generator in jc_voronoi.h is run once over
// those replicas, and each seed's full Voronoi polygon is extracted from the
// central replica.  Polygons are therefore returned unfolded in the plane: they
// are never cut or wrapped into the basic rectangle.
//
// Cells keep the input order.  Edges form a counter-clockwise closed ring per
// cell, and every edge records the original input index of its neighbour plus
// the integer lattice shift of the neighbour's copy:
//
//     neighbour_replica = cells[edge.neighbor].point + (shift_x * W, shift_y * H)
//
// Consequently a neighbour may be the cell itself, and the same neighbour may
// occur several times with different shifts.
//
// License: MIT, same as jc_voronoi.h

#ifndef JC_VORONOI_PERIODIC_H
#define JC_VORONOI_PERIODIC_H

#include "jc_voronoi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jcv_periodic_edge_
{
    jcv_point pos[2];
    int       neighbor; // Index into the original input points
    int       shift_x;  // Neighbour copy shift, in periods
    int       shift_y;
} jcv_periodic_edge;

typedef struct jcv_periodic_cell_
{
    jcv_point point;    // Seed, normalized into the half-open basic rectangle
    jcv_point centroid; // Area centroid of the unfolded polygon
    jcv_real  area;
    int       first_edge;
    int       num_edges;
} jcv_periodic_cell;

typedef struct jcv_periodic_diagram_
{
    jcv_rect             rect;
    int                  numsites;
    int                  numedges;
    jcv_periodic_cell*   cells;
    jcv_periodic_edge*   edges;
} jcv_periodic_diagram;

/**
 * Generates a periodic Voronoi diagram.
 *
 * On success returns 1 and fills `out`, which then owns its arrays until
 * jcv_periodic_free is called.  On rejected input returns 0 and leaves `out`
 * zeroed.  `rect` and `points` are never modified.
 */
extern int jcv_periodic_generate( int n, const jcv_point* points, const jcv_rect* rect, jcv_periodic_diagram* out );

/** Releases the arrays and zeroes the diagram.  NULL and repeated calls are safe. */
extern void jcv_periodic_free( jcv_periodic_diagram* diagram );

/**
 * One simultaneous Lloyd step: writes point + alpha * (centroid - point) for
 * every seed into the caller-owned array, then normalizes each result into the
 * basic rectangle.  alpha must be a finite value in [0, 1].
 *
 * Returns 0 without touching `out_points` for invalid alpha or NULL arguments.
 */
extern int jcv_periodic_relax( const jcv_periodic_diagram* diagram, jcv_real alpha, jcv_point* out_points );

#ifdef __cplusplus
}
#endif

#endif // JC_VORONOI_PERIODIC_H

#ifdef JC_VORONOI_PERIODIC_IMPLEMENTATION
#undef JC_VORONOI_PERIODIC_IMPLEMENTATION

#include <math.h>
#include <stdlib.h>
#include <string.h>

// The implementation below relies on jcv_diagram_generate from jc_voronoi.h.
// Define JC_VORONOI_IMPLEMENTATION in the same translation unit (or link the
// ordinary jc_voronoi implementation separately).

#define JCV_PERIODIC_MAX_SITES 256

typedef struct jcvp_replica_
{
    int original; // Index in the caller's point array
    int shift_x;
    int shift_y;
} jcvp_replica;

static int jcvp_is_finite(double v)
{
    return isfinite(v) ? 1 : 0;
}

static double jcvp_abs(double v)
{
    return v < 0 ? -v : v;
}

static double jcvp_wrapped_offset(double value, double origin, double period)
{
    double ratio = (value - origin) / period;
    double base = floor(ratio);
    double offset = (ratio - base) * period;
    // Guard against the ratio rounding up to the next period.
    if( offset < 0 )
        offset = 0;
    if( offset >= period )
        offset = 0;
    return offset;
}

static double jcvp_periodic_dist_sq(double ax, double ay, double bx, double by,
                                    double w, double h)
{
    double dx = jcvp_abs(ax - bx);
    double dy = jcvp_abs(ay - by);
    if( dx > w * 0.5 )
        dx = w - dx;
    if( dy > h * 0.5 )
        dy = h - dy;
    return dx * dx + dy * dy;
}

static void jcvp_zero_diagram(jcv_periodic_diagram* d)
{
    memset(d, 0, sizeof(*d));
}

// The sweep can emit a duplicate vertex pair separated by a few ulps when two
// circle events resolve to the same corner.  Such slivers carry no adjacency
// and must not survive as fake edges.  The threshold is far below any length
// that matters for the requested geometric tolerance.
static int jcvp_edge_is_degenerate(const jcv_edge* edge, double max_period)
{
    double dx = (double)edge->pos[1].x - (double)edge->pos[0].x;
    double dy = (double)edge->pos[1].y - (double)edge->pos[0].y;
    double threshold = 1.0e-10 * max_period;
    return (dx * dx + dy * dy) <= threshold * threshold;
}

int jcv_periodic_generate( int n, const jcv_point* points, const jcv_rect* rect, jcv_periodic_diagram* out )
{
    if( !out )
        return 0;
    jcvp_zero_diagram(out);

    if( n < 1 || n > JCV_PERIODIC_MAX_SITES || !points || !rect )
        return 0;

    if( !jcvp_is_finite((double)rect->min.x) || !jcvp_is_finite((double)rect->min.y) ||
        !jcvp_is_finite((double)rect->max.x) || !jcvp_is_finite((double)rect->max.y) )
        return 0;

    if( jcvp_abs((double)rect->min.x) > 1000.0 || jcvp_abs((double)rect->min.y) > 1000.0 )
        return 0;

    double w = (double)rect->max.x - (double)rect->min.x;
    double h = (double)rect->max.y - (double)rect->min.y;
    if( !(w >= 0.01 && w <= 100.0) || !(h >= 0.01 && h <= 100.0) )
        return 0;
    if( w > 16.0 * h || h > 16.0 * w )
        return 0;

    double min_period = w < h ? w : h;
    double max_period = w > h ? w : h;

    double offset_x[JCV_PERIODIC_MAX_SITES];
    double offset_y[JCV_PERIODIC_MAX_SITES];
    jcv_point base[JCV_PERIODIC_MAX_SITES];

    for( int i = 0; i < n; ++i )
    {
        double px = (double)points[i].x;
        double py = (double)points[i].y;
        if( !jcvp_is_finite(px) || !jcvp_is_finite(py) )
            return 0;
        if( jcvp_abs(px - (double)rect->min.x) > 10.0 * w ||
            jcvp_abs(py - (double)rect->min.y) > 10.0 * h )
            return 0;

        double ox = jcvp_wrapped_offset(px, (double)rect->min.x, w);
        double oy = jcvp_wrapped_offset(py, (double)rect->min.y, h);
        offset_x[i] = ox;
        offset_y[i] = oy;
        base[i].x = (jcv_real)((double)rect->min.x + ox);
        base[i].y = (jcv_real)((double)rect->min.y + oy);
    }

    // Reject seeds that coincide on the torus.
    double coincide = 1.0e-10 * min_period;
    for( int i = 0; i < n; ++i )
    {
        for( int j = i + 1; j < n; ++j )
        {
            double d2 = jcvp_periodic_dist_sq(offset_x[i], offset_y[i],
                                              offset_x[j], offset_y[j], w, h);
            if( d2 <= coincide * coincide )
                return 0;
        }
    }

    // A cell is inside the disk of radius rho around its seed, and a site that
    // contributes to one of its edges is at most 2*rho away.  Include a full
    // extra period of margin so the ordinary bounding-box clipper never trims a
    // central cell.
    double rho = 0.5 * sqrt(w * w + h * h);
    int kx = (int)ceil(2.0 * rho / w) + 1;
    int ky = (int)ceil(2.0 * rho / h) + 1;
    int nx = 2 * kx + 1;
    int ny = 2 * ky + 1;
    int replica_count = n * nx * ny;

    jcvp_replica* replicas = (jcvp_replica*)malloc(sizeof(jcvp_replica) * (size_t)replica_count);
    jcv_point* replica_points = (jcv_point*)malloc(sizeof(jcv_point) * (size_t)replica_count);
    if( !replicas || !replica_points )
    {
        free(replicas);
        free(replica_points);
        return 0;
    }

    double bbox_min_x = 0, bbox_min_y = 0, bbox_max_x = 0, bbox_max_y = 0;
    int replica_index = 0;
    for( int s = 0; s < n; ++s )
    {
        for( int i = -kx; i <= kx; ++i )
        {
            for( int j = -ky; j <= ky; ++j )
            {
                double x = (double)base[s].x + (double)i * w;
                double y = (double)base[s].y + (double)j * h;
                replicas[replica_index].original = s;
                replicas[replica_index].shift_x = i;
                replicas[replica_index].shift_y = j;
                replica_points[replica_index].x = (jcv_real)x;
                replica_points[replica_index].y = (jcv_real)y;
                if( replica_index == 0 )
                {
                    bbox_min_x = bbox_max_x = x;
                    bbox_min_y = bbox_max_y = y;
                }
                else
                {
                    if( x < bbox_min_x ) bbox_min_x = x;
                    if( x > bbox_max_x ) bbox_max_x = x;
                    if( y < bbox_min_y ) bbox_min_y = y;
                    if( y > bbox_max_y ) bbox_max_y = y;
                }
                ++replica_index;
            }
        }
    }

    jcv_rect bbox;
    bbox.min.x = (jcv_real)bbox_min_x;
    bbox.min.y = (jcv_real)bbox_min_y;
    bbox.max.x = (jcv_real)bbox_max_x;
    bbox.max.y = (jcv_real)bbox_max_y;

    jcv_diagram diagram;
    memset(&diagram, 0, sizeof(diagram));
    jcv_diagram_generate(replica_count, replica_points, &bbox, 0, &diagram);

    int* site_of_replica = (int*)malloc(sizeof(int) * (size_t)replica_count);
    int* central_site = (int*)malloc(sizeof(int) * (size_t)n);
    jcv_periodic_cell* cells = (jcv_periodic_cell*)calloc((size_t)n, sizeof(jcv_periodic_cell));
    jcv_periodic_edge* edges = 0;
    int result = 0;
    if( !site_of_replica || !central_site || !cells )
        goto cleanup;

    for( int i = 0; i < replica_count; ++i )
        site_of_replica[i] = -1;

    const jcv_site* sites = jcv_diagram_get_sites(&diagram);
    for( int i = 0; i < diagram.numsites; ++i )
    {
        int ridx = (int)sites[i].index;
        if( ridx >= 0 && ridx < replica_count )
            site_of_replica[ridx] = i;
    }

    for( int s = 0; s < n; ++s )
    {
        int central_replica = s * nx * ny + kx * ny + ky;
        central_site[s] = site_of_replica[central_replica];
        if( central_site[s] < 0 )
            goto cleanup;
        cells[s].point = base[s];
    }

    // Count the non-degenerate edges of every central cell first.
    int total_edges = 0;
    for( int s = 0; s < n; ++s )
    {
        jcv_edge_iter iter;
        jcv_edge edge;
        int count = 0;
        jcv_site_get_edges(&diagram, &sites[central_site[s]], &iter);
        while( jcv_edge_next(&iter, &edge) )
        {
            if( edge.sites[1] == 0 )
                goto cleanup; // Boundary artifact: the margins should prevent it.
            if( jcvp_edge_is_degenerate(&edge, max_period) )
                continue;
            ++count;
        }
        cells[s].first_edge = total_edges;
        cells[s].num_edges = count;
        total_edges += count;
    }

    if( total_edges < 0 )
        goto cleanup;

    edges = (jcv_periodic_edge*)malloc(sizeof(jcv_periodic_edge) * (size_t)(total_edges > 0 ? total_edges : 1));
    if( !edges )
        goto cleanup;

    int cursor = 0;
    for( int s = 0; s < n; ++s )
    {
        jcv_edge_iter iter;
        jcv_edge edge;
        double area2 = 0.0;
        double cx = 0.0;
        double cy = 0.0;
        jcv_site_get_edges(&diagram, &sites[central_site[s]], &iter);
        while( jcv_edge_next(&iter, &edge) )
        {
            if( jcvp_edge_is_degenerate(&edge, max_period) )
                continue;

            double x0 = (double)edge.pos[0].x;
            double y0 = (double)edge.pos[0].y;
            double x1 = (double)edge.pos[1].x;
            double y1 = (double)edge.pos[1].y;

            int ridx = (int)edge.sites[1]->index;
            if( ridx < 0 || ridx >= replica_count )
                goto cleanup;

            jcv_periodic_edge* pe = &edges[cursor++];
            pe->pos[0] = edge.pos[0];
            pe->pos[1] = edge.pos[1];
            pe->neighbor = replicas[ridx].original;
            pe->shift_x = replicas[ridx].shift_x;
            pe->shift_y = replicas[ridx].shift_y;

            double cross = x0 * y1 - x1 * y0;
            area2 += cross;
            cx += (x0 + x1) * cross;
            cy += (y0 + y1) * cross;
        }

        if( cursor != cells[s].first_edge + cells[s].num_edges )
            goto cleanup;

        double area = area2 * 0.5;
        if( !(area > 0.0) )
            goto cleanup;
        cells[s].area = (jcv_real)area;
        cells[s].centroid.x = (jcv_real)(cx / (6.0 * area));
        cells[s].centroid.y = (jcv_real)(cy / (6.0 * area));
    }

    if( cursor != total_edges )
        goto cleanup;

    out->rect = *rect;
    out->numsites = n;
    out->numedges = total_edges;
    out->cells = cells;
    out->edges = edges;
    cells = 0;
    edges = 0;
    result = 1;

cleanup:
    free(site_of_replica);
    free(central_site);
    free(cells);
    free(edges);
    jcv_diagram_free(&diagram);
    free(replicas);
    free(replica_points);
    return result;
}

void jcv_periodic_free( jcv_periodic_diagram* diagram )
{
    if( !diagram )
        return;
    free(diagram->cells);
    free(diagram->edges);
    jcvp_zero_diagram(diagram);
}

int jcv_periodic_relax( const jcv_periodic_diagram* diagram, jcv_real alpha, jcv_point* out_points )
{
    if( !diagram || !out_points || !diagram->cells )
        return 0;
    if( diagram->numsites < 1 )
        return 0;

    double a = (double)alpha;
    if( !jcvp_is_finite(a) || a < 0.0 || a > 1.0 )
        return 0;

    double w = (double)diagram->rect.max.x - (double)diagram->rect.min.x;
    double h = (double)diagram->rect.max.y - (double)diagram->rect.min.y;
    if( !(w > 0.0) || !(h > 0.0) ||
        !jcvp_is_finite((double)diagram->rect.min.x) || !jcvp_is_finite((double)diagram->rect.min.y) )
        return 0;

    if( a == 0.0 )
    {
        for( int i = 0; i < diagram->numsites; ++i )
            out_points[i] = diagram->cells[i].point;
        return 1;
    }

    // Compute every new position before writing, so the step is simultaneous
    // even when out_points aliases the caller's previous seed array.
    for( int i = 0; i < diagram->numsites; ++i )
    {
        const jcv_periodic_cell* cell = &diagram->cells[i];
        double x = (double)cell->point.x + a * ((double)cell->centroid.x - (double)cell->point.x);
        double y = (double)cell->point.y + a * ((double)cell->centroid.y - (double)cell->point.y);
        double ox = jcvp_wrapped_offset(x, (double)diagram->rect.min.x, w);
        double oy = jcvp_wrapped_offset(y, (double)diagram->rect.min.y, h);
        out_points[i].x = (jcv_real)((double)diagram->rect.min.x + ox);
        out_points[i].y = (jcv_real)((double)diagram->rect.min.y + oy);
    }
    return 1;
}

#endif // JC_VORONOI_PERIODIC_IMPLEMENTATION
