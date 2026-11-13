// Tests for the periodic (toroidal) Voronoi extension.
//
// Build with:
//   cc -std=c99 -O2 -Wall -Wextra -I src -I support -include support/config.h \
//      support/periodic_test.c -lm -o build/periodic-test
//
// A failed assertion exits with a non-zero status.  On success the program
// prints PERIODIC_OK together with the number of executed assertions.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JC_VORONOI_IMPLEMENTATION
#define JC_VORONOI_PERIODIC_IMPLEMENTATION
#include "jc_voronoi.h"
#include "jc_voronoi_periodic.h"

static int g_assertions = 0;

static void test_fail(int line, const char* expression)
{
    fprintf(stderr, "periodic test failure at line %d: %s\n", line, expression);
    exit(1);
}

#define CHECK(expr) do { ++g_assertions; if( !(expr) ) test_fail(__LINE__, #expr); } while(0)

#define CHECK_CLOSE(actual, expected, tolerance)                                              \
    do {                                                                                      \
        ++g_assertions;                                                                       \
        double check_actual = (double)(actual);                                               \
        double check_expected = (double)(expected);                                           \
        double check_tolerance = (double)(tolerance);                                         \
        if( !(fabs(check_actual - check_expected) <= check_tolerance) )                       \
        {                                                                                     \
            fprintf(stderr, "periodic test failure at line %d: |%.17g - %.17g| > %.3g\n",     \
                    __LINE__, check_actual, check_expected, check_tolerance);                 \
            exit(1);                                                                          \
        }                                                                                     \
    } while(0)

#define CHECK_POINT(actual, expected, tolerance)                                              \
    do {                                                                                      \
        ++g_assertions;                                                                       \
        double dx = (double)(actual).x - (double)(expected).x;                                \
        double dy = (double)(actual).y - (double)(expected).y;                                \
        double tolerance_value = (double)(tolerance);                                         \
        if( !(fabs(dx) <= tolerance_value && fabs(dy) <= tolerance_value) )                   \
        {                                                                                     \
            fprintf(stderr, "periodic test failure at line %d: point (%.17g, %.17g) != "      \
                    "(%.17g, %.17g) tolerance %.3g\n", __LINE__,                              \
                    (double)(actual).x, (double)(actual).y,                                   \
                    (double)(expected).x, (double)(expected).y, tolerance_value);             \
            exit(1);                                                                          \
        }                                                                                     \
    } while(0)

// ---------------------------------------------------------------------------
// Small helpers

static double rect_width(const jcv_rect* rect)
{
    return (double)rect->max.x - (double)rect->min.x;
}

static double rect_height(const jcv_rect* rect)
{
    return (double)rect->max.y - (double)rect->min.y;
}

static double rect_scale(const jcv_rect* rect)
{
    double w = rect_width(rect);
    double h = rect_height(rect);
    return w > h ? w : h;
}

static double length_tolerance(const jcv_rect* rect)
{
    return 2.0e-7 * rect_scale(rect);
}

static double area_tolerance(const jcv_rect* rect)
{
    return 2.0e-7 * rect_width(rect) * rect_height(rect);
}

static double wrap_offset(double value, double origin, double period)
{
    double ratio = (value - origin) / period;
    double offset = (ratio - floor(ratio)) * period;
    if( offset < 0 )
        offset = 0;
    if( offset >= period )
        offset = 0;
    return offset;
}

static jcv_point normalized_point(const jcv_point* p, const jcv_rect* rect)
{
    jcv_point out;
    double w = rect_width(rect);
    double h = rect_height(rect);
    out.x = (jcv_real)((double)rect->min.x + wrap_offset((double)p->x, (double)rect->min.x, w));
    out.y = (jcv_real)((double)rect->min.y + wrap_offset((double)p->y, (double)rect->min.y, h));
    return out;
}

static void expect_rejected(int n, const jcv_point* points, const jcv_rect* rect);

static int diagram_is_zeroed(const jcv_periodic_diagram* diagram)
{
    return diagram->numsites == 0 && diagram->numedges == 0 &&
           diagram->cells == 0 && diagram->edges == 0 &&
           diagram->rect.min.x == 0 && diagram->rect.min.y == 0 &&
           diagram->rect.max.x == 0 && diagram->rect.max.y == 0;
}

// Nearest input seed to (x, y).  Each seed is tested with its 3x3 grid of
// copies, i.e. direct Euclidean distance over the nine neighbouring images.
// Seeds whose distance is within `tie_tolerance` of the minimum are all
// reported through `ties` (when non-NULL), because distance ties do not
// determine a single owner.
static double nearest_seed(const jcv_point* points, int n, const jcv_rect* rect,
                           double x, double y, int* best, int* ties, int* num_ties,
                           double tie_tolerance)
{
    double w = rect_width(rect);
    double h = rect_height(rect);
    double best_dist = -1.0;
    for( int i = 0; i < n; ++i )
    {
        jcv_point base = normalized_point(&points[i], rect);
        double base_kx = floor((x - (double)base.x) / w + 0.5);
        double base_ky = floor((y - (double)base.y) / h + 0.5);
        for( int a = -1; a <= 1; ++a )
        {
            for( int b = -1; b <= 1; ++b )
            {
                double qx = (double)base.x + (base_kx + (double)a) * w;
                double qy = (double)base.y + (base_ky + (double)b) * h;
                double dx = x - qx;
                double dy = y - qy;
                double d2 = dx * dx + dy * dy;
                if( best_dist < 0.0 || d2 < best_dist )
                    best_dist = d2;
            }
        }
    }
    *best = -1;
    if( num_ties )
        *num_ties = 0;
    for( int i = 0; i < n; ++i )
    {
        jcv_point base = normalized_point(&points[i], rect);
        double base_kx = floor((x - (double)base.x) / w + 0.5);
        double base_ky = floor((y - (double)base.y) / h + 0.5);
        double seed_best = -1.0;
        for( int a = -1; a <= 1; ++a )
        {
            for( int b = -1; b <= 1; ++b )
            {
                double qx = (double)base.x + (base_kx + (double)a) * w;
                double qy = (double)base.y + (base_ky + (double)b) * h;
                double dx = x - qx;
                double dy = y - qy;
                double d2 = dx * dx + dy * dy;
                if( seed_best < 0.0 || d2 < seed_best )
                    seed_best = d2;
            }
        }
        if( seed_best <= best_dist + tie_tolerance * tie_tolerance )
        {
            if( *best < 0 )
                *best = i;
            if( ties && num_ties && *num_ties < 16 )
                ties[(*num_ties)++] = i;
        }
    }
    return best_dist;
}

