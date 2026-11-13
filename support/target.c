#include "checks.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#define PAR_SHAPES_IMPLEMENTATION
#include "par_shapes.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int nearly(float a, float b)
{
    float d = a - b;
    if (d < 0) {
        d = -d;
    }
    return d < 1e-4f;
}

static par_shapes_mesh* make_mesh(int np, int nt, float const* pts,
    PAR_SHAPES_T const* tris, float const* norms, float const* uvs)
{
    par_shapes_mesh* m = (par_shapes_mesh*) calloc(1, sizeof(par_shapes_mesh));
    m->npoints = np;
    m->ntriangles = nt;
    m->points = (float*) malloc(sizeof(float) * 3 * np);
    memcpy(m->points, pts, sizeof(float) * 3 * np);
    m->triangles = (PAR_SHAPES_T*) malloc(sizeof(PAR_SHAPES_T) * 3 * nt);
    memcpy(m->triangles, tris, sizeof(PAR_SHAPES_T) * 3 * nt);
    if (norms) {
        m->normals = (float*) malloc(sizeof(float) * 3 * np);
        memcpy(m->normals, norms, sizeof(float) * 3 * np);
    }
    if (uvs) {
        m->tcoords = (float*) malloc(sizeof(float) * 2 * np);
        memcpy(m->tcoords, uvs, sizeof(float) * 2 * np);
    }
    return m;
}

typedef struct {
    int lo;
    int hi;
    int dir;
} test_edge;

static int edge_cmp(const void* a, const void* b)
{
    test_edge const* x = (test_edge const*) a;
    test_edge const* y = (test_edge const*) b;
    if (x->lo != y->lo) {
        return x->lo - y->lo;
    }
    if (x->hi != y->hi) {
        return x->hi - y->hi;
    }
    return x->dir - y->dir;
}

// Merge the render copies by (vertex_source, vertex_layer), then require
// every merged undirected edge to have exactly two oppositely oriented faces.
static int manifold_after_merge(par_shapes_shell const* shell)
{
    par_shapes_mesh const* m = shell->mesh;
    int identities = 0;
    int* key_to_id = (int*) malloc(sizeof(int) * 2 * m->npoints);
    for (int i = 0; i < 2 * m->npoints; i++) {
        key_to_id[i] = -1;
    }
    int* merged = (int*) malloc(sizeof(int) * m->npoints);
    for (int i = 0; i < m->npoints; i++) {
        if (shell->vertex_source[i] < 0 ||
            shell->vertex_layer[i] < 0 || shell->vertex_layer[i] > 1) {
            free(key_to_id);
            free(merged);
            return 0;
        }
        int key = 2 * shell->vertex_source[i] + shell->vertex_layer[i];
        if (key_to_id[key] < 0) {
            key_to_id[key] = identities++;
        }
        merged[i] = key_to_id[key];
    }
    int nedge = 3 * m->ntriangles;
    test_edge* edges = (test_edge*) malloc(sizeof(test_edge) * nedge);
    for (int f = 0; f < m->ntriangles; f++) {
        for (int k = 0; k < 3; k++) {
            int a = merged[m->triangles[3 * f + k]];
            int b = merged[m->triangles[3 * f + (k + 1) % 3]];
            edges[3 * f + k].lo = a < b ? a : b;
            edges[3 * f + k].hi = a < b ? b : a;
            edges[3 * f + k].dir = a < b ? 1 : -1;
        }
    }
    qsort(edges, nedge, sizeof(test_edge), edge_cmp);
    int ok = 1;
    int e = 0;
    while (e < nedge) {
        int j = e + 1;
        while (j < nedge && edges[j].lo == edges[e].lo &&
            edges[j].hi == edges[e].hi) {
            j++;
        }
        if (j - e != 2 || edges[e].dir == edges[e + 1].dir) {
            ok = 0;
            break;
        }
        e = j;
    }
    free(edges);
    free(merged);
    free(key_to_id);
    return ok;
}

