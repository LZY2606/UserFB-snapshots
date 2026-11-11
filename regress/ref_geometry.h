/*
 * regress/ref_geometry.h
 *
 * Independent fill reference used by regress/fill_reference.c.
 *
 * The reference works exclusively from the test's own line segments and
 * cubic Bezier control points (stored as doubles).  It never calls into the
 * product's flattening, rasterization, containment or extents code; the only
 * thing shared with the product is the public canvas/path/surface API used by
 * the driver in fill_reference.c.
 *
 * Coverage is recovered from first principles: horizontal ray crossings are
 * solved analytically per scanline (bisection on monotone pieces bounded by
 * the cubic's y-extrema), directed crossings drive the non-zero rule and
 * transverse-crossing parity drives even-odd, and the resulting intervals are
 * integrated over pixel area.
 */
#ifndef REGRESS_REF_GEOMETRY_H
#define REGRESS_REF_GEOMETRY_H

#include <stdint.h>

typedef struct {
    double x, y;
} rpoint_t;

typedef enum {
    RSEG_LINE = 0,
    RSEG_CUBIC = 1
} rseg_type_t;

typedef struct {
    rseg_type_t type;
    rpoint_t p[4]; /* line: p0,p1  cubic: p0,p1,p2,p3 */
} rseg_t;

typedef struct {
    rseg_t* data;
    int size;
    int capacity;
    int closed; /* implicit closing segment back to the move-to point */
} rcontour_t;

typedef struct {
    double a, b, c, d, e, f;
} rmatrix_t;

#define RRULE_NON_ZERO 0
#define RRULE_EVEN_ODD 1

typedef struct {
    rcontour_t* contours;
    int size;
    int capacity;
    int rule;
    rmatrix_t matrix; /* user space -> device space, applied by the reference */
} rshape_t;

typedef struct {
    double lo;    /* lower coverage bound, 0..1 */
    double exact; /* best estimate of the ideal area fraction */
    double hi;    /* upper coverage bound, 0..1 */
} rcov_t;

void rcontour_init(rcontour_t* c);
void rcontour_destroy(rcontour_t* c);
void rcontour_move_to(rcontour_t* c, double x, double y);
void rcontour_line_to(rcontour_t* c, double x, double y);
void rcontour_cubic_to(rcontour_t* c, double x1, double y1,
    double x2, double y2, double x3, double y3);
void rcontour_close(rcontour_t* c);

void rshape_init(rshape_t* s, int rule);
void rshape_destroy(rshape_t* s);
rcontour_t* rshape_begin_contour(rshape_t* s);
void rshape_set_matrix(rshape_t* s, double a, double b, double c,
    double d, double e, double f);

rpoint_t rmatrix_map(const rmatrix_t* m, rpoint_t p);

/*
 * Renders one shape to a W x H coverage grid.
 *
 * subrows is the integration density (number of horizontal probes per pixel
 * row).  Every pixel is written, including fully covered and fully empty
 * ones.  When band_width > 0 the shape must contain curves; lo/hi are then
 * widened by the geometrically derived L-infinity error band of the boundary.
 * band_width == 0 is used for pure line scenes, where lo == exact == hi.
 */
void rshape_coverage(const rshape_t* s, int W, int H, int subrows,
    double band_width, rcov_t* grid);

/*
 * Geometric band variant.  An N x N sample lattice is placed inside every
 * pixel; each sample contributes to `lo` only when all four L-infinity
 * corners at distance band_width are inside the region, and to `hi` when any
 * corner is inside.  Ray crossings are solved once per distinct probe row.
 * The returned discretization margin is half a sample cell in device
 * pixels; callers add it to the geometric band they pass in.
 */
void rshape_coverage_banded(const rshape_t* s, int W, int H, int N,
    double band_width, rcov_t* grid, double* discretization);

/*
 * Forward-derived flattening/quantization band for every cubic in the shape,
 * replicating the product's 26.6 grid rounding and the FreeType smooth
 * rasterizer's cubic stop/subdivision rules with integer arithmetic.  Returns
 * the largest L-infinity deviation (device pixels) of the true curves from
 * the flattened chord polygon, plus the 1/64-grid rounding margin.
 */
double rshape_curve_band(const rshape_t* s);

/*
 * Exact horizontal-ray self test at an explicitly chosen scanline, including
 * a horizontal tangency.  Reports the winding/parity state at probe points so
 * callers can assert tangent bookkeeping (no parity flip, no winding change).
 */
void rshape_ray_state(const rshape_t* s, double y, double x,
    int* winding, int* parity);

/* Integer-rounded product coverage convention: a*b/255 truncated. */
int rcoverage_combine(int a, int b);

#endif /* REGRESS_REF_GEOMETRY_H */
