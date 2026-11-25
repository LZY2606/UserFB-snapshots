// Targeted checks for par_shapes_create_shell.  This translation unit owns the
// implementation; support/driver.c provides main() and task_check().
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PAR_SHAPES_IMPLEMENTATION
#include "par_shapes.h"
#include "checks.h"

static par_shapes_mesh* make_mesh(int nv, float const* pts, int nf,
    int const* tris, float const* norms, float const* uv)
{
    par_shapes_mesh* m = calloc(1, sizeof(par_shapes_mesh));
    m->npoints = nv;
    m->points = malloc(sizeof(float) * 3 * nv);
    memcpy(m->points, pts, sizeof(float) * 3 * nv);
    m->ntriangles = nf;
    m->triangles = malloc(sizeof(PAR_SHAPES_T) * 3 * nf);
    for (int i = 0; i < 3 * nf; i++) {
        m->triangles[i] = (PAR_SHAPES_T) tris[i];
    }
    if (norms) {
        m->normals = malloc(sizeof(float) * 3 * nv);
        memcpy(m->normals, norms, sizeof(float) * 3 * nv);
    }
    if (uv) {
        m->tcoords = malloc(sizeof(float) * 2 * nv);
        memcpy(m->tcoords, uv, sizeof(float) * 2 * nv);
    }
    return m;
}

static void free_raw(par_shapes_mesh* m)
{
    par_shapes_free_mesh(m);
}

// Square annulus: four outer and four inner corners joined by a ring of quads.
// It has two boundary loops and one hole.
static par_shapes_mesh* make_annulus(float z)
{
    float pts[24] = {
        2, 0, z, 2, 2, z, 0, 2, z, 0, 0, z,
        1.5f, 0.5f, z, 1.5f, 1.5f, z, 0.5f, 1.5f, z, 0.5f, 0.5f, z,
    };
    int tris[24];
    int k = 0;
    for (int i = 0; i < 4; i++) {
        int o0 = i, o1 = (i + 1) % 4, i0 = 4 + i, i1 = 4 + (i + 1) % 4;
        tris[k++] = o0;
        tris[k++] = o1;
        tris[k++] = i1;
        tris[k++] = o0;
        tris[k++] = i1;
        tris[k++] = i0;
    }
    float uv[16];
    for (int i = 0; i < 8; i++) {
        uv[2 * i] = pts[3 * i];
        uv[2 * i + 1] = pts[3 * i + 1];
    }
    return make_mesh(8, pts, 8, tris, 0, uv);
}

// Two disconnected unit quads in the z=0 plane.
static par_shapes_mesh* make_two_quads(void)
{
    float pts[24] = {
        0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0,
        3, 0, 0, 4, 0, 0, 4, 1, 0, 3, 1, 0,
    };
    int tris[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    float uv[16];
    for (int i = 0; i < 8; i++) {
        uv[2 * i] = pts[3 * i];
        uv[2 * i + 1] = pts[3 * i + 1];
    }
    return make_mesh(8, pts, 4, tris, 0, uv);
}

static void normalize3(float* v)
{
    float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 0) {
        v[0] /= len;
        v[1] /= len;
        v[2] /= len;
    }
}

static int same3(float const* a, float const* b, float eps)
{
    return fabsf(a[0] - b[0]) <= eps && fabsf(a[1] - b[1]) <= eps &&
        fabsf(a[2] - b[2]) <= eps;
}

// Recover the boundary slots in the same order the implementation does:
// directed edges with no opposite, scanning slots in ascending order.
static int boundary_slots(par_shapes_mesh const* m, int* bnd)
{
    int np = 0;
    for (int s = 0; s < 3 * m->ntriangles; s++) {
        int f = s / 3, k = s % 3;
        int a = (int) m->triangles[3 * f + k];
        int b = (int) m->triangles[3 * f + (k + 1) % 3];
        int twin = 0;
        for (int t = 0; t < m->ntriangles && !twin; t++) {
            for (int q = 0; q < 3; q++) {
                if ((int) m->triangles[3 * t + q] == b &&
                    (int) m->triangles[3 * t + (q + 1) % 3] == a) {
                    twin = 1;
                    break;
                }
            }
        }
        if (!twin) {
            bnd[np++] = s;
        }
    }
    return np;
}