// Count connected components of the merged shell topology.
static int merged_components(par_shapes_shell const* shell)
{
    par_shapes_mesh const* m = shell->mesh;
    int nident = 0;
    int* key_to_id = (int*) malloc(sizeof(int) * 2 * m->npoints);
    for (int i = 0; i < 2 * m->npoints; i++) {
        key_to_id[i] = -1;
    }
    int* merged = (int*) malloc(sizeof(int) * m->npoints);
    for (int i = 0; i < m->npoints; i++) {
        int key = 2 * shell->vertex_source[i] + shell->vertex_layer[i];
        if (key_to_id[key] < 0) {
            key_to_id[key] = nident++;
        }
        merged[i] = key_to_id[key];
    }
    int* adj_count = (int*) calloc(nident, sizeof(int));
    for (int f = 0; f < m->ntriangles; f++) {
        for (int k = 0; k < 3; k++) {
            adj_count[merged[m->triangles[3 * f + k]]]++;
        }
    }
    int** adj = (int**) malloc(sizeof(int*) * nident);
    int* cursor = (int*) calloc(nident, sizeof(int));
    for (int i = 0; i < nident; i++) {
        // Two neighbors for every triangle that references the identity.
        adj[i] = (int*) malloc(sizeof(int) * 2 * adj_count[i]);
    }
    for (int f = 0; f < m->ntriangles; f++) {
        int ids[3];
        for (int k = 0; k < 3; k++) {
            ids[k] = merged[m->triangles[3 * f + k]];
        }
        for (int k = 0; k < 3; k++) {
            for (int q = 0; q < 3; q++) {
                if (q != k) {
                    adj[ids[k]][cursor[ids[k]]++] = ids[q];
                }
            }
        }
    }
    unsigned char* visited = (unsigned char*) calloc(nident, 1);
    int* stack = (int*) malloc(sizeof(int) * nident);
    int components = 0;
    for (int seed = 0; seed < nident; seed++) {
        if (visited[seed]) {
            continue;
        }
        components++;
        int sp = 0;
        stack[sp++] = seed;
        visited[seed] = 1;
        while (sp > 0) {
            int v = stack[--sp];
            for (int q = 0; q < adj_count[v]; q++) {
                int w = adj[v][q];
                if (!visited[w]) {
                    visited[w] = 1;
                    stack[sp++] = w;
                }
            }
        }
    }
    for (int i = 0; i < nident; i++) {
        free(adj[i]);
    }
    free(adj);
    free(cursor);
    free(adj_count);
    free(visited);
    free(stack);
    free(merged);
    free(key_to_id);
    return components;
}

// A wavy open grid with no normal array, so the shell must derive area
// weighted offset directions itself.
static par_shapes_mesh* make_wavy_grid(int cols, int rows)
{
    int np = (cols + 1) * (rows + 1);
    int nt = 2 * cols * rows;
    float* pts = (float*) malloc(sizeof(float) * 3 * np);
    PAR_SHAPES_T* tris = (PAR_SHAPES_T*) malloc(sizeof(PAR_SHAPES_T) * 3 * nt);
    for (int j = 0; j <= rows; j++) {
        for (int i = 0; i <= cols; i++) {
            float x = (float) i / cols - 0.5f;
            float y = (float) j / rows - 0.5f;
            float* p = pts + 3 * (j * (cols + 1) + i);
            p[0] = x;
            p[1] = y;
            p[2] = 0.35f * sinf(5.0f * x) * cosf(4.0f * y);
        }
    }
    int t = 0;
    for (int j = 0; j < rows; j++) {
        for (int i = 0; i < cols; i++) {
            int a = j * (cols + 1) + i;
            int b = a + 1;
            int c = a + cols + 1;
            int d = c + 1;
            tris[t++] = (PAR_SHAPES_T) a;
            tris[t++] = (PAR_SHAPES_T) b;
            tris[t++] = (PAR_SHAPES_T) c;
            tris[t++] = (PAR_SHAPES_T) b;
            tris[t++] = (PAR_SHAPES_T) d;
            tris[t++] = (PAR_SHAPES_T) c;
        }
    }
    par_shapes_mesh* m = make_mesh(np, nt, pts, tris, NULL, NULL);
    free(pts);
    free(tris);
    return m;
}