// Convex, counter-clockwise polygon test.  `tol` is a Euclidean distance.
static int point_in_cell(const jcv_periodic_diagram* diagram, const jcv_periodic_cell* cell,
                         double x, double y, double tol)
{
    for( int e = 0; e < cell->num_edges; ++e )
    {
        const jcv_periodic_edge* edge = &diagram->edges[cell->first_edge + e];
        double x0 = (double)edge->pos[0].x;
        double y0 = (double)edge->pos[0].y;
        double x1 = (double)edge->pos[1].x;
        double y1 = (double)edge->pos[1].y;
        double ex = x1 - x0;
        double ey = y1 - y0;
        double cross = ex * (y - y0) - ey * (x - x0);
        double len = sqrt(ex * ex + ey * ey);
        if( len <= 0.0 )
            continue;
        if( cross < -tol * len )
            return 0;
    }
    return 1;
}

static int edge_length_positive(const jcv_periodic_edge* edge)
{
    double dx = (double)edge->pos[1].x - (double)edge->pos[0].x;
    double dy = (double)edge->pos[1].y - (double)edge->pos[0].y;
    return (dx * dx + dy * dy) > 0.0;
}

// ---------------------------------------------------------------------------
// Full per-diagram validation

static void validate_diagram(const jcv_periodic_diagram* diagram, int n, const jcv_point* input,
                             const jcv_rect* rect, int expect_diagonal_shift)
{
    double w = rect_width(rect);
    double h = rect_height(rect);
    double ltol = length_tolerance(rect);
    double atol = area_tolerance(rect);
    double scale = rect_scale(rect);

    CHECK(diagram->numsites == n);
    CHECK(diagram->cells != 0);
    CHECK(diagram->edges != 0 || diagram->numedges == 0);
    CHECK(diagram->rect.min.x == rect->min.x);
    CHECK(diagram->rect.min.y == rect->min.y);
    CHECK(diagram->rect.max.x == rect->max.x);
    CHECK(diagram->rect.max.y == rect->max.y);

    int total_edges = 0;
    for( int i = 0; i < diagram->numsites; ++i )
        total_edges += diagram->cells[i].num_edges;
    CHECK(total_edges == diagram->numedges);

    double total_area = 0.0;
    int saw_diagonal_shift = 0;

    for( int i = 0; i < diagram->numsites; ++i )
    {
        const jcv_periodic_cell* cell = &diagram->cells[i];
        jcv_point expected = normalized_point(&input[i], rect);
        CHECK_POINT(cell->point, expected, 1.0e-12 * scale);
        CHECK(cell->point.x >= rect->min.x && cell->point.x < rect->max.x);
        CHECK(cell->point.y >= rect->min.y && cell->point.y < rect->max.y);
        CHECK(cell->num_edges >= 3);
        CHECK(cell->first_edge >= 0);
        CHECK(cell->first_edge + cell->num_edges <= diagram->numedges);

        double area2 = 0.0;
        double cx_total = 0.0;
        double cy_total = 0.0;
        double fan_area2 = 0.0;
        double fan_cx = 0.0;
        double fan_cy = 0.0;

        for( int e = 0; e < cell->num_edges; ++e )
        {
            const jcv_periodic_edge* edge = &diagram->edges[cell->first_edge + e];
            const jcv_periodic_edge* next = &diagram->edges[cell->first_edge + (e + 1) % cell->num_edges];

            CHECK(edge_length_positive(edge));
            // The ring is a closed counter-clockwise chain.
            CHECK_POINT(edge->pos[1], next->pos[0], ltol);

            double x0 = (double)edge->pos[0].x;
            double y0 = (double)edge->pos[0].y;
            double x1 = (double)edge->pos[1].x;
            double y1 = (double)edge->pos[1].y;

            double cross = x0 * y1 - x1 * y0;
            area2 += cross;
            cx_total += (x0 + x1) * cross;
            cy_total += (y0 + y1) * cross;

            // Independent triangle-fan moments anchored at the seed.
            double px = (double)cell->point.x;
            double py = (double)cell->point.y;
            double tcross = (x0 - px) * (y1 - py) - (y0 - py) * (x1 - px);
            CHECK(tcross > -atol);
            fan_area2 += tcross;
            fan_cx += tcross * (px + x0 + x1);
            fan_cy += tcross * (py + y0 + y1);

            // Every edge is equidistant from the cell's seed and the
            // neighbour replica, and both endpoints lie on that bisector.
            CHECK(edge->neighbor >= 0 && edge->neighbor < n);
            CHECK(edge->shift_x >= -(int)(4.0 * scale / w + 16));
            CHECK(edge->shift_x <= (int)(4.0 * scale / w + 16));
            CHECK(edge->shift_y >= -(int)(4.0 * scale / h + 16));
            CHECK(edge->shift_y <= (int)(4.0 * scale / h + 16));
            if( edge->shift_x != 0 && edge->shift_y != 0 )
                saw_diagonal_shift = 1;

            const jcv_periodic_cell* neighbor_cell = &diagram->cells[edge->neighbor];
            jcv_point replica;
            replica.x = (jcv_real)((double)neighbor_cell->point.x + (double)edge->shift_x * w);
            replica.y = (jcv_real)((double)neighbor_cell->point.y + (double)edge->shift_y * h);

            for( int endpoint = 0; endpoint < 2; ++endpoint )
            {
                double ex = (double)edge->pos[endpoint].x;
                double ey = (double)edge->pos[endpoint].y;
                double d0 = hypot(ex - (double)cell->point.x, ey - (double)cell->point.y);
                double d1 = hypot(ex - (double)replica.x, ey - (double)replica.y);
                CHECK_CLOSE(d0, d1, ltol);
            }

            // Midpoint belongs to the Voronoi regions of both replicas, i.e.
            // no other seed copy is strictly closer.
            double mx = 0.5 * (x0 + x1);
            double my = 0.5 * (y0 + y1);
            int best = -1;
            int midpoint_ties = 0;
            double best2 = nearest_seed(input, n, rect, mx, my, &best, 0, &midpoint_ties, ltol);
            double own = hypot(mx - (double)cell->point.x, my - (double)cell->point.y);
            double other = hypot(mx - (double)replica.x, my - (double)replica.y);
            CHECK(best >= 0);
            CHECK(own <= sqrt(best2) + ltol);
            CHECK(other <= sqrt(best2) + ltol);
            CHECK_CLOSE(own, other, ltol);
        }

        double area = area2 * 0.5;
        CHECK(area > 0.0);
        CHECK_CLOSE(cell->area, area, atol);
        CHECK_CLOSE(fan_area2 * 0.5, area, atol * 4.0);

        jcv_point centroid;
        centroid.x = (jcv_real)(cx_total / (6.0 * area));
        centroid.y = (jcv_real)(cy_total / (6.0 * area));
        CHECK_POINT(cell->centroid, centroid, ltol);

        // Independent moment computation from the triangle fan.
        jcv_point fan_centroid;
        fan_centroid.x = (jcv_real)(fan_cx / (3.0 * fan_area2));
        fan_centroid.y = (jcv_real)(fan_cy / (3.0 * fan_area2));
        CHECK_POINT(cell->centroid, fan_centroid, ltol);

        // The seed is inside its own (convex) polygon.
        CHECK(point_in_cell(diagram, cell, (double)cell->point.x, (double)cell->point.y, ltol));

        total_area += (double)cell->area;
    }

    CHECK_CLOSE(total_area, w * h, atol);
    if( expect_diagonal_shift )
        CHECK(saw_diagonal_shift == 1);

    // Every edge has exactly one reversed partner in the neighbour's ring:
    // e.pos[k] - shift * period is the neighbour edge traversed backwards.
    for( int i = 0; i < diagram->numsites; ++i )
    {
        const jcv_periodic_cell* cell = &diagram->cells[i];
        for( int e = 0; e < cell->num_edges; ++e )
        {
            const jcv_periodic_edge* edge = &diagram->edges[cell->first_edge + e];
            const jcv_periodic_cell* neighbor_cell = &diagram->cells[edge->neighbor];
            int matches = 0;
            for( int f = 0; f < neighbor_cell->num_edges; ++f )
            {
                const jcv_periodic_edge* reverse = &diagram->edges[neighbor_cell->first_edge + f];
                if( reverse->neighbor != i )
                    continue;
                if( reverse->shift_x != -edge->shift_x || reverse->shift_y != -edge->shift_y )
                    continue;
                double shift_x = (double)edge->shift_x * w;
                double shift_y = (double)edge->shift_y * h;
                jcv_point expected0;
                jcv_point expected1;
                expected0.x = (jcv_real)((double)edge->pos[1].x - shift_x);
                expected0.y = (jcv_real)((double)edge->pos[1].y - shift_y);
                expected1.x = (jcv_real)((double)edge->pos[0].x - shift_x);
                expected1.y = (jcv_real)((double)edge->pos[0].y - shift_y);
                if( fabs((double)reverse->pos[0].x - (double)expected0.x) <= ltol &&
                    fabs((double)reverse->pos[0].y - (double)expected0.y) <= ltol &&
                    fabs((double)reverse->pos[1].x - (double)expected1.x) <= ltol &&
                    fabs((double)reverse->pos[1].y - (double)expected1.y) <= ltol )
                {
                    ++matches;
                }
            }
            CHECK(matches == 1);
        }
    }

    // Sampling attribution: for sample points inside the basic rectangle the
    // directly computed 3x3 nearest seed must own the point.
    const int samples_per_axis = 7;
    for( int sy = 0; sy < samples_per_axis; ++sy )
    {
        for( int sx = 0; sx < samples_per_axis; ++sx )
        {
            double fx = ((double)sx + 0.371) / (double)samples_per_axis;
            double fy = ((double)sy + 0.619) / (double)samples_per_axis;
            double x = (double)rect->min.x + fx * w;
            double y = (double)rect->min.y + fy * h;
            int ties[16];
            int num_ties = 0;
            int best = -1;
            nearest_seed(input, n, rect, x, y, &best, ties, &num_ties, ltol);
            CHECK(best >= 0);
            CHECK(num_ties >= 1);
            int owner = 0;
            for( int t = 0; t < num_ties; ++t )
            {
                int candidate = ties[t];
                const jcv_periodic_cell* cell = &diagram->cells[candidate];
                double kx = floor((x - (double)cell->point.x) / w + 0.5);
                double ky = floor((y - (double)cell->point.y) / h + 0.5);
                double qx = x - kx * w;
                double qy = y - ky * h;
                if( point_in_cell(diagram, cell, qx, qy, ltol) )
                    owner = 1;
            }
            CHECK(owner == 1);
        }
    }
}