static float edge_len(par_shapes_mesh const* m, int slot)
{
    int a = (int) m->triangles[slot];
    int b = (int) m->triangles[3 * (slot / 3) + (slot % 3 + 1) % 3];
    float dx = m->points[3 * b + 0] - m->points[3 * a + 0];
    float dy = m->points[3 * b + 1] - m->points[3 * a + 1];
    float dz = m->points[3 * b + 2] - m->points[3 * a + 2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

// Verify every merged (vertex_source, vertex_layer) edge has exactly one
// opposite face.  Returns the number of violations.
static int merged_edge_violations(par_shapes_mesh const* m, int const* src,
    int const* lay)
{
    int np = m->npoints, nf = m->ntriangles;
    int maxsrc = 0;
    for (int i = 0; i < np; i++) {
        if (src[i] > maxsrc) {
            maxsrc = src[i];
        }
    }
    int* map = malloc(sizeof(int) * (size_t) (2 * (maxsrc + 1)));
    for (int i = 0; i < 2 * (maxsrc + 1); i++) {
        map[i] = -1;
    }
    int ids = 0;
    for (int i = 0; i < np; i++) {
        int key = src[i] * 2 + lay[i];
        if (map[key] < 0) {
            map[key] = ids++;
        }
    }
    int ne = 3 * nf;
    int* ea = malloc(sizeof(int) * ne);
    int* eb = malloc(sizeof(int) * ne);
    for (int f = 0; f < nf; f++) {
        for (int k = 0; k < 3; k++) {
            int a = map[src[m->triangles[3 * f + k]] * 2 +
                lay[m->triangles[3 * f + k]]];
            int b = map[src[m->triangles[3 * f + (k + 1) % 3]] * 2 +
                lay[m->triangles[3 * f + (k + 1) % 3]]];
            ea[3 * f + k] = a;
            eb[3 * f + k] = b;
        }
    }
    int bad = 0;
    for (int i = 0; i < ne; i++) {
        int total = 0, rev = 0;
        for (int j = 0; j < ne; j++) {
            int same = (ea[i] == ea[j] && eb[i] == eb[j]) ||
                (ea[i] == eb[j] && eb[i] == ea[j]);
            if (same) {
                total++;
                if (ea[j] == eb[i] && eb[j] == ea[i]) {
                    rev++;
                }
            }
        }
        if (total != 2 || rev != 1) {
            bad++;
        }
    }
    free(map);
    free(ea);
    free(eb);
    return bad;
}

static int dsu_find(int* p, int i)
{
    while (p[i] != i) {
        p[i] = p[p[i]];
        i = p[i];
    }
    return i;
}

static void dsu_union(int* p, int a, int b)
{
    a = dsu_find(p, a);
    b = dsu_find(p, b);
    if (a != b) {
        p[a] = b;
    }
}

// Euler characteristic of the merged shell, plus merged connectivity.
static int merged_euler(par_shapes_mesh const* m, int const* src,
    int const* lay, int* components, int* boundary_edges)
{
    int np = m->npoints, nf = m->ntriangles;
    int maxsrc = 0;
    for (int i = 0; i < np; i++) {
        if (src[i] > maxsrc) {
            maxsrc = src[i];
        }
    }
    int* map = malloc(sizeof(int) * (size_t) (2 * (maxsrc + 1)));
    for (int i = 0; i < 2 * (maxsrc + 1); i++) {
        map[i] = -1;
    }
    int ids = 0;
    for (int i = 0; i < np; i++) {
        int key = src[i] * 2 + lay[i];
        if (map[key] < 0) {
            map[key] = ids++;
        }
    }
    int* parent = malloc(sizeof(int) * ids);
    int* used = calloc(ids, sizeof(int));
    for (int i = 0; i < ids; i++) {
        parent[i] = i;
    }
    int ne = 3 * nf;
    int* ea = malloc(sizeof(int) * ne);
    int* eb = malloc(sizeof(int) * ne);
    for (int f = 0; f < nf; f++) {
        for (int k = 0; k < 3; k++) {
            int va = m->triangles[3 * f + k];
            int vb = m->triangles[3 * f + (k + 1) % 3];
            int a = map[src[va] * 2 + lay[va]];
            int b = map[src[vb] * 2 + lay[vb]];
            ea[3 * f + k] = a;
            eb[3 * f + k] = b;
            used[a] = used[b] = 1;
            dsu_union(parent, a, b);
        }
    }
    int unique = 0, open = 0;
    for (int i = 0; i < ne; i++) {
        int first = 1;
        for (int j = 0; j < i; j++) {
            if ((ea[j] == ea[i] && eb[j] == eb[i]) ||
                (ea[j] == eb[i] && eb[j] == ea[i])) {
                first = 0;
                break;
            }
        }
        if (first) {
            unique++;
        }
        int twin = 0;
        for (int j = 0; j < ne; j++) {
            if (ea[j] == eb[i] && eb[j] == ea[i]) {
                twin = 1;
                break;
            }
        }
        if (!twin) {
            open++;
        }
    }
    int comps = 0;
    for (int i = 0; i < ids; i++) {
        if (used[i] && dsu_find(parent, i) == i) {
            comps++;
        }
    }
    *components = comps;
    *boundary_edges = open;
    int v = 0;
    for (int i = 0; i < ids; i++) {
        v += used[i] ? 1 : 0;
    }
    free(map);
    free(parent);
    free(used);
    free(ea);
    free(eb);
    return v - unique + nf;
}

static void check_skin_offsets(par_shapes_mesh const* source,
    par_shapes_shell const* shell, float thickness)
{
    int nv = source->npoints;
    float half = thickness * 0.5f;
    par_shapes_mesh* reference = par_shapes_clone(source, 0);
    par_shapes_compute_normals(reference);
    int outer_ok = 1, inner_ok = 1;
    for (int p = 0; p < nv; p++) {
        float n[3] = {reference->normals[3 * p], reference->normals[3 * p + 1],
            reference->normals[3 * p + 2]};
        if (source->normals) {
            n[0] = source->normals[3 * p];
            n[1] = source->normals[3 * p + 1];
            n[2] = source->normals[3 * p + 2];
        }
        normalize3(n);
        float want[3], got[3];
        for (int k = 0; k < 3; k++) {
            want[k] = source->points[3 * p + k] + half * n[k];
            got[k] = shell->mesh->points[3 * (nv + p) + k];
        }
        if (!same3(want, got, 1e-4f)) {
            outer_ok = 0;
        }
        for (int k = 0; k < 3; k++) {
            want[k] = source->points[3 * p + k] - half * n[k];
            got[k] = shell->mesh->points[3 * p + k];
        }
        if (!same3(want, got, 1e-4f)) {
            inner_ok = 0;
        }
    }
    task_check(outer_ok, "outer skin offset along unit normals");
    task_check(inner_ok, "inner skin offset against unit normals");
    par_shapes_free_mesh(reference);
}

static void group_skins(void)
{
    par_shapes_mesh* sphere = par_shapes_create_parametric_sphere(8, 5);
    par_shapes_mesh* copy = par_shapes_clone(sphere, 0);
    par_shapes_shell* sh = par_shapes_create_shell(sphere, 0.5f);
    task_check(sh != 0, "curved sphere produces a shell");
    if (sh) {
        int nv = sphere->npoints;
        task_check(sh->mesh->npoints > 2 * nv, "curved open source gains rim verts");
        check_skin_offsets(sphere, sh, 0.5f);
        int unit = 1;
        for (int i = 0; i < 2 * nv; i++) {
            float n[3] = {sh->mesh->normals[3 * i], sh->mesh->normals[3 * i + 1],
                sh->mesh->normals[3 * i + 2]};
            float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (fabsf(len - 1.0f) > 1e-4f) {
                unit = 0;
            }
        }
        task_check(unit, "all skin normals are unit length");
        // The offset direction should follow the per-vertex normals, so two
        // different vertices must see different unit offsets.
        int varies = 0;
        for (int p = 1; p < nv; p++) {
            float d0[3], d1[3];
            for (int k = 0; k < 3; k++) {
                d0[k] = sh->mesh->points[3 * (nv + 0) + k] -
                    sphere->points[3 * 0 + k];
                d1[k] = sh->mesh->points[3 * (nv + p) + k] -
                    sphere->points[3 * p + k];
            }
            normalize3(d0);
            normalize3(d1);
            if (!same3(d0, d1, 1e-3f)) {
                varies = 1;
                break;
            }
        }
        task_check(varies, "offset direction varies per vertex on a curved surface");
        int uv_ok = 1;
        for (int i = 0; i < 2 * nv; i++) {
            int src = sh->vertex_source[i] % nv;
            if (sh->mesh->tcoords[2 * i] != sphere->tcoords[2 * src] ||
                sh->mesh->tcoords[2 * i + 1] != sphere->tcoords[2 * src + 1]) {
                uv_ok = 0;
            }
        }
        task_check(uv_ok, "skins keep the source texture coordinates");
        task_check(sh->mesh->tcoords != 0,
            "shell exposes tcoords when source has them");
        par_shapes_free_shell(sh);
    }
    int unchanged = 1;
    for (int i = 0; i < 3 * sphere->npoints; i++) {
        if (sphere->points[i] != copy->points[i]) {
            unchanged = 0;
        }
    }
    task_check(unchanged, "source mesh is not modified");
    par_shapes_free_mesh(copy);
    par_shapes_free_mesh(sphere);

    // No source normals: the direction comes from area-weighted normals.
    par_shapes_mesh* icosa = par_shapes_create_icosahedron();
    task_check(icosa->normals == 0, "icosahedron has no source normals");
    par_shapes_shell* ish = par_shapes_create_shell(icosa, 0.3f);
    task_check(ish != 0, "closed source without normals still builds a shell");
    if (ish) {
        check_skin_offsets(icosa, ish, 0.3f);
        task_check(ish->mesh->npoints == 2 * icosa->npoints,
            "closed input yields only the two skins");
        task_check(ish->mesh->tcoords == 0, "missing source UV yields no tcoords");
        par_shapes_free_shell(ish);
    }
    par_shapes_free_mesh(icosa);

    // Supplied normals are not copied onto the skins: bend two triangles but
    // label every vertex with +Z.  Re-accumulation from the offset geometry
    // must produce tilted normals.
    {
        float pts[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0.5f};
        int tris[6] = {0, 1, 2, 1, 3, 2};
        float nrm[12];
        for (int i = 0; i < 4; i++) {
            nrm[3 * i + 0] = 0;
            nrm[3 * i + 1] = 0;
            nrm[3 * i + 2] = 1;
        }
        par_shapes_mesh* roof = make_mesh(4, pts, 2, tris, nrm, 0);
        par_shapes_shell* rs = par_shapes_create_shell(roof, 0.2f);
        task_check(rs != 0, "non-planar surface with supplied normals succeeds");
        if (rs) {
            int differs = 0, unit = 1;
            for (int i = 0; i < 2 * roof->npoints; i++) {
                float n[3] = {rs->mesh->normals[3 * i],
                    rs->mesh->normals[3 * i + 1], rs->mesh->normals[3 * i + 2]};
                float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (fabsf(len - 1.0f) > 1e-4f) {
                    unit = 0;
                }
                if (fabsf(n[2] - 1.0f) > 1e-3f) {
                    differs = 1;
                }
            }
            task_check(unit, "roof skin normals are unit length");
            task_check(differs, "skin normals are re-accumulated, not copied");
            par_shapes_free_shell(rs);
        }
        free_raw(roof);
    }
}

static int wall_uv_error(par_shapes_mesh const* source,
    par_shapes_shell const* sh, int* nloops_out)
{
    int bnd[512];
    int nb = boundary_slots(source, bnd);
    int nv = source->npoints;
    if (nb > 256) {
        return 1;
    }
    double arc[512], elen[512], perim[512];
    int loopid[512], nextedge[512];
    int bystart_one[512];
    for (int i = 0; i <= nv; i++) {
        bystart_one[i] = 0;
    }
    for (int i = 0; i < nb; i++) {
        int a = (int) source->triangles[bnd[i]];
        elen[i] = edge_len(source, bnd[i]);
        if (bystart_one[a] != 0) {
            return 1;
        }
        bystart_one[a] = i + 1;
    }
    for (int i = 0; i < nb; i++) {
        int b = (int) source->triangles[3 * (bnd[i] / 3) + (bnd[i] % 3 + 1) % 3];
        if (bystart_one[b] == 0) {
            return 1;
        }
        nextedge[i] = bystart_one[b] - 1;
        loopid[i] = -1;
    }
    int nloops = 0;
    for (int i = 0; i < nb; i++) {
        if (loopid[i] >= 0) {
            continue;
        }
        int e = i;
        double acc = 0;
        for (int step = 0; step < nb; step++) {
            loopid[e] = nloops;
            arc[e] = acc;
            acc += elen[e];
            e = nextedge[e];
            if (e == i) {
                break;
            }
        }
        perim[nloops++] = acc;
    }
    if (nloops_out) {
        *nloops_out = nloops;
    }

    // Reported wall triangles hold six fresh vertices per boundary slot.
    int wall = 2 * nv;
    int errors = 0;
    int atend[6] = {1, 0, 0, 0, 1, 1};
    int lay[6] = {1, 1, 0, 0, 0, 1};
    for (int i = 0; i < nb; i++) {
        for (int k = 0; k < 6; k++) {
            double want = (arc[i] + (atend[k] ? elen[i] : 0)) / perim[loopid[i]];
            float got = sh->mesh->tcoords[2 * (wall + k)];
            float v = sh->mesh->tcoords[2 * (wall + k) + 1];
            if (fabs((double) got - want) > 1e-5 || v != (float) lay[k]) {
                errors++;
            }
        }
        wall += 6;
    }
    return errors;
}

static void group_rim(void)
{
    par_shapes_mesh* ann = make_annulus(0.0f);
    par_shapes_shell* sh = par_shapes_create_shell(ann, 0.2f);
    task_check(sh != 0, "annulus with a hole builds a shell");
    if (!sh) {
        free_raw(ann);
        return;
    }
    int bnd[512];
    int nb = boundary_slots(ann, bnd);
    task_check(nb == 8, "annulus has two loops of four boundary edges");
    task_check(sh->mesh->npoints == 2 * ann->npoints + 6 * nb,
        "one independent render vertex set per boundary edge");
    task_check(sh->mesh->ntriangles == 2 * ann->ntriangles + 2 * nb,
        "two wall triangles per boundary edge");
    int flat = 1, unit = 1;
    for (int f = 2 * ann->ntriangles; f < sh->mesh->ntriangles; f++) {
        int ids[3];
        for (int k = 0; k < 3; k++) {
            ids[k] = sh->mesh->triangles[3 * f + k];
        }
        for (int k = 0; k < 3; k++) {
            for (int c = 0; c < 3; c++) {
                if (sh->mesh->normals[3 * ids[0] + c] !=
                    sh->mesh->normals[3 * ids[k] + c]) {
                    flat = 0;
                }
            }
        }
        float n[3] = {sh->mesh->normals[3 * ids[0]],
            sh->mesh->normals[3 * ids[0] + 1],
            sh->mesh->normals[3 * ids[0] + 2]};
        float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (fabsf(len - 1.0f) > 1e-4f) {
            unit = 0;
        }
    }
    task_check(flat, "wall triangles use per-triangle flat normals");
    task_check(unit, "wall normals are unit length");
    int ann_loops = 0;
    task_check(wall_uv_error(ann, sh, &ann_loops) == 0,
        "wall U follows normalized loop arc length and V the layer");
    task_check(ann_loops == 2, "annulus rim has two independent loops");
    int vloops = 0;
    int wall = 2 * ann->npoints;
    for (int i = 0; i < 6 * nb; i++) {
        float u = sh->mesh->tcoords[2 * (wall + i)];
        if (u < 0.0f || u > 1.0f) {
            vloops = 1;
        }
    }
    task_check(!vloops, "all wall U values are in [0,1]");
    // Each loop's origin edge starts at arc length zero, so every loop
    // contributes at least one U=0 wall vertex pair.
    int uzero = 0;
    for (int i = 0; i < 6 * nb; i++) {
        if (sh->mesh->tcoords[2 * (wall + i)] == 0.0f) {
            uzero++;
        }
    }
    task_check(uzero >= ann_loops, "each boundary loop restarts U at zero");
    int from_boundary = 1;
    for (int i = 0; i < 6 * nb; i++) {
        int v = wall + i;
        int src = sh->vertex_source[v];
        int found = 0;
        for (int j = 0; j < nb; j++) {
            int a = (int) ann->triangles[bnd[j]];
            int b = (int) ann->triangles[3 * (bnd[j] / 3) + (bnd[j] % 3 + 1) % 3];
            if (src == a || src == b) {
                found = 1;
            }
        }
        if (!found) {
            from_boundary = 0;
        }
    }
    task_check(from_boundary, "wall vertices point back at boundary sources");
    par_shapes_free_shell(sh);
    free_raw(ann);

    // Two disconnected components, each contributing a boundary loop.
    par_shapes_mesh* two = make_two_quads();
    par_shapes_shell* ts = par_shapes_create_shell(two, 0.15f);
    task_check(ts != 0, "multiple components build a shell");
    if (ts) {
        int tb[512];
        int tnb = boundary_slots(two, tb);
        task_check(tnb == 8, "two quads contribute eight boundary edges");
        task_check(ts->mesh->npoints == 2 * two->npoints + 6 * tnb,
            "multi-component wall vertices match boundary edges");
        int two_loops = 0;
        task_check(wall_uv_error(two, ts, &two_loops) == 0,
            "UVs restart for each component's loop");
        task_check(two_loops == 2, "each component contributes its own loop");
        par_shapes_free_shell(ts);
    }
    free_raw(two);
}

static void group_topology(void)
{
    par_shapes_mesh* plane = par_shapes_create_plane(2, 2);
    par_shapes_shell* ps = par_shapes_create_shell(plane, 0.1f);
    task_check(ps != 0, "open plane builds a shell");
    if (ps) {
        task_check(merged_edge_violations(ps->mesh, ps->vertex_source,
                ps->vertex_layer) == 0,
            "merged ids give every shell edge one opposite face");
        int comps = 0, open = 0;
        int euler = merged_euler(ps->mesh, ps->vertex_source,
            ps->vertex_layer, &comps, &open);
        task_check(euler == 2, "thickened disk is a closed genus-0 surface");
        task_check(open == 0, "no unpaired merged edges on a disk shell");
        task_check(comps == 1, "disk shell is a single connected surface");
        par_shapes_free_shell(ps);
    }
    free_raw(plane);

    par_shapes_mesh* cube = par_shapes_create_cube();
    par_shapes_shell* cs = par_shapes_create_shell(cube, 0.2f);
    task_check(cs != 0, "closed cube builds a shell");
    if (cs) {
        task_check(cs->mesh->npoints == 2 * cube->npoints,
            "closed input has exactly two render vertices per source point");
        task_check(cs->mesh->ntriangles == 2 * cube->ntriangles,
            "closed input needs no rim triangles");
        task_check(merged_edge_violations(cs->mesh, cs->vertex_source,
                cs->vertex_layer) == 0,
            "closed shell is edge-manifold under identity merging");
        int comps = 0, open = 0;
        int euler = merged_euler(cs->mesh, cs->vertex_source,
            cs->vertex_layer, &comps, &open);
        task_check(euler == 4, "two disjoint sphere skins on a closed cube");
        task_check(comps == 2, "closed shell has disconnected inner and outer skins");
        int layers = 1;
        for (int i = 0; i < cs->mesh->npoints; i++) {
            int want = i < cube->npoints ? 0 : 1;
            if (cs->vertex_layer[i] != want ||
                cs->vertex_source[i] != (i % cube->npoints)) {
                layers = 0;
            }
        }
        task_check(layers, "layer and source arrays identify every skin vertex");
        par_shapes_free_shell(cs);
    }
    free_raw(cube);

    // An annulus is thickened into a solid ring: its shell is a torus.
    par_shapes_mesh* ann = make_annulus(0.0f);
    par_shapes_shell* as = par_shapes_create_shell(ann, 0.1f);
    task_check(as != 0, "annulus shell exists");
    if (as) {
        int comps = 0, open = 0;
        int euler = merged_euler(as->mesh, as->vertex_source,
            as->vertex_layer, &comps, &open);
        task_check(euler == 0, "thickened annulus is a torus");
        task_check(open == 0, "annulus shell has no open merged edges");
        task_check(comps == 1, "annulus shell is connected through its walls");
        int identity_ok = 1;
        for (int i = 0; i < as->mesh->npoints; i++) {
            if (as->vertex_source[i] < 0 || as->vertex_source[i] >= ann->npoints) {
                identity_ok = 0;
            }
            if (as->vertex_layer[i] != 0 && as->vertex_layer[i] != 1) {
                identity_ok = 0;
            }
        }
        task_check(identity_ok, "vertex identity arrays stay in range");
        par_shapes_free_shell(as);
    }
    free_raw(ann);
}

static int shell_is_null(par_shapes_mesh* m, float t)
{
    par_shapes_shell* s = par_shapes_create_shell(m, t);
    int is_null = s == 0;
    par_shapes_free_shell(s);
    return is_null;
}

static void group_reject(void)
{
    // Zero-area triangle.
    {
        float pts[9] = {0, 0, 0, 1, 0, 0, 2, 0, 0};
        int tris[3] = {0, 1, 2};
        par_shapes_mesh* m = make_mesh(3, pts, 1, tris, 0, 0);
        task_check(shell_is_null(m, 0.1f), "zero-area triangle is rejected");
        free_raw(m);
    }
    // Invalid index.
    {
        float pts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        int tris[3] = {0, 1, 3};
        par_shapes_mesh* m = make_mesh(3, pts, 1, tris, 0, 0);
        task_check(shell_is_null(m, 0.1f), "out-of-range index is rejected");
        free_raw(m);
    }
    // Same-direction shared edge.
    {
        float pts[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
        int tris[6] = {0, 1, 2, 0, 3, 1};
        par_shapes_mesh* m = make_mesh(4, pts, 2, tris, 0, 0);
        task_check(shell_is_null(m, 0.1f), "same-direction shared edge is rejected");
        free_raw(m);
    }
    // Three faces on one edge.
    {
        float pts[15] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, -1, 0, 0, 0, 1};
        int tris[9] = {0, 1, 2, 1, 0, 3, 0, 1, 4};
        par_shapes_mesh* m = make_mesh(5, pts, 3, tris, 0, 0);
        task_check(shell_is_null(m, 0.1f), "edge with three faces is rejected");
        free_raw(m);
    }
    // Point contact: coplanar, consistently wound triangles sharing only
    // vertex 0.  The accumulated normal there is non-zero, so this must be
    // caught by the fan check rather than the normal check.
    {
        float pts[15] = {0, 0, 0, 1, 0, 0, 0, 1, 0, -1, 0, 0, 0, -1, 0};
        int tris[6] = {0, 1, 2, 0, 3, 4};
        par_shapes_mesh* m = make_mesh(5, pts, 2, tris, 0, 0);
        task_check(shell_is_null(m, 0.1f), "point-touching face fans are rejected");
        free_raw(m);
    }
    // Non-finite coordinates and normals.
    {
        float pts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        int tris[3] = {0, 1, 2};
        pts[1] = NAN;
        par_shapes_mesh* m = make_mesh(3, pts, 1, tris, 0, 0);
        task_check(shell_is_null(m, 0.1f), "non-finite coordinates are rejected");
        free_raw(m);
    }
    {
        float pts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        int tris[3] = {0, 1, 2};
        float nrm[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        nrm[5] = INFINITY;
        par_shapes_mesh* m = make_mesh(3, pts, 1, tris, nrm, 0);
        task_check(shell_is_null(m, 0.1f), "non-finite normals are rejected");
        free_raw(m);
    }
    // Zero supplied normal: no offset direction.
    {
        float pts[12] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0};
        int tris[6] = {0, 1, 2, 2, 1, 3};
        float nrm[12] = {0};
        par_shapes_mesh* m = make_mesh(4, pts, 2, tris, nrm, 0);
        task_check(shell_is_null(m, 0.1f), "zero-length offset direction is rejected");
        free_raw(m);
    }
    // Tangent input normals collapse a rim wall: flat surface, normals along x.
    {
        float pts[15] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0, 2, 2, 0};
        int tris[12] = {0, 1, 2, 2, 1, 3, 1, 4, 3};
        float nrm[15];
        for (int i = 0; i < 5; i++) {
            nrm[3 * i + 0] = 1;
            nrm[3 * i + 1] = 0;
            nrm[3 * i + 2] = 0;
        }
        par_shapes_mesh* m = make_mesh(5, pts, 3, tris, nrm, 0);
        task_check(shell_is_null(m, 0.1f),
            "collapsing tangent offset is rejected after offsetting");
        free_raw(m);
    }
    // In-plane converging normals collapse the outer skin itself: with a
    // thickness of 2, points 0 and 1 both land exactly on (1, 0, 0).
    {
        float pts[9] = {2, 0, 0, 0, 0, 0, 0, 1, 0};
        int tris[3] = {0, 1, 2};
        float nrm[9] = {-1, 0, 0, 1, 0, 0, 0, 0, 1};
        par_shapes_mesh* m = make_mesh(3, pts, 1, tris, nrm, 0);
        task_check(shell_is_null(m, 2.0f),
            "zero-area offset skin is rejected after offsetting");
        free_raw(m);
    }
    // Invalid thickness values.
    {
        float pts[9] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        int tris[3] = {0, 1, 2};
        par_shapes_mesh* m = make_mesh(3, pts, 1, tris, 0, 0);
        task_check(shell_is_null(m, 0.0f) && shell_is_null(m, -1.0f) &&
            shell_is_null(m, NAN) && shell_is_null(m, INFINITY),
            "non-positive or non-finite thickness is rejected");
        task_check(par_shapes_create_shell(0, 0.1f) == 0,
            "null source is rejected");
        free_raw(m);
    }
    // A valid non-planar, non-degenerate surface still succeeds.
    {
        par_shapes_mesh* torus = par_shapes_create_torus(10, 6, 0.3f);
        par_shapes_shell* ok = par_shapes_create_shell(torus, 0.05f);
        task_check(ok != 0, "ordinary curved closed surface succeeds");
        par_shapes_free_shell(ok);
        free_raw(torus);
    }
    // Capacity: the 16-bit build cannot address the result, the 32-bit one can.
    {
        par_shapes_mesh* big = par_shapes_create_plane(200, 200);
        long long maxindex = (long long) ((PAR_SHAPES_T) ~(PAR_SHAPES_T) 0);
        long long nvtx = 2 * (long long) big->npoints + 6 * 4 * 200LL;
        par_shapes_shell* s = par_shapes_create_shell(big, 0.001f);
        if (nvtx > maxindex + 1) {
            task_check(s == 0 && nvtx > 65535,
                "index capacity limits are enforced");
        } else {
            task_check(s != 0 && s->mesh->npoints == (int) nvtx,
                "wide index type handles a large shell");
        }
        par_shapes_free_shell(s);
        free_raw(big);
    }
}

void task_cases(const char* group)
{
    if (!strcmp(group, "skins")) {
        group_skins();
    } else if (!strcmp(group, "rim")) {
        group_rim();
    } else if (!strcmp(group, "topology")) {
        group_topology();
    } else if (!strcmp(group, "reject")) {
        group_reject();
    } else {
        task_check(0, "unknown group");
    }
}