// Flat annulus in the XY plane: an outer loop and an inner hole.
static par_shapes_mesh* make_annulus(int slices)
{
    int np = 2 * slices;
    int nt = 2 * slices;
    float* pts = (float*) malloc(sizeof(float) * 3 * np);
    float* uvs = (float*) malloc(sizeof(float) * 2 * np);
    PAR_SHAPES_T* tris = (PAR_SHAPES_T*) malloc(sizeof(PAR_SHAPES_T) * 3 * nt);
    for (int i = 0; i < slices; i++) {
        float ang = 2.0f * (float) M_PI * i / slices;
        float ca = cosf(ang);
        float sa = sinf(ang);
        float* po = pts + 3 * i;
        float* pi = pts + 3 * (slices + i);
        po[0] = ca;
        po[1] = sa;
        po[2] = 0;
        pi[0] = 0.5f * ca;
        pi[1] = 0.5f * sa;
        pi[2] = 0;
        uvs[2 * i] = (float) i / slices;
        uvs[2 * i + 1] = 1.0f;
        uvs[2 * (slices + i)] = (float) i / slices;
        uvs[2 * (slices + i) + 1] = 0.0f;
    }
    int t = 0;
    for (int i = 0; i < slices; i++) {
        int j = (i + 1) % slices;
        int oi = i, oj = j, ii = slices + i, ij = slices + j;
        tris[t++] = (PAR_SHAPES_T) oi;
        tris[t++] = (PAR_SHAPES_T) oj;
        tris[t++] = (PAR_SHAPES_T) ij;
        tris[t++] = (PAR_SHAPES_T) oi;
        tris[t++] = (PAR_SHAPES_T) ij;
        tris[t++] = (PAR_SHAPES_T) ii;
    }
    par_shapes_mesh* m = make_mesh(np, nt, pts, tris, NULL, uvs);
    free(pts);
    free(uvs);
    free(tris);
    return m;
}