// ---------------------------------------------------------------------------
// Test cases

static void test_single_seed(void)
{
    jcv_point input[1] = { { (jcv_real)0.3, (jcv_real)0.7 } };
    jcv_rect rect = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)1.0, (jcv_real)1.0 } };
    jcv_periodic_diagram diagram;
    memset(&diagram, 0, sizeof(diagram));

    CHECK(jcv_periodic_generate(1, input, &rect, &diagram) == 1);
    validate_diagram(&diagram, 1, input, &rect, 0);

    CHECK(diagram.numsites == 1);
    CHECK(diagram.cells[0].num_edges == 4);
    CHECK_CLOSE(diagram.cells[0].area, 1.0, area_tolerance(&rect));
    CHECK_POINT(diagram.cells[0].centroid, input[0], length_tolerance(&rect));

    // The four edges lead back to the seed itself, one period away on each axis.
    int horizontal = 0;
    int vertical = 0;
    for( int e = 0; e < diagram.cells[0].num_edges; ++e )
    {
        const jcv_periodic_edge* edge = &diagram.edges[diagram.cells[0].first_edge + e];
        CHECK(edge->neighbor == 0);
        if( edge->shift_x != 0 && edge->shift_y == 0 )
        {
            CHECK(abs(edge->shift_x) == 1);
            ++horizontal;
        }
        else if( edge->shift_x == 0 && edge->shift_y != 0 )
        {
            CHECK(abs(edge->shift_y) == 1);
            ++vertical;
        }
        else
        {
            CHECK(0);
        }
    }
    CHECK(horizontal == 2);
    CHECK(vertical == 2);

    jcv_periodic_free(&diagram);
}

