// Tests for par_shapes_orient_solid.  Built and run by task_support/run.sh;
// the driver in task_support/driver.c provides main() and task_check().

#include "task_support/check.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

// Allocation hooks let one test arm a budget so that allocations fail, and
// let every test verify that temporaries are released.  They must be defined
// before par_shapes.h so the implementation picks them up.
static int tst_alloc_budget = -1; // When >= 0, number of allocations to allow.
static long tst_alloc_live = 0;   // Number of live blocks from the hooks.

static void* tst_malloc(size_t n)
{
    if (tst_alloc_budget == 0) {
        return 0;
    }
    if (tst_alloc_budget > 0) {
        tst_alloc_budget--;
    }
    void* p = malloc(n ? n : 1);
    if (p) {
        tst_alloc_live++;
    }
    return p;
}

static void* tst_calloc(size_t n, size_t sz)
{
    if (tst_alloc_budget == 0) {
        return 0;
    }
    if (tst_alloc_budget > 0) {
        tst_alloc_budget--;
    }
    void* p = calloc(n ? n : 1, sz ? sz : 1);
    if (p) {
        tst_alloc_live++;
    }
    return p;
}

static void* tst_realloc(void* p, size_t n)
{
    if (!p) {
        return tst_malloc(n);
    }
    return realloc(p, n ? n : 1);
}

static void tst_free(void* p)
{
    if (p) {
        tst_alloc_live--;
    }
    free(p);
}

#define PAR_MALLOC(T, N) ((T*) tst_malloc((size_t) (N) * sizeof(T)))
#define PAR_CALLOC(T, N) ((T*) tst_calloc((size_t) (N), sizeof(T)))
#define PAR_REALLOC(T, BUF, N) ((T*) tst_realloc((BUF), (size_t) (N) * sizeof(T)))
#define PAR_FREE(BUF) tst_free(BUF)

#define PAR_SHAPES_IMPLEMENTATION
#include "par_shapes.h"

// ---------------------------------------------------------------------------
// Independent helpers: none of them call par_shapes_orient_solid.
// ---------------------------------------------------------------------------