static void group_skins(void)
{
    par_shapes_mesh* m = par_shapes_create_hemisphere(10, 6);
    task_check(m != NULL, "hemisphere generator");
    float thick = 0.12f;
    par_shapes_shell* s = par_shapes_create_shell(m, thick);
    task_check(s != NULL, "non-planar hemisphere shell succeeds");
    par_shapes_mesh* h = s->mesh;
    task_check(h->normals != NULL && h->tcoords != NULL,
        "shell carries recomputed normals and copied UVs");
    // Offset directions vary per vertex and come from the unit source normals.
    int varying = 0;
    for (int i = 1; i < m->npoints; i++) {
        float const* n0 = m->normals;
        float const* ni = m->normals + 3 * i;
        if (!nearly(n0[0], ni[0]) || !nearly(n0[1], ni[1]) ||
            !nearly(n0[2], ni[2])) {
            varying = 1;
        }
    }
    task_check(varying, "hemisphere normals vary across vertices");
    for (int i = 0; i < m->npoints; i++) {
        float const* p = m->points + 3 * i;
        float const* n = m->normals + 3 * i;
        float const* qo = h->points + 3 * i;
        float const* qi = h->points + 3 * (m->npoints + i);
        task_check(nearly(qo[0], p[0] + 0.5f * thick * n[0]) &&
            nearly(qo[1], p[1] + 0.5f * thick * n[1]) &&
            nearly(qo[2], p[2] + 0.5f * thick * n[2]),
            "outer skin displaced along unit normal");
        task_check(nearly(qi[0], p[0] - 0.5f * thick * n[0]) &&
            nearly(qi[2], p[2] - 0.5f * thick * n[2]),
            "inner skin displaced opposite the normal");
        task_check(s->vertex_source[i] == i &&
            s->vertex_source[m->npoints + i] == i,
            "skin copies point back at their source vertices");
        task_check(s->vertex_layer[i] == 1 &&
            s->vertex_layer[m->npoints + i] == 0,
            "skin layer tags are outer=1 inner=0");
        // Recomputed normals must follow the actual offset triangle winding.
        task_check(h->normals[3 * i] * n[0] +
            h->normals[3 * i + 1] * n[1] +
            h->normals[3 * i + 2] * n[2] > 0.0f,
            "outer normal points along the offset direction");
        float const* ni2 = h->normals + 3 * (m->npoints + i);
        task_check(ni2[0] * n[0] + ni2[1] * n[1] + ni2[2] * n[2] < 0.0f,
            "inner normal points against the offset direction");
        task_check(nearly(h->tcoords[2 * i], m->tcoords[2 * i]) &&
            nearly(h->tcoords[2 * i + 1], m->tcoords[2 * i + 1]),
            "skin UVs preserve the source UVs");
    }
    task_check(manifold_after_merge(s),
        "merged hemisphere shell edges each have two opposite faces");
    par_shapes_free_shell(s);
    par_shapes_free_mesh(m);

    // Without source normals the direction must match the area weighted
    // facet normal accumulated at each vertex.
    par_shapes_mesh* w = make_wavy_grid(4, 4);
    task_check(w->normals == NULL, "wavy grid starts without normals");
    par_shapes_shell* sw = par_shapes_create_shell(w, 0.1f);
    task_check(sw != NULL, "non-planar surface without normals succeeds");
    par_shapes_mesh* hw = sw->mesh;
    int corner = 0;  // Single incident triangle, so its normal is exact.
    int face = -1;
    for (int f = 0; f < w->ntriangles; f++) {
        for (int k = 0; k < 3; k++) {
            if (w->triangles[3 * f + k] == corner) {
                face = f;
            }
        }
    }
    task_check(face >= 0, "corner vertex belongs to a triangle");
    float pa[3], pb[3], pc[3], ab[3], ac[3], nrm[3];
    int ia = w->triangles[3 * face];
    int ib = w->triangles[3 * face + 1];
    int ic = w->triangles[3 * face + 2];
    memcpy(pa, w->points + 3 * ia, sizeof(pa));
    memcpy(pb, w->points + 3 * ib, sizeof(pb));
    memcpy(pc, w->points + 3 * ic, sizeof(pc));
    for (int k = 0; k < 3; k++) {
        ab[k] = pb[k] - pa[k];
        ac[k] = pc[k] - pa[k];
    }
    nrm[0] = ab[1] * ac[2] - ab[2] * ac[1];
    nrm[1] = ab[2] * ac[0] - ab[0] * ac[2];
    nrm[2] = ab[0] * ac[1] - ab[1] * ac[0];
    float ln = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
    for (int k = 0; k < 3; k++) {
        nrm[k] /= ln;
    }
    for (int k = 0; k < 3; k++) {
        float disp = (hw->points[3 * corner + k] -
            w->points[3 * corner + k]) / 0.05f;
        task_check(nearly(disp, nrm[k]),
            "derived offset direction equals the facet normal");
    }
    task_check(manifold_after_merge(sw), "wavy grid shell is manifold");
    par_shapes_free_shell(sw);
    par_shapes_free_mesh(w);
}