static void test_two_seeds_offsets(void)
{
    jcv_point input[2] = { { (jcv_real)0.1, (jcv_real)0.2 }, { (jcv_real)0.6, (jcv_real)0.2 } };
    jcv_rect rect = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)1.0, (jcv_real)1.0 } };
    jcv_periodic_diagram diagram;
    memset(&diagram, 0, sizeof(diagram));

    CHECK(jcv_periodic_generate(2, input, &rect, &diagram) == 1);
    validate_diagram(&diagram, 2, input, &rect, 0);
    CHECK(diagram.numsites == 2);

    // Cell 0 must keep both horizontal offsets of the same neighbour plus the
    // vertical self adjacency.  Cell 1 is symmetric.
    for( int i = 0; i < 2; ++i )
    {
        const jcv_periodic_cell* cell = &diagram.cells[i];
        CHECK(cell->num_edges == 4);
        int neighbor_horizontal = 0;
        int self_vertical = 0;
        int horizontal_shifts[2] = { 0, 0 };
        int found_horizontal = 0;
        for( int e = 0; e < cell->num_edges; ++e )
        {
            const jcv_periodic_edge* edge = &diagram.edges[cell->first_edge + e];
            if( edge->neighbor == 1 - i && edge->shift_y == 0 )
            {
                // The same neighbour appears at two adjacent lattice copies.
                CHECK(abs(edge->shift_x) <= 1);
                horizontal_shifts[found_horizontal++] = edge->shift_x;
                ++neighbor_horizontal;
            }
            if( edge->neighbor == i && edge->shift_x == 0 && edge->shift_y != 0 )
            {
                ++self_vertical;
            }
        }
        CHECK(neighbor_horizontal == 2);
        CHECK(self_vertical == 2);
        CHECK(found_horizontal == 2);
        CHECK(horizontal_shifts[0] != horizontal_shifts[1]);
        CHECK_CLOSE(cell->area, 0.5, area_tolerance(&rect));
        CHECK_POINT(cell->centroid, input[i], length_tolerance(&rect));
    }

    jcv_periodic_free(&diagram);
}

static void run_case(const char* name, int n, const jcv_point* input, const jcv_rect* rect,
                     int expect_diagonal_shift)
{
    (void)name;
    jcv_point input_copy[256];
    jcv_rect rect_copy = *rect;
    memcpy(input_copy, input, sizeof(jcv_point) * (size_t)n);

    jcv_periodic_diagram diagram;
    memset(&diagram, 0, sizeof(diagram));

    CHECK(jcv_periodic_generate(n, input, rect, &diagram) == 1);
    CHECK(memcmp(input_copy, input, sizeof(jcv_point) * (size_t)n) == 0);
    CHECK(rect_copy.min.x == rect->min.x && rect_copy.min.y == rect->min.y);
    CHECK(rect_copy.max.x == rect->max.x && rect_copy.max.y == rect->max.y);
    validate_diagram(&diagram, n, input, rect, expect_diagonal_shift);
    jcv_periodic_free(&diagram);
}

static void test_asymmetric_four(void)
{
    jcv_point square[4] = {
        { (jcv_real)0.08, (jcv_real)0.17 },
        { (jcv_real)0.58, (jcv_real)0.22 },
        { (jcv_real)0.33, (jcv_real)0.81 },
        { (jcv_real)0.87, (jcv_real)0.69 },
    };
    jcv_rect unit = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)1.0, (jcv_real)1.0 } };
    run_case("asymmetric unit", 4, square, &unit, 1);

    // Non-square rectangle, origin off zero, spanning both seams on both axes.
    jcv_rect wide = { { (jcv_real)-2.25, (jcv_real)1.5 }, { (jcv_real)1.45, (jcv_real)3.1 } };
    jcv_point wide_points[4];
    for( int i = 0; i < 4; ++i )
    {
        wide_points[i].x = (jcv_real)((double)wide.min.x + (double)square[i].x * rect_width(&wide));
        wide_points[i].y = (jcv_real)((double)wide.min.y + (double)square[i].y * rect_height(&wide));
    }
    run_case("asymmetric rect", 4, wide_points, &wide, 1);

    // Tall rectangle, aspect ratio 16.
    jcv_rect tall = { { (jcv_real)0.0, (jcv_real)-4.0 }, { (jcv_real)1.0, (jcv_real)12.0 } };
    jcv_point tall_points[4];
    for( int i = 0; i < 4; ++i )
    {
        tall_points[i].x = (jcv_real)((double)tall.min.x + (double)square[i].x * rect_width(&tall));
        tall_points[i].y = (jcv_real)((double)tall.min.y + (double)square[i].y * rect_height(&tall));
    }
    run_case("tall rect", 4, tall_points, &tall, 1);
}

static void test_collinear_and_grid(void)
{
    jcv_rect unit = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)1.0, (jcv_real)1.0 } };

    jcv_point collinear[5] = {
        { (jcv_real)0.1, (jcv_real)0.5 },
        { (jcv_real)0.3, (jcv_real)0.5 },
        { (jcv_real)0.5, (jcv_real)0.5 },
        { (jcv_real)0.7, (jcv_real)0.5 },
        { (jcv_real)0.9, (jcv_real)0.5 },
    };
    run_case("collinear", 5, collinear, &unit, 0);

    jcv_point grid[4] = {
        { (jcv_real)0.25, (jcv_real)0.25 },
        { (jcv_real)0.75, (jcv_real)0.25 },
        { (jcv_real)0.25, (jcv_real)0.75 },
        { (jcv_real)0.75, (jcv_real)0.75 },
    };
    run_case("grid", 4, grid, &unit, 0);

    jcv_rect rect = { { (jcv_real)1.0, (jcv_real)2.0 }, { (jcv_real)3.0, (jcv_real)3.5 } };
    jcv_point grid_offset[4] = {
        { (jcv_real)1.2, (jcv_real)2.3 },
        { (jcv_real)1.4, (jcv_real)2.3 },
        { (jcv_real)1.2, (jcv_real)2.6 },
        { (jcv_real)1.4, (jcv_real)2.6 },
    };
    run_case("offset grid", 4, grid_offset, &rect, 0);
}