// Signed volume enclosed by faces [f0, f1) under their current winding.
static double tst_volume(par_shapes_mesh const* m, int f0, int f1)
{
    double v = 0;
    for (int f = f0; f < f1; f++) {
        float const* a = m->points + 3 * m->triangles[3 * f];
        float const* b = m->points + 3 * m->triangles[3 * f + 1];
        float const* c = m->points + 3 * m->triangles[3 * f + 2];
        v += ((double) a[0] * (b[1] * c[2] - b[2] * c[1]) +
            (double) a[1] * (b[2] * c[0] - b[0] * c[2]) +
            (double) a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
    }
    return v;
}

// True if every directed half-edge has exactly one opposite partner, i.e.
// the mesh is closed and the two sides of every edge wind oppositely.
static int tst_edges_consistent(par_shapes_mesh const* m)
{
    for (int f = 0; f < m->ntriangles; f++) {
        for (int k = 0; k < 3; k++) {
            int a = m->triangles[3 * f + k];
            int b = m->triangles[3 * f + (k + 1) % 3];
            int fwd = 0, rev = 0;
            for (int g = 0; g < m->ntriangles; g++) {
                for (int j = 0; j < 3; j++) {
                    int c = m->triangles[3 * g + j];
                    int d = m->triangles[3 * g + (j + 1) % 3];
                    fwd += (c == a && d == b);
                    rev += (c == b && d == a);
                }
            }
            if (fwd != 1 || rev != 1) {
                return 0;
            }
        }
    }
    return 1;
}

// True if every undirected edge is shared by exactly two half-edges,
// regardless of their direction.
static int tst_edges_paired(par_shapes_mesh const* m)
{
    for (int f = 0; f < m->ntriangles; f++) {
        for (int k = 0; k < 3; k++) {
            int a = m->triangles[3 * f + k];
            int b = m->triangles[3 * f + (k + 1) % 3];
            int count = 0;
            for (int g = 0; g < m->ntriangles; g++) {
                for (int j = 0; j < 3; j++) {
                    int c = m->triangles[3 * g + j];
                    int d = m->triangles[3 * g + (j + 1) % 3];
                    count += (c == a && d == b) || (c == b && d == a);
                }
            }
            if (count != 2) {
                return 0;
            }
        }
    }
    return 1;
}

// Deep snapshot of all mesh arrays, used to verify that failed calls (and
// successful ones, where required) leave the input untouched.
typedef struct {
    int npoints, ntriangles, has_normals, has_tcoords;
    float* points;
    float* normals;
    float* tcoords;
    PAR_SHAPES_T* triangles;
} tst_snapshot;

static float* tst_copy_floats(float const* src, int n)
{
    float* dst = (float*) malloc(sizeof(float) * (size_t) n);
    memcpy(dst, src, sizeof(float) * (size_t) n);
    return dst;
}

static void tst_snap(tst_snapshot* s, par_shapes_mesh const* m)
{
    s->npoints = m->npoints;
    s->ntriangles = m->ntriangles;
    s->has_normals = m->normals != 0;
    s->has_tcoords = m->tcoords != 0;
    s->points = tst_copy_floats(m->points, 3 * m->npoints);
    s->normals = s->has_normals ? tst_copy_floats(m->normals, 3 * m->npoints) : 0;
    s->tcoords = s->has_tcoords ? tst_copy_floats(m->tcoords, 2 * m->npoints) : 0;
    s->triangles = (PAR_SHAPES_T*) malloc(sizeof(PAR_SHAPES_T) * 3 *
        (size_t) m->ntriangles);
    memcpy(s->triangles, m->triangles, sizeof(PAR_SHAPES_T) * 3 *
        (size_t) m->ntriangles);
}

static int tst_same(tst_snapshot const* s, par_shapes_mesh const* m)
{
    if (m->npoints != s->npoints || m->ntriangles != s->ntriangles) {
        return 0;
    }
    if ((m->normals != 0) != s->has_normals ||
        (m->tcoords != 0) != s->has_tcoords) {
        return 0;
    }
    if (memcmp(m->points, s->points, sizeof(float) * 3 * (size_t) s->npoints)) {
        return 0;
    }
    if (memcmp(m->triangles, s->triangles, sizeof(PAR_SHAPES_T) * 3 *
        (size_t) s->ntriangles)) {
        return 0;
    }
    if (s->has_normals && memcmp(m->normals, s->normals, sizeof(float) * 3 *
        (size_t) s->npoints)) {
        return 0;
    }
    if (s->has_tcoords && memcmp(m->tcoords, s->tcoords, sizeof(float) * 2 *
        (size_t) s->npoints)) {
        return 0;
    }
    return 1;
}

static void tst_snap_free(tst_snapshot* s)
{
    free(s->points);
    free(s->normals);
    free(s->tcoords);
    free(s->triangles);
}

static void tst_flip_face(par_shapes_mesh* m, int f)
{
    PAR_SHAPES_T* t = m->triangles + 3 * f;
    PAR_SHAPES_T tmp = t[1];
    t[1] = t[2];
    t[2] = tmp;
}

static void tst_flip_range(par_shapes_mesh* m, int f0, int f1)
{
    for (int f = f0; f < f1; f++) {
        tst_flip_face(m, f);
    }
}

// Closed, index-welded sphere (radius r) centered at (x, y, z), with no
// normals or texture coordinates.
static par_shapes_mesh* tst_sphere(float r, float x, float y, float z)
{
    par_shapes_mesh* m = par_shapes_create_subdivided_sphere(1);
    par_shapes_mesh* w = par_shapes_weld(m, 1e-3f, 0);
    par_shapes_free_mesh(m);
    par_shapes_scale(w, r, r, r);
    par_shapes_translate(w, x, y, z);
    return w;
}

// Regular tetrahedron with outward winding, built by hand.
static par_shapes_mesh* tst_tetra(void)
{
    static const float p[12] = {
        1, 1, 1,  1, -1, -1,  -1, 1, -1,  -1, -1, 1};
    static const PAR_SHAPES_T t[12] = {
        0, 1, 2,  0, 3, 1,  0, 2, 3,  1, 3, 2};
    par_shapes_mesh* m = PAR_CALLOC(par_shapes_mesh, 1);
    m->npoints = 4;
    m->ntriangles = 4;
    m->points = PAR_MALLOC(float, 12);
    memcpy(m->points, p, sizeof p);
    m->triangles = PAR_MALLOC(PAR_SHAPES_T, 12);
    memcpy(m->triangles, t, sizeof t);
    return m;
}

// Concave L-shaped prism with outward winding, built by hand: an L-shaped
// cross section in the XY plane extruded along Z from 0 to 1.
static par_shapes_mesh* tst_lprism(void)
{
    static const float xy[6][2] = {
        {0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}};
    static const PAR_SHAPES_T caps[24] = {
        0, 2, 1,  0, 3, 2,  0, 4, 3,  0, 5, 4,
        6, 7, 8,  6, 8, 9,  6, 9, 10, 6, 10, 11};
    par_shapes_mesh* m = PAR_CALLOC(par_shapes_mesh, 1);
    m->npoints = 12;
    m->ntriangles = 20;
    m->points = PAR_MALLOC(float, 36);
    for (int i = 0; i < 6; i++) {
        m->points[3 * i] = xy[i][0];
        m->points[3 * i + 1] = xy[i][1];
        m->points[3 * i + 2] = 0;
        m->points[3 * i + 18] = xy[i][0];
        m->points[3 * i + 19] = xy[i][1];
        m->points[3 * i + 20] = 1;
    }
    m->triangles = PAR_MALLOC(PAR_SHAPES_T, 60);
    memcpy(m->triangles, caps, sizeof caps);
    PAR_SHAPES_T* t = m->triangles + 24;
    for (int i = 0; i < 6; i++) {
        int j = (i + 1) % 6;
        *t++ = (PAR_SHAPES_T) i;
        *t++ = (PAR_SHAPES_T) j;
        *t++ = (PAR_SHAPES_T) (j + 6);
        *t++ = (PAR_SHAPES_T) i;
        *t++ = (PAR_SHAPES_T) (j + 6);
        *t++ = (PAR_SHAPES_T) (i + 6);
    }
    return m;
}

// Combinatorial projective plane: a twisted 3-quad Mobius strip whose
// boundary cycle is capped by a triangle fan.  Closed and non-orientable.
static par_shapes_mesh* tst_projective_plane(void)
{
    static const PAR_SHAPES_T bnd[6] = {0, 2, 4, 1, 3, 5};
    par_shapes_mesh* m = PAR_CALLOC(par_shapes_mesh, 1);
    m->npoints = 7;
    m->ntriangles = 12;
    m->points = PAR_MALLOC(float, 21);
    for (int i = 0; i < 3; i++) {
        double th = 2 * PAR_PI * i / 3;
        double cx = cos(th), sy = sin(th);
        double dx = cos(th / 2) * cx, dy = cos(th / 2) * sy, dz = sin(th / 2);
        for (int s = 0; s < 2; s++) {
            double w = s ? 0.3 : -0.3;
            m->points[3 * (2 * i + s)] = (float) (cx + w * dx);
            m->points[3 * (2 * i + s) + 1] = (float) (sy + w * dy);
            m->points[3 * (2 * i + s) + 2] = (float) (w * dz);
        }
    }
    m->points[18] = 0;
    m->points[19] = 0;
    m->points[20] = 3;
    m->triangles = PAR_MALLOC(PAR_SHAPES_T, 36);
    PAR_SHAPES_T* t = m->triangles;
    for (int i = 0; i < 3; i++) {
        int a = 2 * i;
        int b = 2 * i + 1;
        int c = i < 2 ? 2 * (i + 1) : 1;     // P(i+1, 0), with the twist
        int d = i < 2 ? 2 * (i + 1) + 1 : 0; // P(i+1, 1), with the twist
        *t++ = (PAR_SHAPES_T) a;
        *t++ = (PAR_SHAPES_T) c;
        *t++ = (PAR_SHAPES_T) d;
        *t++ = (PAR_SHAPES_T) a;
        *t++ = (PAR_SHAPES_T) d;
        *t++ = (PAR_SHAPES_T) b;
    }
    for (int k = 0; k < 6; k++) {
        *t++ = 6;
        *t++ = bnd[k];
        *t++ = bnd[(k + 1) % 6];
    }
    return m;
}

// Two tetrahedra sharing exactly one vertex: closed edges, but the vertex
// fan at the shared vertex is two disjoint rings.
static par_shapes_mesh* tst_pinched(void)
{
    static const float p[21] = {
        1, 1, 1,  1, -1, -1,  -1, 1, -1,  -1, -1, 1,
        3, 1, 1,  1, 3, 1,  1, 1, 3};
    static const PAR_SHAPES_T t[24] = {
        0, 1, 2,  0, 3, 1,  0, 2, 3,  1, 3, 2,
        0, 4, 5,  0, 6, 4,  0, 5, 6,  4, 6, 5};
    par_shapes_mesh* m = PAR_CALLOC(par_shapes_mesh, 1);
    m->npoints = 7;
    m->ntriangles = 8;
    m->points = PAR_MALLOC(float, 21);
    memcpy(m->points, p, sizeof p);
    m->triangles = PAR_MALLOC(PAR_SHAPES_T, 24);
    memcpy(m->triangles, t, sizeof t);
    return m;
}

// ---------------------------------------------------------------------------
// topology group
// ---------------------------------------------------------------------------

// A few locally flipped faces of a single shell are restored to the exact
// winding of the independently constructed reference.
static int case_scrambled_shell_restored(void)
{
    par_shapes_mesh* m = par_shapes_create_icosahedron();
    TASK_CHECK(m && m->ntriangles == 20);
    TASK_CHECK(tst_volume(m, 0, 20) > 0); // generator winds outward
    PAR_SHAPES_T ref[60];
    memcpy(ref, m->triangles, sizeof ref);
    tst_flip_face(m, 2);
    tst_flip_face(m, 5);
    tst_flip_face(m, 9);
    tst_flip_face(m, 13);
    tst_flip_face(m, 17);
    int depth[20];
    for (int i = 0; i < 20; i++) {
        depth[i] = -1;
    }
    TASK_CHECK(par_shapes_orient_solid(m, depth));
    TASK_CHECK(memcmp(m->triangles, ref, sizeof ref) == 0);
    for (int i = 0; i < 20; i++) {
        TASK_CHECK(depth[i] == 0);
    }
    TASK_CHECK(tst_edges_consistent(m));
    TASK_CHECK(par_shapes_orient_solid(m, 0)); // already correct: stable
    TASK_CHECK(memcmp(m->triangles, ref, sizeof ref) == 0);
    par_shapes_free_mesh(m);
    return 0;
}

// A shell whose every face is inverted is restored as a whole.
static int case_inverted_sphere_restored(void)
{
    par_shapes_mesh* m = tst_sphere(1, 0, 0, 0);
    TASK_CHECK(m->ntriangles == 80);
    PAR_SHAPES_T ref[240];
    memcpy(ref, m->triangles, sizeof ref);
    tst_flip_range(m, 0, 80);
    TASK_CHECK(tst_volume(m, 0, 80) < 0);
    TASK_CHECK(par_shapes_orient_solid(m, 0));
    TASK_CHECK(memcmp(m->triangles, ref, sizeof ref) == 0);
    TASK_CHECK(tst_volume(m, 0, 80) > 0);
    TASK_CHECK(tst_edges_consistent(m));
    par_shapes_free_mesh(m);
    return 0;
}

// Topology defects must be rejected without modifying the mesh.
static int case_invalid_topology_rejected(void)
{
    par_shapes_mesh* m = tst_pinched();
    TASK_CHECK(tst_edges_paired(m)); // closed, but the fan is two rings
    TASK_CHECK(!par_shapes_orient_solid(m, 0));
    par_shapes_free_mesh(m);
    m = tst_tetra();
    m->ntriangles = 3; // open boundary edges
    TASK_CHECK(!par_shapes_orient_solid(m, 0));
    m->ntriangles = 4;
    par_shapes_free_mesh(m);
    m = tst_tetra(); // edge shared by three faces
    m->npoints = 5;
    m->points = PAR_REALLOC(float, m->points, 15);
    m->points[12] = 1;
    m->points[13] = 1;
    m->points[14] = -3;
    m->ntriangles = 5;
    m->triangles = PAR_REALLOC(PAR_SHAPES_T, m->triangles, 15);
    m->triangles[12] = 0;
    m->triangles[13] = 1;
    m->triangles[14] = 4;
    TASK_CHECK(!par_shapes_orient_solid(m, 0));
    par_shapes_free_mesh(m);
    m = tst_tetra(); // duplicated face, opposite winding
    m->ntriangles = 5;
    m->triangles = PAR_REALLOC(PAR_SHAPES_T, m->triangles, 15);
    m->triangles[12] = 0;
    m->triangles[13] = 2;
    m->triangles[14] = 1;
    TASK_CHECK(!par_shapes_orient_solid(m, 0));
    par_shapes_free_mesh(m);
    m = tst_tetra(); // zero-area face (vertex 2 moved onto vertex 1)
    memcpy(m->points + 6, m->points + 3, 3 * sizeof(float));
    TASK_CHECK(!par_shapes_orient_solid(m, 0));
    par_shapes_free_mesh(m);
    m = tst_tetra(); // face with a repeated index
    m->triangles[2] = m->triangles[1];
    TASK_CHECK(!par_shapes_orient_solid(m, 0));
    par_shapes_free_mesh(m);
    m = tst_projective_plane(); // closed but not orientable
    TASK_CHECK(tst_edges_paired(m));
    TASK_CHECK(!par_shapes_orient_solid(m, 0));
    par_shapes_free_mesh(m);
    m = par_shapes_create_klein_bottle(24, 24); // non-orientable generator
    par_shapes_mesh* kw = par_shapes_weld(m, 1e-3f, 0);
    par_shapes_free_mesh(m);
    TASK_CHECK(!par_shapes_orient_solid(kw, 0));
    par_shapes_free_mesh(kw);
    return 0;
}

// Out-of-range indices, non-finite positions, and bad counts are rejected;
// an empty mesh is trivially accepted.
static int case_invalid_scalars_rejected(void)
{
    TASK_CHECK(!par_shapes_orient_solid(0, 0));
    par_shapes_mesh* m = tst_tetra();
    m->ntriangles = -1;
    TASK_CHECK(!par_shapes_orient_solid(m, 0));
    m->ntriangles = 4;
    m->npoints = -1;
    TASK_CHECK(!par_shapes_orient_solid(m, 0));
    m->npoints = 4;
    par_shapes_free_mesh(m);
    m = tst_tetra();
    m->triangles[5] = 4; // npoints is 4
    TASK_CHECK(!par_shapes_orient_solid(m, 0));
    par_shapes_free_mesh(m);
    m = tst_tetra();
    m->points[0] = NAN;
    TASK_CHECK(!par_shapes_orient_solid(m, 0));
    par_shapes_free_mesh(m);
    m = tst_tetra();
    m->points[11] = INFINITY;
    TASK_CHECK(!par_shapes_orient_solid(m, 0));
    par_shapes_free_mesh(m);
    m = par_shapes_create_empty();
    TASK_CHECK(par_shapes_orient_solid(m, 0));
    par_shapes_free_mesh(m);
    return 0;
}

// ---------------------------------------------------------------------------
// nesting group
// ---------------------------------------------------------------------------

// Three concentric shells get depths 0, 1, 2 and alternating volume signs.
static int case_three_nested_shells(void)
{
    par_shapes_mesh* m = tst_sphere(3, 0, 0, 0);
    par_shapes_mesh* mid = tst_sphere(2, 0, 0, 0);
    par_shapes_mesh* in = tst_sphere(1, 0, 0, 0);
    par_shapes_merge(m, mid);
    par_shapes_merge(m, in);
    par_shapes_free_mesh(mid);
    par_shapes_free_mesh(in);
    TASK_CHECK(m->ntriangles == 240);
    PAR_SHAPES_T first[240];
    for (int f = 0; f < 240; f++) {
        first[f] = m->triangles[3 * f];
    }
    tst_flip_range(m, 80, 160); // cavity wall wound like the outer shell
    tst_flip_face(m, 3);
    tst_flip_face(m, 50);
    tst_flip_face(m, 170);
    tst_flip_face(m, 200);
    int depth[240];
    TASK_CHECK(par_shapes_orient_solid(m, depth));
    for (int f = 0; f < 240; f++) {
        TASK_CHECK(m->triangles[3 * f] == first[f]); // first index stays
        TASK_CHECK(depth[f] == (f < 80 ? 0 : f < 160 ? 1 : 2));
    }
    TASK_CHECK(tst_volume(m, 0, 80) > 0);
    TASK_CHECK(tst_volume(m, 80, 160) < 0);
    TASK_CHECK(tst_volume(m, 160, 240) > 0);
    TASK_CHECK(tst_edges_consistent(m));
    PAR_SHAPES_T* copy = (PAR_SHAPES_T*) malloc(sizeof first * 3);
    memcpy(copy, m->triangles, sizeof first * 3);
    TASK_CHECK(par_shapes_orient_solid(m, depth)); // idempotent
    TASK_CHECK(memcmp(m->triangles, copy, sizeof first * 3) == 0);
    free(copy);
    par_shapes_free_mesh(m);
    return 0;
}

// Two disjoint concave shells, one fully inverted and one locally
// scrambled, are restored to their hand-built reference winding.
static int case_separate_concave_shells(void)
{
    par_shapes_mesh* a = tst_lprism();
    TASK_CHECK(tst_volume(a, 0, 20) > 0); // construction winds outward
    par_shapes_mesh* b = tst_lprism();
    par_shapes_scale(b, 1.5f, 1.5f, 1.5f);
    float axis[3] = {0.26726124f, 0.53452248f, 0.80178368f}; // (1,2,3)/|.|
    par_shapes_rotate(b, 0.7f, axis);
    par_shapes_translate(b, 10, -3, 2);
    par_shapes_merge(a, b);
    par_shapes_free_mesh(b);
    TASK_CHECK(a->ntriangles == 40);
    PAR_SHAPES_T ref[120];
    memcpy(ref, a->triangles, sizeof ref);
    tst_flip_range(a, 20, 40); // second shell fully inverted
    tst_flip_face(a, 1);       // first shell locally scrambled
    tst_flip_face(a, 4);
    tst_flip_face(a, 18);
    int depth[40];
    TASK_CHECK(par_shapes_orient_solid(a, depth));
    TASK_CHECK(memcmp(a->triangles, ref, sizeof ref) == 0);
    for (int f = 0; f < 40; f++) {
        TASK_CHECK(depth[f] == 0);
    }
    TASK_CHECK(tst_volume(a, 0, 20) > 0);
    TASK_CHECK(tst_volume(a, 20, 40) > 0);
    TASK_CHECK(tst_edges_consistent(a));
    par_shapes_free_mesh(a);
    return 0;
}

// A small shell inside the hole of a torus is not inside the torus: both
// stay at depth 0.  Enclosing everything in a big sphere makes the torus
// and the hole shell cavity walls (depth 1), while a far shell stays free.
static int case_shell_in_torus_hole(void)
{
    par_shapes_mesh* torus = par_shapes_create_torus(12, 24, 0.3f);
    par_shapes_mesh* m = par_shapes_weld(torus, 1e-3f, 0);
    par_shapes_free_mesh(torus);
    TASK_CHECK(m->ntriangles == 576);
    PAR_FREE(m->normals);
    m->normals = 0;
    TASK_CHECK(m->tcoords != 0); // kept to verify preservation
    par_shapes_mesh* hole = tst_sphere(0.3f, 0, 0, 0);
    par_shapes_mesh* far = tst_sphere(0.2f, 5, 0, 0);
    par_shapes_merge(m, hole);
    par_shapes_merge(m, far);
    par_shapes_free_mesh(hole);
    par_shapes_free_mesh(far);
    TASK_CHECK(m->ntriangles == 736);
    tst_snapshot snap;
    tst_snap(&snap, m);
    tst_flip_range(m, 0, 576); // torus fully inverted
    tst_flip_face(m, 600);
    tst_flip_face(m, 700);
    int depth[816];
    TASK_CHECK(par_shapes_orient_solid(m, depth));
    for (int f = 0; f < 736; f++) {
        TASK_CHECK(depth[f] == 0);
    }
    TASK_CHECK(tst_volume(m, 0, 576) > 0);
    TASK_CHECK(tst_volume(m, 576, 656) > 0);
    TASK_CHECK(tst_volume(m, 656, 736) > 0);
    TASK_CHECK(tst_edges_consistent(m));
    TASK_CHECK(m->npoints == snap.npoints && m->ntriangles == snap.ntriangles);
    TASK_CHECK(memcmp(m->points, snap.points, sizeof(float) * 3 *
        (size_t) snap.npoints) == 0);
    TASK_CHECK(m->tcoords != 0);
    TASK_CHECK(memcmp(m->tcoords, snap.tcoords, sizeof(float) * 2 *
        (size_t) snap.npoints) == 0);
    tst_snap_free(&snap);
    par_shapes_mesh* big = tst_sphere(3, 0, 0, 0);
    par_shapes_merge(m, big);
    par_shapes_free_mesh(big);
    TASK_CHECK(m->ntriangles == 816);
    tst_flip_face(m, 610);
    tst_flip_range(m, 736, 776);
    TASK_CHECK(par_shapes_orient_solid(m, depth));
    for (int f = 0; f < 656; f++) {
        TASK_CHECK(depth[f] == 1); // torus and hole shell are cavity walls
    }
    for (int f = 656; f < 816; f++) {
        TASK_CHECK(depth[f] == 0); // far shell and big shell are outermost
    }
    TASK_CHECK(tst_volume(m, 0, 576) < 0);
    TASK_CHECK(tst_volume(m, 576, 656) < 0);
    TASK_CHECK(tst_volume(m, 656, 736) > 0);
    TASK_CHECK(tst_volume(m, 736, 816) > 0);
    TASK_CHECK(tst_edges_consistent(m));
    par_shapes_free_mesh(m);
    return 0;
}

// ---------------------------------------------------------------------------
// publication group
// ---------------------------------------------------------------------------

// Orientation decisions are invariant under rigid motion, uniform scaling,
// and a permutation of the face order: scene B must end up with exactly
// the indices of scene A, modulo its face permutation.
static int case_rigid_transform_face_order(void)
{
    par_shapes_mesh* a = tst_lprism();
    par_shapes_mesh* s = tst_sphere(0.3f, 0.5f, 0.5f, 0.5f);
    par_shapes_mesh* t = tst_tetra();
    par_shapes_scale(t, 2, 2, 2);
    par_shapes_translate(t, 10, 0, 0);
    par_shapes_merge(a, s);
    par_shapes_merge(a, t);
    par_shapes_free_mesh(s);
    par_shapes_free_mesh(t);
    par_shapes_mesh* b = tst_lprism();
    s = tst_sphere(0.3f, 0.5f, 0.5f, 0.5f);
    t = tst_tetra();
    par_shapes_scale(t, 2, 2, 2);
    par_shapes_translate(t, 10, 0, 0);
    par_shapes_merge(b, s);
    par_shapes_merge(b, t);
    par_shapes_free_mesh(s);
    par_shapes_free_mesh(t);
    int nf = a->ntriangles;
    TASK_CHECK(nf == 104 && b->ntriangles == nf);
    for (int f = 0; f < nf; f++) { // identical scramble on both copies
        if ((f * 7 + 3) % 11 == 0) {
            tst_flip_face(a, f);
            tst_flip_face(b, f);
        }
    }
    tst_flip_range(a, 20, 100);
    tst_flip_range(b, 20, 100);
    for (int f = 0; f < nf / 2; f++) { // reverse the face order of B
        for (int k = 0; k < 3; k++) {
            PAR_SHAPES_T tmp = b->triangles[3 * f + k];
            b->triangles[3 * f + k] = b->triangles[3 * (nf - 1 - f) + k];
            b->triangles[3 * (nf - 1 - f) + k] = tmp;
        }
    }
    float axis[3] = {0.26726124f, 0.53452248f, 0.80178368f};
    par_shapes_rotate(b, 1.1f, axis);
    par_shapes_translate(b, 30, -20, 10);
    par_shapes_scale(b, 2.5f, 2.5f, 2.5f);
    int da[104], db[104];
    TASK_CHECK(par_shapes_orient_solid(a, da));
    TASK_CHECK(par_shapes_orient_solid(b, db));
    for (int f = 0; f < nf; f++) {
        int g = nf - 1 - f;
        TASK_CHECK(da[f] == db[g]);
        for (int k = 0; k < 3; k++) {
            TASK_CHECK(a->triangles[3 * f + k] == b->triangles[3 * g + k]);
        }
    }
    TASK_CHECK(tst_volume(a, 0, 20) > 0);    // concave shell outward
    TASK_CHECK(tst_volume(a, 20, 100) < 0);  // enclosed sphere is a cavity
    TASK_CHECK(tst_volume(a, 100, 104) > 0); // separate tetra outward
    TASK_CHECK(tst_edges_consistent(b));
    par_shapes_free_mesh(a);
    par_shapes_free_mesh(b);
    return 0;
}

// Existing normals are recomputed from the final winding (never carried
// over), unreferenced vertices get zero normals, and a mesh without normals
// keeps them null.
static int case_normals_recomputed(void)
{
    par_shapes_mesh* m = tst_tetra();
    m->npoints = 5; // vertex 4 is not referenced by any face
    m->points = PAR_REALLOC(float, m->points, 15);
    m->points[12] = 7;
    m->points[13] = 8;
    m->points[14] = 9;
    m->normals = PAR_MALLOC(float, 15);
    for (int i = 0; i < 15; i++) {
        m->normals[i] = 9.0f; // stale garbage that must not be reused
    }
    PAR_SHAPES_T ref[12];
    memcpy(ref, m->triangles, sizeof ref);
    tst_flip_face(m, 1);
    tst_flip_face(m, 2);
    TASK_CHECK(par_shapes_orient_solid(m, 0));
    TASK_CHECK(memcmp(m->triangles, ref, sizeof ref) == 0);
    TASK_CHECK(m->normals != 0);
    for (int v = 0; v < 4; v++) {
        float const* n = m->normals + 3 * v;
        float const* p = m->points + 3 * v;
        double len = sqrt(n[0] * (double) n[0] + n[1] * (double) n[1] +
            n[2] * (double) n[2]);
        double plen = sqrt(p[0] * (double) p[0] + p[1] * (double) p[1] +
            p[2] * (double) p[2]);
        double dot = (n[0] * (double) p[0] + n[1] * (double) p[1] +
            n[2] * (double) p[2]) / plen;
        TASK_CHECK(fabs(len - 1) < 1e-5);
        TASK_CHECK(dot > 0.99); // outward for this symmetric tetrahedron
    }
    TASK_CHECK(m->normals[12] == 0 && m->normals[13] == 0 &&
        m->normals[14] == 0);
    par_shapes_free_mesh(m);
    m = tst_tetra();
    tst_flip_face(m, 0);
    TASK_CHECK(par_shapes_orient_solid(m, 0));
    TASK_CHECK(m->normals == 0);
    par_shapes_free_mesh(m);
    return 0;
}

// Rejected input and allocation failure leave the mesh, its arrays, and
// face_depth untouched, and release all temporary memory.
static int case_failure_preserves_input(void)
{
    par_shapes_mesh* m = tst_tetra();
    m->normals = PAR_MALLOC(float, 12);
    m->tcoords = PAR_MALLOC(float, 8);
    for (int i = 0; i < 12; i++) {
        m->normals[i] = 0.5f;
    }
    for (int i = 0; i < 8; i++) {
        m->tcoords[i] = 0.25f;
    }
    m->triangles[7] = 9; // out of range
    tst_snapshot snap;
    tst_snap(&snap, m);
    int depth[4];
    for (int i = 0; i < 4; i++) {
        depth[i] = -777;
    }
    TASK_CHECK(!par_shapes_orient_solid(m, depth));
    TASK_CHECK(tst_same(&snap, m));
    for (int i = 0; i < 4; i++) {
        TASK_CHECK(depth[i] == -777);
    }
    tst_snap_free(&snap);
    par_shapes_free_mesh(m);
    m = tst_lprism();
    tst_flip_face(m, 3);
    tst_flip_face(m, 11);
    tst_snap(&snap, m);
    int budget;
    for (budget = 0; budget < 64; budget++) {
        tst_alloc_budget = budget;
        long before = tst_alloc_live;
        int ok = par_shapes_orient_solid(m, 0);
        TASK_CHECK(tst_alloc_live == before); // no leaked temporaries
        tst_alloc_budget = -1;
        if (ok) {
            break;
        }
        TASK_CHECK(tst_same(&snap, m));
    }
    TASK_CHECK(budget < 64); // a large enough budget eventually succeeds
    TASK_CHECK(tst_edges_consistent(m));
    tst_snap_free(&snap);
    par_shapes_free_mesh(m);
    return 0;
}

const struct task_case task_cases[] = {
    {"topology", "scrambled_shell_restored", case_scrambled_shell_restored},
    {"topology", "inverted_sphere_restored", case_inverted_sphere_restored},
    {"topology", "invalid_topology_rejected", case_invalid_topology_rejected},
    {"topology", "invalid_scalars_rejected", case_invalid_scalars_rejected},
    {"nesting", "three_nested_shells", case_three_nested_shells},
    {"nesting", "separate_concave_shells", case_separate_concave_shells},
    {"nesting", "shell_in_torus_hole", case_shell_in_torus_hole},
    {"publication", "rigid_transform_face_order",
        case_rigid_transform_face_order},
    {"publication", "normals_recomputed", case_normals_recomputed},
    {"publication", "failure_preserves_input", case_failure_preserves_input},
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