static void group_rim(void)
{
    int slices = 12;
    par_shapes_mesh* m = make_annulus(slices);
    int nboundary = 2 * slices;
    par_shapes_shell* s = par_shapes_create_shell(m, 0.08f);
    task_check(s != NULL, "annulus with a hole produces a shell");
    par_shapes_mesh* h = s->mesh;
    int expect_points = 2 * m->npoints + 6 * nboundary;
    int expect_tris = 2 * m->ntriangles + 2 * nboundary;
    task_check(h->npoints == expect_points, "rim adds six vertices per edge");
    task_check(h->ntriangles == expect_tris, "rim adds two triangles per edge");
    int rim_base = 2 * m->npoints;
    int layer0 = 0, layer1 = 0;
    int u_zero = 0;
    int v_ok = 1, u_ok = 1;
    for (int i = rim_base; i < h->npoints; i++) {
        if (s->vertex_layer[i] == 0) {
            layer0++;
        } else {
            layer1++;
        }
        float u = h->tcoords[2 * i];
        float v = h->tcoords[2 * i + 1];
        if (u < -1e-6f || u > 1.0f + 1e-6f) {
            u_ok = 0;
        }
        if (!nearly(v, 0.0f) && !nearly(v, 1.0f)) {
            v_ok = 0;
        }
        if (nearly(u, 0.0f)) {
            u_zero++;
        }
    }
    task_check(layer0 == 3 * nboundary && layer1 == 3 * nboundary,
        "rim splits vertices evenly between inner and outer layers");
    task_check(v_ok, "rim V coordinate is 0 inner and 1 outer");
    task_check(u_ok, "rim U coordinate stays normalized to [0, 1]");
    // Two independent loops, each restarting arc length at zero.
    task_check(u_zero >= 4, "both boundary loops restart U at zero");
    // Every wall triangle has three private copies sharing one flat normal.
    int skin_tris = 2 * m->ntriangles;
    int flat_ok = 1;
    for (int f = skin_tris; f < h->ntriangles; f++) {
        float const* n0 = h->normals + 3 * h->triangles[3 * f];
        for (int k = 1; k < 3; k++) {
            float const* nk = h->normals + 3 * h->triangles[3 * f + k];
            if (!nearly(n0[0], nk[0]) || !nearly(n0[1], nk[1]) ||
                !nearly(n0[2], nk[2])) {
                flat_ok = 0;
            }
        }
    }
    task_check(flat_ok, "rim triangles use per-triangle flat normals");
    // On each circular loop the equal-length edges give equal U increments.
    // Gather the distinct outer-layer U values; the two loops share the same
    // normalized range, so expect 0, 1/n, ..., (n-1)/n.
    float us[256];
    int nu = 0;
    for (int i = rim_base; i < h->npoints; i++) {
        if (nearly(h->tcoords[2 * i + 1], 1.0f)) {
            float u = h->tcoords[2 * i];
            int known = 0;
            for (int q = 0; q < nu; q++) {
                if (nearly(us[q], u)) {
                    known = 1;
                    break;
                }
            }
            if (!known) {
                task_check(nu < 256, "distinct rim U count fits the buffer");
                us[nu++] = u;
            }
        }
    }
    for (int i = 0; i < nu; i++) {
        for (int j = i + 1; j < nu; j++) {
            if (us[j] < us[i]) {
                float tmp = us[i];
                us[i] = us[j];
                us[j] = tmp;
            }
        }
    }
    task_check(nu == slices + 1,
        "distinct rim U samples span the closed [0, 1] range");
    int arc_ok = nearly(us[0], 0.0f) && nearly(us[nu - 1], 1.0f);
    for (int i = 1; i < nu && arc_ok; i++) {
        if (!nearly(us[i] - us[i - 1], 1.0f / slices)) {
            arc_ok = 0;
        }
    }
    task_check(arc_ok, "rim U follows accumulated arc length");
    task_check(manifold_after_merge(s),
        "inner hole and outer rim are sealed with manifold edges");
    par_shapes_free_shell(s);
    par_shapes_free_mesh(m);
}