static void test_boundary_and_range(void)
{
    jcv_rect rect = { { (jcv_real)-1.0, (jcv_real)2.0 }, { (jcv_real)1.0, (jcv_real)3.0 } };
    double w = rect_width(&rect);
    double h = rect_height(&rect);

    // Seeds on the closed maximum boundary normalize to the minimum.
    jcv_point boundary[3] = {
        { (jcv_real)1.0, (jcv_real)2.5 },
        { (jcv_real)0.0, (jcv_real)3.0 },
        { (jcv_real)0.5, (jcv_real)2.2 },
    };
    jcv_periodic_diagram diagram;
    memset(&diagram, 0, sizeof(diagram));
    CHECK(jcv_periodic_generate(3, boundary, &rect, &diagram) == 1);
    jcv_point expected0;
    jcv_point expected1;
    expected0.x = rect.min.x;
    expected0.y = (jcv_real)2.5;
    expected1.x = (jcv_real)0.0;
    expected1.y = rect.min.y;
    CHECK_POINT(diagram.cells[0].point, expected0, 1e-12 * rect_scale(&rect));
    CHECK_POINT(diagram.cells[1].point, expected1, 1e-12 * rect_scale(&rect));
    CHECK(diagram.cells[0].point.x >= rect.min.x && diagram.cells[0].point.x < rect.max.x);
    CHECK(diagram.cells[1].point.y >= rect.min.y && diagram.cells[1].point.y < rect.max.y);
    jcv_periodic_free(&diagram);

    // A displacement of exactly ten periods on each axis is accepted, and it
    // normalizes to the same configuration as an in-range input.
    jcv_point inside[3] = {
        { (jcv_real)-0.6, (jcv_real)2.4 },
        { (jcv_real)-0.1, (jcv_real)2.9 },
        { (jcv_real)0.7, (jcv_real)2.1 },
    };
    jcv_point far_points[3];
    for( int i = 0; i < 3; ++i )
    {
        // The farthest seed ends up exactly ten periods from rect.min on the
        // negative side, which is the inclusive limit.
        far_points[i].x = (jcv_real)((double)inside[i].x - 10.0 * w);
        far_points[i].y = (jcv_real)((double)inside[i].y - 10.0 * h);
    }
    jcv_periodic_diagram near_diagram;
    jcv_periodic_diagram far_diagram;
    memset(&near_diagram, 0, sizeof(near_diagram));
    memset(&far_diagram, 0, sizeof(far_diagram));
    CHECK(jcv_periodic_generate(3, inside, &rect, &near_diagram) == 1);
    CHECK(jcv_periodic_generate(3, far_points, &rect, &far_diagram) == 1);
    validate_diagram(&far_diagram, 3, far_points, &rect, 0);
    for( int i = 0; i < 3; ++i )
    {
        CHECK_POINT(far_diagram.cells[i].point, near_diagram.cells[i].point, 1e-12 * rect_scale(&rect));
        CHECK_CLOSE(far_diagram.cells[i].area, near_diagram.cells[i].area, area_tolerance(&rect));
        CHECK_POINT(far_diagram.cells[i].centroid, near_diagram.cells[i].centroid,
                    length_tolerance(&rect));
    }
    jcv_periodic_free(&near_diagram);
    jcv_periodic_free(&far_diagram);

    // Just beyond ten periods is rejected.  Measure from the seed sitting on
    // rect.min so the displacement is exactly 10.001 periods.
    jcv_point too_far[3];
    for( int i = 0; i < 3; ++i )
    {
        too_far[i].x = (jcv_real)((double)rect.min.x - 10.001 * w);
        too_far[i].y = (jcv_real)inside[i].y;
    }
    expect_rejected(3, too_far, &rect);
}

static void test_relax_cycle(void)
{
    jcv_rect rect = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)2.5, (jcv_real)1.25 } };
    jcv_point seeds[4];
    jcv_point original[4] = {
        { (jcv_real)0.08, (jcv_real)0.17 },
        { (jcv_real)0.58, (jcv_real)0.22 },
        { (jcv_real)0.33, (jcv_real)0.81 },
        { (jcv_real)0.87, (jcv_real)0.69 },
    };
    for( int i = 0; i < 4; ++i )
    {
        seeds[i].x = (jcv_real)((double)rect.min.x + (double)original[i].x * rect_width(&rect));
        seeds[i].y = (jcv_real)((double)rect.min.y + (double)original[i].y * rect_height(&rect));
    }
    const double alphas[3] = { 0.0, 0.5, 1.0 };
    double ltol = length_tolerance(&rect);

    jcv_periodic_diagram diagram;
    memset(&diagram, 0, sizeof(diagram));
    CHECK(jcv_periodic_generate(4, seeds, &rect, &diagram) == 1);
    validate_diagram(&diagram, 4, seeds, &rect, 1);

    // Each stale diagram must stay readable after later generations.
    jcv_periodic_cell saved_cells[4];
    jcv_periodic_edge saved_edges[64];

    jcv_point relaxed[4];
    for( int iteration = 0; iteration < 3; ++iteration )
    {
        double alpha = alphas[iteration];

        int saved_edges_count = diagram.numedges;
        CHECK(saved_edges_count <= 64);
        memcpy(saved_cells, diagram.cells, sizeof(jcv_periodic_cell) * 4);
        memcpy(saved_edges, diagram.edges, sizeof(jcv_periodic_edge) * (size_t)saved_edges_count);
        jcv_periodic_cell* old_cells = diagram.cells;
        jcv_periodic_edge* old_edges = diagram.edges;

        // alpha = 0 yields exactly the normalized seeds.
        jcv_point zero_out[4];
        CHECK(jcv_periodic_relax(&diagram, (jcv_real)0.0, zero_out) == 1);
        for( int i = 0; i < 4; ++i )
            CHECK_POINT(zero_out[i], normalized_point(&seeds[i], &rect), 1.0e-12 * rect_scale(&rect));

        CHECK(jcv_periodic_relax(&diagram, (jcv_real)alpha, relaxed) == 1);
        for( int i = 0; i < 4; ++i )
        {
            double expected_x = (double)diagram.cells[i].point.x +
                alpha * ((double)diagram.cells[i].centroid.x - (double)diagram.cells[i].point.x);
            double expected_y = (double)diagram.cells[i].point.y +
                alpha * ((double)diagram.cells[i].centroid.y - (double)diagram.cells[i].point.y);
            jcv_point expected;
            expected.x = (jcv_real)expected_x;
            expected.y = (jcv_real)expected_y;
            CHECK_POINT(relaxed[i], normalized_point(&expected, &rect), ltol);
        }

        // The old diagram is untouched by relax and by the next generate.
        CHECK(diagram.cells == old_cells);
        CHECK(diagram.edges == old_edges);
        CHECK(diagram.numsites == 4);
        CHECK(memcmp(diagram.cells, saved_cells, sizeof(jcv_periodic_cell) * 4) == 0);
        CHECK(memcmp(diagram.edges, saved_edges,
                     sizeof(jcv_periodic_edge) * (size_t)saved_edges_count) == 0);
        validate_diagram(&diagram, 4, seeds, &rect, 1);

        // Regenerate from the relaxed seeds.
        jcv_periodic_diagram next;
        memset(&next, 0, sizeof(next));
        CHECK(jcv_periodic_generate(4, relaxed, &rect, &next) == 1);
        validate_diagram(&next, 4, relaxed, &rect, 1);

        // alpha = 1 must land on the wrapped centroids of the previous diagram.
        jcv_point centroid_out[4];
        CHECK(jcv_periodic_relax(&diagram, (jcv_real)1.0, centroid_out) == 1);
        for( int i = 0; i < 4; ++i )
            CHECK_POINT(centroid_out[i], normalized_point(&diagram.cells[i].centroid, &rect), ltol);

        // alpha = 0.5 must be exactly halfway in the unfolded space.
        jcv_point half_out[4];
        CHECK(jcv_periodic_relax(&diagram, (jcv_real)0.5, half_out) == 1);
        for( int i = 0; i < 4; ++i )
        {
            jcv_point mid;
            mid.x = (jcv_real)((double)diagram.cells[i].point.x +
                               0.5 * ((double)diagram.cells[i].centroid.x - (double)diagram.cells[i].point.x));
            mid.y = (jcv_real)((double)diagram.cells[i].point.y +
                               0.5 * ((double)diagram.cells[i].centroid.y - (double)diagram.cells[i].point.y));
            CHECK_POINT(half_out[i], normalized_point(&mid, &rect), ltol);
        }

        memcpy(seeds, relaxed, sizeof(seeds));
        jcv_periodic_free(&diagram);
        diagram = next;
    }

    // Same input twice must produce identical output order and values.
    jcv_periodic_diagram first;
    jcv_periodic_diagram second;
    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    CHECK(jcv_periodic_generate(4, seeds, &rect, &first) == 1);
    CHECK(jcv_periodic_generate(4, seeds, &rect, &second) == 1);
    CHECK(first.numsites == second.numsites);
    CHECK(first.numedges == second.numedges);
    CHECK(memcmp(first.cells, second.cells, sizeof(jcv_periodic_cell) * 4) == 0);
    CHECK(memcmp(first.edges, second.edges,
                 sizeof(jcv_periodic_edge) * (size_t)first.numedges) == 0);
    jcv_periodic_free(&first);
    jcv_periodic_free(&second);
    jcv_periodic_free(&diagram);
}