static void group_topology(void)
{
    // Closed input: two disconnected skins, no rim vertices at all.
    par_shapes_mesh* m = par_shapes_create_icosahedron();
    task_check(m != NULL && m->normals == NULL && m->tcoords == NULL,
        "icosahedron has neither normals nor UVs");
    par_shapes_shell* s = par_shapes_create_shell(m, 0.1f);
    task_check(s != NULL, "closed input shells successfully");
    par_shapes_mesh* h = s->mesh;
    task_check(h->npoints == 2 * m->npoints, "closed shell has no rim vertices");
    task_check(h->ntriangles == 2 * m->ntriangles, "closed shell duplicates faces");
    task_check(h->tcoords == NULL && h->normals != NULL,
        "missing source UVs stay missing, normals always computed");
    task_check(merged_components(s) == 2, "closed input gives two skin components");
    task_check(manifold_after_merge(s), "closed shell is manifold after merging");
    int identities = 0;
    for (int i = 0; i < h->npoints; i++) {
        int key = 2 * s->vertex_source[i] + s->vertex_layer[i];
        if (key >= identities) {
            identities = key + 1;
        }
    }
    task_check(identities == 2 * m->npoints,
        "one inner and one outer identity per source vertex");
    // Inner skin reverses the source winding; outer and inner faces of each
    // source triangle are emitted as an interleaved pair.
    int rev_ok = 1;
    for (int f = 0; f < m->ntriangles; f++) {
        int a = h->triangles[3 * (2 * f + 1)];
        int b = h->triangles[3 * (2 * f + 1) + 1];
        int c = h->triangles[3 * (2 * f + 1) + 2];
        int sa = m->triangles[3 * f], sb = m->triangles[3 * f + 1],
            sc = m->triangles[3 * f + 2];
        if (a != m->npoints + sa || b != m->npoints + sc ||
            c != m->npoints + sb) {
            rev_ok = 0;
        }
        int oa = h->triangles[3 * (2 * f)];
        int ob = h->triangles[3 * (2 * f) + 1];
        int oc = h->triangles[3 * (2 * f) + 2];
        if (oa != sa || ob != sb || oc != sc) {
            rev_ok = 0;  // Outer skin preserves the source winding exactly.
        }
    }
    task_check(rev_ok,
        "outer skin keeps and inner skin reverses the source winding");
    par_shapes_free_shell(s);
    par_shapes_free_mesh(m);

    // Two disconnected open components each become their own closed shell.
    par_shapes_mesh* p0 = par_shapes_create_plane(2, 2);
    par_shapes_mesh* p1 = par_shapes_create_plane(2, 2);
    par_shapes_translate(p1, 5.0f, 0.0f, 0.0f);
    par_shapes_merge(p0, p1);
    par_shapes_free_mesh(p1);
    par_shapes_shell* sp = par_shapes_create_shell(p0, 0.05f);
    task_check(sp != NULL, "multi-component open surface shells");
    task_check(manifold_after_merge(sp), "multi-component shell is manifold");
    task_check(merged_components(sp) == 2, "each component seals independently");
    par_shapes_free_shell(sp);
    par_shapes_free_mesh(p0);

    // The disk generator has no normals or UVs and still produces a shell.
    par_shapes_mesh* d = par_shapes_create_disk(1.0f, 12,
        (float[3]) {0, 0, 0}, (float[3]) {0, 0, 1});
    par_shapes_shell* sd = par_shapes_create_shell(d, 0.1f);
    task_check(sd != NULL && sd->mesh->tcoords == NULL,
        "normal-less UV-less open disk still shells without UVs");
    par_shapes_free_shell(sd);
    par_shapes_free_mesh(d);
}