static void test_translation(void)
{
    jcv_rect rect = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)3.2, (jcv_real)1.7 } };
    jcv_point input[4] = {
        { (jcv_real)0.25, (jcv_real)0.19 },
        { (jcv_real)1.85, (jcv_real)0.37 },
        { (jcv_real)1.05, (jcv_real)1.38 },
        { (jcv_real)2.78, (jcv_real)1.17 },
    };
    double w = rect_width(&rect);
    double h = rect_height(&rect);
    double ltol = length_tolerance(&rect);

    jcv_periodic_diagram base;
    memset(&base, 0, sizeof(base));
    CHECK(jcv_periodic_generate(4, input, &rect, &base) == 1);
    validate_diagram(&base, 4, input, &rect, 1);

    // Whole-period translation: identical normalized seeds and unfolded cells.
    jcv_point shifted[4];
    for( int i = 0; i < 4; ++i )
    {
        shifted[i].x = (jcv_real)((double)input[i].x + 3.0 * w);
        shifted[i].y = (jcv_real)((double)input[i].y - 2.0 * h);
    }
    jcv_periodic_diagram moved;
    memset(&moved, 0, sizeof(moved));
    CHECK(jcv_periodic_generate(4, shifted, &rect, &moved) == 1);
    validate_diagram(&moved, 4, shifted, &rect, 1);
    for( int i = 0; i < 4; ++i )
    {
        CHECK_POINT(moved.cells[i].point, base.cells[i].point, ltol);
        CHECK_POINT(moved.cells[i].centroid, base.cells[i].centroid, ltol);
        CHECK_CLOSE(moved.cells[i].area, base.cells[i].area, area_tolerance(&rect));
    }
    for( int i = 0; i < 4; ++i )
    {
        jcv_point relaxed_base[4];
        jcv_point relaxed_moved[4];
        CHECK(jcv_periodic_relax(&base, (jcv_real)1.0, relaxed_base) == 1);
        CHECK(jcv_periodic_relax(&moved, (jcv_real)1.0, relaxed_moved) == 1);
        for( int k = 0; k < 4; ++k )
            CHECK_POINT(relaxed_moved[k], relaxed_base[k], ltol);
    }
    jcv_periodic_free(&moved);

    // Continuous translation across the seam: the whole configuration moves by
    // (dx, dy); per-seed normalization subtracts an integer number of periods,
    // and the unfolded polygon and centroid follow that same copy.  Nothing
    // collapses toward the rectangle centre.
    double dx = 0.62 * w;
    double dy = 0.31 * h;
    jcv_point translated[4];
    int wraps_x[4];
    int wraps_y[4];
    for( int i = 0; i < 4; ++i )
    {
        double raw_x = (double)input[i].x + dx;
        double raw_y = (double)input[i].y + dy;
        wraps_x[i] = (int)floor((raw_x - (double)rect.min.x) / w);
        wraps_y[i] = (int)floor((raw_y - (double)rect.min.y) / h);
        translated[i].x = (jcv_real)raw_x;
        translated[i].y = (jcv_real)raw_y;
    }
    jcv_periodic_diagram continuous;
    memset(&continuous, 0, sizeof(continuous));
    CHECK(jcv_periodic_generate(4, translated, &rect, &continuous) == 1);
    validate_diagram(&continuous, 4, translated, &rect, 1);

    int crossed_seam = 0;
    for( int i = 0; i < 4; ++i )
    {
        CHECK_POINT(continuous.cells[i].point, normalized_point(&translated[i], &rect), ltol);
        jcv_point expected_centroid;
        expected_centroid.x = (jcv_real)((double)base.cells[i].centroid.x + dx - (double)wraps_x[i] * w);
        expected_centroid.y = (jcv_real)((double)base.cells[i].centroid.y + dy - (double)wraps_y[i] * h);
        CHECK_POINT(continuous.cells[i].centroid, expected_centroid, ltol);
        CHECK_CLOSE(continuous.cells[i].area, base.cells[i].area, area_tolerance(&rect));
        if( wraps_x[i] != 0 || wraps_y[i] != 0 )
            crossed_seam = 1;
    }
    CHECK(crossed_seam == 1);

    // Whole unfolded cell moves with the matching copy: compare the edge
    // positions as a multiset, translated per cell.
    for( int i = 0; i < 4; ++i )
    {
        const jcv_periodic_cell* old_cell = &base.cells[i];
        const jcv_periodic_cell* new_cell = &continuous.cells[i];
        CHECK(old_cell->num_edges == new_cell->num_edges);
        double shift_x = dx - (double)wraps_x[i] * w;
        double shift_y = dy - (double)wraps_y[i] * h;
        for( int e = 0; e < old_cell->num_edges; ++e )
        {
            const jcv_periodic_edge* old_edge = &base.edges[old_cell->first_edge + e];
            int found = 0;
            for( int f = 0; f < new_cell->num_edges; ++f )
            {
                const jcv_periodic_edge* new_edge = &continuous.edges[new_cell->first_edge + f];
                if( new_edge->neighbor != old_edge->neighbor )
                    continue;
                double want0x = (double)old_edge->pos[0].x + shift_x;
                double want0y = (double)old_edge->pos[0].y + shift_y;
                double want1x = (double)old_edge->pos[1].x + shift_x;
                double want1y = (double)old_edge->pos[1].y + shift_y;
                if( fabs((double)new_edge->pos[0].x - want0x) <= ltol &&
                    fabs((double)new_edge->pos[0].y - want0y) <= ltol &&
                    fabs((double)new_edge->pos[1].x - want1x) <= ltol &&
                    fabs((double)new_edge->pos[1].y - want1y) <= ltol )
                {
                    ++found;
                }
            }
            CHECK(found == 1);
        }
    }

    jcv_periodic_free(&continuous);
    jcv_periodic_free(&base);
}

static void test_permutation(void)
{
    jcv_rect rect = { { (jcv_real)-1.0, (jcv_real)0.5 }, { (jcv_real)1.5, (jcv_real)2.0 } };
    jcv_point input[4] = {
        { (jcv_real)0.08, (jcv_real)0.17 },
        { (jcv_real)0.58, (jcv_real)0.22 },
        { (jcv_real)0.33, (jcv_real)0.81 },
        { (jcv_real)0.87, (jcv_real)0.69 },
    };
    // Reorder so the sort order changes without introducing coordinate ties.
    const int permutation[4] = { 2, 0, 3, 1 };
    jcv_point permuted[4];
    for( int i = 0; i < 4; ++i )
    {
        permuted[i].x = (jcv_real)((double)rect.min.x +
                                   (double)input[permutation[i]].x * rect_width(&rect));
        permuted[i].y = (jcv_real)((double)rect.min.y +
                                   (double)input[permutation[i]].y * rect_height(&rect));
    }
    jcv_point ordered[4];
    for( int i = 0; i < 4; ++i )
    {
        ordered[i].x = (jcv_real)((double)rect.min.x + (double)input[i].x * rect_width(&rect));
        ordered[i].y = (jcv_real)((double)rect.min.y + (double)input[i].y * rect_height(&rect));
    }
    double ltol = length_tolerance(&rect);

    jcv_periodic_diagram reference;
    jcv_periodic_diagram reordered;
    memset(&reference, 0, sizeof(reference));
    memset(&reordered, 0, sizeof(reordered));
    CHECK(jcv_periodic_generate(4, ordered, &rect, &reference) == 1);
    CHECK(jcv_periodic_generate(4, permuted, &rect, &reordered) == 1);
    validate_diagram(&reference, 4, ordered, &rect, 1);
    validate_diagram(&reordered, 4, permuted, &rect, 1);

    // Cells keep their input order.
    for( int i = 0; i < 4; ++i )
        CHECK_POINT(reordered.cells[i].point, permuted[i], 1e-12 * rect_scale(&rect));

    for( int i = 0; i < 4; ++i )
    {
        const jcv_periodic_cell* a = &reordered.cells[i];
        const jcv_periodic_cell* b = &reference.cells[permutation[i]];
        CHECK_CLOSE(a->area, b->area, area_tolerance(&rect));
        CHECK_POINT(a->centroid, b->centroid, ltol);
        CHECK(a->num_edges == b->num_edges);

        // Edge multiset with neighbour indices remapped through the permutation.
        int matched = 0;
        for( int e = 0; e < a->num_edges; ++e )
        {
            const jcv_periodic_edge* ea = &reordered.edges[a->first_edge + e];
            int want_neighbor = permutation[ea->neighbor];
            int found = 0;
            for( int f = 0; f < b->num_edges; ++f )
            {
                const jcv_periodic_edge* eb = &reference.edges[b->first_edge + f];
                if( eb->neighbor != want_neighbor )
                    continue;
                if( eb->shift_x != ea->shift_x || eb->shift_y != ea->shift_y )
                    continue;
                if( fabs((double)eb->pos[0].x - (double)ea->pos[0].x) <= ltol &&
                    fabs((double)eb->pos[0].y - (double)ea->pos[0].y) <= ltol &&
                    fabs((double)eb->pos[1].x - (double)ea->pos[1].x) <= ltol &&
                    fabs((double)eb->pos[1].y - (double)ea->pos[1].y) <= ltol )
                {
                    ++found;
                }
            }
            CHECK(found == 1);
            ++matched;
        }
        CHECK(matched == a->num_edges);
    }

    jcv_periodic_free(&reference);
    jcv_periodic_free(&reordered);
}

static void expect_rejected(int n, const jcv_point* points, const jcv_rect* rect)
{
    jcv_periodic_diagram diagram;
    memset(&diagram, 0, sizeof(diagram));
    CHECK(jcv_periodic_generate(n, points, rect, &diagram) == 0);
    CHECK(diagram_is_zeroed(&diagram));
}