static void group_reject(void)
{
    // Zero-area triangle.
    {
        float pts[9] = {0, 0, 0, 1, 0, 0, 2, 0, 0};
        PAR_SHAPES_T tris[3] = {0, 1, 2};
        par_shapes_mesh* m = make_mesh(3, 1, pts, tris, NULL, NULL);
        task_check(par_shapes_create_shell(m, 0.1f) == NULL,
            "zero-area triangle rejected");
        par_shapes_free_mesh(m);
    }
    // Illegal index.
    {
        float pts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        PAR_SHAPES_T tris[3] = {0, 1, 9};
        par_shapes_mesh* m = make_mesh(3, 1, pts, tris, NULL, NULL);
        task_check(par_shapes_create_shell(m, 0.1f) == NULL,
            "out-of-range index rejected");
        par_shapes_free_mesh(m);
    }
    // Same-direction shared edge.
    {
        float pts[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
        PAR_SHAPES_T tris[6] = {0, 1, 2, 0, 1, 3};
        par_shapes_mesh* m = make_mesh(4, 2, pts, tris, NULL, NULL);
        task_check(par_shapes_create_shell(m, 0.1f) == NULL,
            "same-direction shared edge rejected");
        par_shapes_free_mesh(m);
    }
    // Three faces sharing one edge.
    {
        float pts[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
        PAR_SHAPES_T tris[9] = {0, 1, 2, 0, 1, 3, 0, 1, 2};
        par_shapes_mesh* m = make_mesh(4, 3, pts, tris, NULL, NULL);
        task_check(par_shapes_create_shell(m, 0.1f) == NULL,
            "edge shared by three faces rejected");
        par_shapes_free_mesh(m);
    }
    // Point contact: two face fans touching at a single vertex.
    {
        float pts[15] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 2, 0, 1};
        PAR_SHAPES_T tris[6] = {0, 1, 2, 0, 3, 4};
        par_shapes_mesh* m = make_mesh(5, 2, pts, tris, NULL, NULL);
        task_check(par_shapes_create_shell(m, 0.1f) == NULL,
            "point-contact multiple fans rejected");
        par_shapes_free_mesh(m);
    }
    // Non-finite coordinate and a vertex with no definable direction.
    {
        float pts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        PAR_SHAPES_T tris[3] = {0, 1, 2};
        pts[0] = NAN;
        par_shapes_mesh* m = make_mesh(3, 1, pts, tris, NULL, NULL);
        task_check(par_shapes_create_shell(m, 0.1f) == NULL,
            "non-finite coordinate rejected");
        par_shapes_free_mesh(m);
        float pts2[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 2, 2, 2};
        PAR_SHAPES_T tris2[3] = {0, 1, 2};
        par_shapes_mesh* m2 = make_mesh(4, 1, pts2, tris2, NULL, NULL);
        task_check(par_shapes_create_shell(m2, 0.1f) == NULL,
            "unreferenced vertex with no direction rejected");
        par_shapes_free_mesh(m2);
    }
    // Provided normals: zero vector is unusable, NaN is non-finite.
    {
        float pts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        PAR_SHAPES_T tris[3] = {0, 1, 2};
        float norms[9] = {0, 0, 0, 0, 0, 1, 0, 0, 1};
        par_shapes_mesh* m = make_mesh(3, 1, pts, tris, norms, NULL);
        task_check(par_shapes_create_shell(m, 0.1f) == NULL,
            "zero-length provided normal rejected");
        par_shapes_free_mesh(m);
        norms[0] = NAN;
        m = make_mesh(3, 1, pts, tris, norms, NULL);
        task_check(par_shapes_create_shell(m, 0.1f) == NULL,
            "non-finite provided normal rejected");
        par_shapes_free_mesh(m);
    }
    // Tangential offset degeneracy: all normals run along one edge, so the
    // wall quad for that edge collapses to a line.
    {
        float pts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        PAR_SHAPES_T tris[3] = {0, 1, 2};
        float norms[9] = {1, 0, 0, 1, 0, 0, 1, 0, 0};
        par_shapes_mesh* m = make_mesh(3, 1, pts, tris, norms, NULL);
        task_check(par_shapes_create_shell(m, 0.2f) == NULL,
            "tangential normal collapsing a rim wall rejected");
        par_shapes_free_mesh(m);
    }
    // Bad arguments, and the source must remain untouched on failure.
    {
        task_check(par_shapes_create_shell(NULL, 0.1f) == NULL,
            "null source rejected");
        par_shapes_mesh* m = par_shapes_create_plane(2, 2);
        task_check(par_shapes_create_shell(m, 0.0f) == NULL &&
            par_shapes_create_shell(m, -1.0f) == NULL &&
            par_shapes_create_shell(m, NAN) == NULL,
            "non-positive or non-finite thickness rejected");
        float saved[3];
        memcpy(saved, m->points, sizeof(saved));
        PAR_SHAPES_T saved_tri[3];
        memcpy(saved_tri, m->triangles, sizeof(saved_tri));
        task_check(par_shapes_create_shell(m, 0.0f) == NULL &&
            memcmp(saved, m->points, sizeof(saved)) == 0 &&
            memcmp(saved_tri, m->triangles, sizeof(saved_tri)) == 0,
            "failed call leaves the source mesh unchanged");
        par_shapes_free_mesh(m);
    }
    // Index-width capacity: the expanded shell must fit PAR_SHAPES_T.
    {
        int n = 182;
        int np = n * n;
        int nt = 2 * (n - 1) * (n - 1);
        float* pts = (float*) malloc(sizeof(float) * 3 * np);
        PAR_SHAPES_T* tris = (PAR_SHAPES_T*)
            malloc(sizeof(PAR_SHAPES_T) * 3 * nt);
        for (int j = 0; j < n; j++) {
            for (int i = 0; i < n; i++) {
                float* p = pts + 3 * (j * n + i);
                p[0] = (float) i / n;
                p[1] = (float) j / n;
                p[2] = 0.05f * sinf(i) * cosf(j);
            }
        }
        int t = 0;
        for (int j = 0; j < n - 1; j++) {
            for (int i = 0; i < n - 1; i++) {
                int a = j * n + i, b = a + 1, c = a + n, d = c + 1;
                tris[t++] = (PAR_SHAPES_T) a;
                tris[t++] = (PAR_SHAPES_T) b;
                tris[t++] = (PAR_SHAPES_T) c;
                tris[t++] = (PAR_SHAPES_T) b;
                tris[t++] = (PAR_SHAPES_T) d;
                tris[t++] = (PAR_SHAPES_T) c;
            }
        }
        par_shapes_mesh* m = make_mesh(np, nt, pts, tris, NULL, NULL);
        free(pts);
        free(tris);
        par_shapes_shell* s = par_shapes_create_shell(m, 0.01f);
        if (sizeof(PAR_SHAPES_T) < 4) {
            task_check(s == NULL,
                "16-bit indices reject a shell past 65535 vertices");
        } else {
            task_check(s != NULL && manifold_after_merge(s),
                "32-bit indices accept the same large shell");
        }
        if (s) {
            par_shapes_free_shell(s);
        }
        par_shapes_free_mesh(m);
    }
    // Ordinary non-planar, non-degenerate surfaces must still succeed.
    {
        par_shapes_mesh* m = make_wavy_grid(6, 5);
        par_shapes_shell* s = par_shapes_create_shell(m, 0.05f);
        task_check(s != NULL && manifold_after_merge(s),
            "ordinary non-planar surface shells successfully");
        par_shapes_free_shell(s);
        par_shapes_free_mesh(m);
        par_shapes_mesh* h = par_shapes_create_hemisphere(8, 4);
        par_shapes_shell* sh = par_shapes_create_shell(h, 0.1f);
        task_check(sh != NULL, "ordinary curved open surface succeeds");
        par_shapes_free_shell(sh);
        par_shapes_free_mesh(h);
    }
}

void task_cases(const char* group)
{
    if (strcmp(group, "skins") == 0) {
        group_skins();
    } else if (strcmp(group, "rim") == 0) {
        group_rim();
    } else if (strcmp(group, "topology") == 0) {
        group_topology();
    } else if (strcmp(group, "reject") == 0) {
        group_reject();
    } else {
        task_check(0, "unknown group");
    }
}