static void test_invalid_inputs(void)
{
    jcv_rect unit = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)1.0, (jcv_real)1.0 } };
    jcv_point good[3] = {
        { (jcv_real)0.1, (jcv_real)0.1 },
        { (jcv_real)0.5, (jcv_real)0.6 },
        { (jcv_real)0.8, (jcv_real)0.3 },
    };

    // Point count range.
    expect_rejected(0, good, &unit);
    {
        jcv_point many[257];
        for( int i = 0; i < 257; ++i )
        {
            many[i].x = (jcv_real)((double)(i % 16) / 16.0 + 0.001);
            many[i].y = (jcv_real)((double)(i / 16) / 17.0 + 0.002);
        }
        expect_rejected(257, many, &unit);
    }

    // Null arguments.
    {
        jcv_periodic_diagram diagram;
        memset(&diagram, 0, sizeof(diagram));
        CHECK(jcv_periodic_generate(3, 0, &unit, &diagram) == 0);
        CHECK(diagram_is_zeroed(&diagram));
        CHECK(jcv_periodic_generate(3, good, 0, &diagram) == 0);
        CHECK(diagram_is_zeroed(&diagram));
        CHECK(jcv_periodic_generate(3, good, &unit, 0) == 0);
    }

    // Degenerate and out-of-range rectangles.
    {
        jcv_rect inverted = { { (jcv_real)1.0, (jcv_real)0.0 }, { (jcv_real)0.0, (jcv_real)1.0 } };
        jcv_rect too_small = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)0.009, (jcv_real)1.0 } };
        jcv_rect too_large = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)100.5, (jcv_real)1.0 } };
        jcv_rect too_wide = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)17.0, (jcv_real)1.0 } };
        jcv_rect too_tall = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)1.0, (jcv_real)17.0 } };
        jcv_rect origin_x = { { (jcv_real)1000.5, (jcv_real)0.0 }, { (jcv_real)1001.5, (jcv_real)1.0 } };
        jcv_rect origin_y = { { (jcv_real)0.0, (jcv_real)-1000.5 }, { (jcv_real)1.0, (jcv_real)-999.5 } };
        jcv_rect nan_rect = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)NAN, (jcv_real)1.0 } };
        jcv_rect inf_rect = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)1.0, (jcv_real)INFINITY } };
        expect_rejected(3, good, &inverted);
        expect_rejected(3, good, &too_small);
        expect_rejected(3, good, &too_large);
        expect_rejected(3, good, &too_wide);
        expect_rejected(3, good, &too_tall);
        expect_rejected(3, good, &origin_x);
        expect_rejected(3, good, &origin_y);
        expect_rejected(3, good, &nan_rect);
        expect_rejected(3, good, &inf_rect);
    }

    // Non-finite and out-of-range points.
    {
        jcv_point bad[3] = { { (jcv_real)0.1, (jcv_real)0.1 },
                             { (jcv_real)0.5, (jcv_real)0.6 },
                             { (jcv_real)0.8, (jcv_real)0.3 } };
        bad[1].x = (jcv_real)NAN;
        expect_rejected(3, bad, &unit);
        bad[1].x = (jcv_real)INFINITY;
        expect_rejected(3, bad, &unit);
        bad[1].x = (jcv_real)0.5;
        bad[2].y = (jcv_real)(10.5);
        expect_rejected(3, bad, &unit);
        bad[2].y = (jcv_real)(-10.5);
        expect_rejected(3, bad, &unit);
    }

    // Coincident seeds after periodic normalization.
    {
        jcv_point coincident[3] = {
            { (jcv_real)0.1, (jcv_real)0.2 },
            { (jcv_real)0.1, (jcv_real)0.2 },
            { (jcv_real)0.8, (jcv_real)0.7 },
        };
        expect_rejected(3, coincident, &unit);

        jcv_point wrapped[3] = {
            { (jcv_real)0.1, (jcv_real)0.2 },
            { (jcv_real)3.1, (jcv_real)0.2 },
            { (jcv_real)0.8, (jcv_real)0.7 },
        };
        expect_rejected(3, wrapped, &unit);

        double threshold = 1.0e-10;
        jcv_point near[3] = {
            { (jcv_real)0.1, (jcv_real)0.2 },
            { (jcv_real)(0.1 + 0.4 * threshold), (jcv_real)0.2 },
            { (jcv_real)0.8, (jcv_real)0.7 },
        };
        expect_rejected(3, near, &unit);
    }

    // 256 sites are accepted.
    {
        jcv_point many[256];
        jcv_rect big = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)8.0, (jcv_real)8.0 } };
        for( int i = 0; i < 256; ++i )
        {
            many[i].x = (jcv_real)(0.5 * (double)(i % 16) + 0.13);
            many[i].y = (jcv_real)(0.5 * (double)(i / 16) + 0.29);
        }
        jcv_periodic_diagram diagram;
        memset(&diagram, 0, sizeof(diagram));
        CHECK(jcv_periodic_generate(256, many, &big, &diagram) == 1);
        validate_diagram(&diagram, 256, many, &big, 0);
        jcv_periodic_free(&diagram);
    }
}

static void test_invalid_relax_and_free(void)
{
    jcv_rect rect = { { (jcv_real)0.0, (jcv_real)0.0 }, { (jcv_real)1.0, (jcv_real)1.0 } };
    jcv_point seeds[3] = {
        { (jcv_real)0.1, (jcv_real)0.1 },
        { (jcv_real)0.5, (jcv_real)0.6 },
        { (jcv_real)0.8, (jcv_real)0.3 },
    };
    jcv_periodic_diagram diagram;
    memset(&diagram, 0, sizeof(diagram));
    CHECK(jcv_periodic_generate(3, seeds, &rect, &diagram) == 1);

    jcv_point out[3];
    const jcv_real sentinel = (jcv_real)-1234.5;
    for( int i = 0; i < 3; ++i )
    {
        out[i].x = sentinel;
        out[i].y = sentinel;
    }

    CHECK(jcv_periodic_relax(&diagram, (jcv_real)-0.1, out) == 0);
    CHECK(jcv_periodic_relax(&diagram, (jcv_real)1.1, out) == 0);
    CHECK(jcv_periodic_relax(&diagram, (jcv_real)NAN, out) == 0);
    CHECK(jcv_periodic_relax(&diagram, (jcv_real)INFINITY, out) == 0);
    CHECK(jcv_periodic_relax(0, (jcv_real)0.5, out) == 0);
    CHECK(jcv_periodic_relax(&diagram, (jcv_real)0.5, 0) == 0);
    for( int i = 0; i < 3; ++i )
    {
        CHECK(out[i].x == sentinel);
        CHECK(out[i].y == sentinel);
    }

    // NULL and repeated releases are harmless.
    jcv_periodic_free(0);
    jcv_periodic_free(&diagram);
    CHECK(diagram_is_zeroed(&diagram));
    jcv_periodic_free(&diagram);
    CHECK(diagram_is_zeroed(&diagram));

    jcv_periodic_diagram zeroed;
    memset(&zeroed, 0, sizeof(zeroed));
    jcv_periodic_free(&zeroed);
    CHECK(diagram_is_zeroed(&zeroed));
}

int main(void)
{
    test_single_seed();
    test_two_seeds_offsets();
    test_asymmetric_four();
    test_collinear_and_grid();
    test_relax_cycle();
    test_boundary_and_range();
    test_translation();
    test_permutation();
    test_invalid_inputs();
    test_invalid_relax_and_free();

    printf("PERIODIC_OK assertions=%d\n", g_assertions);
    return 0;
}
