#include "checks.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Failure-injecting allocators.  These macros are seen by the implementation
   below because PAR_SHAPES_IMPLEMENTATION is defined before par_shapes.h. */
static unsigned long rt_alloc_calls;
static unsigned long rt_fail_at;
static long rt_live_allocations;

static void* rt_malloc_impl(size_t element_size, size_t count) {
    size_t bytes;
    if (count != 0 && element_size > (size_t)-1 / count) return NULL;
    bytes = element_size * count;
    ++rt_alloc_calls;
    if (rt_fail_at != 0 && rt_alloc_calls == rt_fail_at) return NULL;
    void* p = malloc(bytes ? bytes : 1);
    if (p) ++rt_live_allocations;
    return p;
}
static void* rt_calloc_impl(size_t element_size, size_t count) {
    if (count != 0 && element_size > (size_t)-1 / count) return NULL;
    ++rt_alloc_calls;
    if (rt_fail_at != 0 && rt_alloc_calls == rt_fail_at) return NULL;
    void* p = calloc(count ? count : 1, element_size ? element_size : 1);
    if (p) ++rt_live_allocations;
    return p;
}
static void* rt_realloc_impl(void* old, size_t element_size, size_t count) {
    size_t bytes;
    void* p;
    if (count != 0 && element_size > (size_t)-1 / count) return NULL;
    bytes = element_size * count;
    ++rt_alloc_calls;
    if (rt_fail_at != 0 && rt_alloc_calls == rt_fail_at) return NULL;
    p = realloc(old, bytes ? bytes : 1);
    if (p && !old) ++rt_live_allocations;
    return p;
}
static void rt_free_impl(void* p) {
    if (p) {
        --rt_live_allocations;
        free(p);
    }
}

#define PAR_MALLOC(T, N) ((T*) rt_malloc_impl(sizeof(T), (size_t)(N)))
#define PAR_CALLOC(T, N) ((T*) rt_calloc_impl(sizeof(T), (size_t)(N)))
#define PAR_REALLOC(T, P, N) ((T*) rt_realloc_impl((P), sizeof(T), (size_t)(N)))
#define PAR_FREE(P) rt_free_impl(P)
#define PAR_SHAPES_REALLOC(T, P, N, OLD_N) \
    ((T*) rt_realloc_impl((P), sizeof(T), (size_t)(N)))

#define PAR_SHAPES_IMPLEMENTATION
#include "par_shapes.h"

/* ------------------------------------------------------------------ */
/* Small independent double-precision geometry toolkit.                */
/* ------------------------------------------------------------------ */

typedef struct rt_vec { double x, y, z; } rt_vec;
static rt_vec rt_v(double x, double y, double z) { rt_vec r = {x,y,z}; return r; }
static rt_vec rt_add(rt_vec a, rt_vec b) { return rt_v(a.x+b.x,a.y+b.y,a.z+b.z); }
static rt_vec rt_sub(rt_vec a, rt_vec b) { return rt_v(a.x-b.x,a.y-b.y,a.z-b.z); }
static rt_vec rt_mul(rt_vec a, double s) { return rt_v(a.x*s,a.y*s,a.z*s); }
static double rt_dot(rt_vec a, rt_vec b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static rt_vec rt_cross(rt_vec a, rt_vec b) {
    return rt_v(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x);
}
static double rt_len(rt_vec a) { return sqrt(rt_dot(a,a)); }
static rt_vec rt_norm(rt_vec a) {
    double l = rt_len(a);
    return l > 0 ? rt_mul(a, 1.0/l) : a;
}
static rt_vec rt_from(float const* p) { return rt_v(p[0],p[1],p[2]); }
static rt_vec rt_slerp(rt_vec a, rt_vec b, double t) {
    double d = PAR_CLAMP(rt_dot(a,b), -1.0, 1.0);
    double theta = acos(d);
    if (theta < 1e-12) return rt_norm(rt_add(rt_mul(a,1-t), rt_mul(b,t)));
    double sa = sin((1-t)*theta)/sin(theta);
    double sb = sin(t*theta)/sin(theta);
    return rt_norm(rt_add(rt_mul(a,sa), rt_mul(b,sb)));
}
static rt_vec rt_closest_segment(rt_vec p, rt_vec a, rt_vec b) {
    rt_vec ab = rt_sub(b,a);
    double d = rt_dot(ab,ab);
    double t = d > 0 ? rt_dot(rt_sub(p,a),ab)/d : 0;
    t = PAR_CLAMP(t,0,1);
    return rt_add(a, rt_mul(ab,t));
}
static rt_vec rt_closest_triangle(rt_vec p, rt_vec a, rt_vec b, rt_vec c) {
    rt_vec ab = rt_sub(b,a), ac = rt_sub(c,a), ap = rt_sub(p,a);
    double d1 = rt_dot(ab,ap), d2 = rt_dot(ac,ap);
    if (d1 <= 0 && d2 <= 0) return a;
    rt_vec bp = rt_sub(p,b);
    double d3 = rt_dot(ab,bp), d4 = rt_dot(ac,bp);
    if (d3 >= 0 && d4 <= d3) return b;
    double vc = d1*d4-d3*d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0)
        return rt_add(a, rt_mul(ab,d1/(d1-d3)));
    rt_vec cp = rt_sub(p,c);
    double d5 = rt_dot(ab,cp), d6 = rt_dot(ac,cp);
    if (d6 >= 0 && d5 <= d6) return c;
    double vb = d5*d2-d1*d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0)
        return rt_add(a, rt_mul(ac,d2/(d2-d6)));
    double va = d3*d6-d5*d4;
    if (va <= 0 && d4-d3 >= 0 && d5-d6 >= 0) {
        double t = (d4-d3)/((d4-d3)+(d5-d6));
        return rt_add(b, rt_mul(rt_sub(c,b),t));
    }
    double den = va+vb+vc;
    double v = vb/den, w = vc/den;
    return rt_add(rt_add(a, rt_mul(ab,v)), rt_mul(ac,w));
}
static rt_vec rt_closest_mesh(rt_vec p, par_shapes_mesh const* m) {
    double best = 1e300;
    rt_vec qbest = rt_v(0,0,0);
    for (int f = 0; f < m->ntriangles; ++f) {
        int ia = m->triangles[3*f+0], ib = m->triangles[3*f+1],
            ic = m->triangles[3*f+2];
        rt_vec a = rt_from(m->points+3*ia);
        rt_vec b = rt_from(m->points+3*ib);
        rt_vec c = rt_from(m->points+3*ic);
        rt_vec q = rt_closest_triangle(p,a,b,c);
        double d = rt_len(rt_sub(p,q));
        if (d < best) { best = d; qbest = q; }
    }
    return qbest;
}
static double rt_distance_mesh(rt_vec p, par_shapes_mesh const* m) {
    return rt_len(rt_sub(p, rt_closest_mesh(p,m)));
}

/* ------------------------------------------------------------------ */
/* Fixtures.                                                           */
/* ------------------------------------------------------------------ */

static par_shapes_mesh* rt_make(int nv, float const* points, int nt,
    int const* indices) {
    par_shapes_mesh* m = (par_shapes_mesh*)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->npoints = nv;
    m->ntriangles = nt;
    m->points = (float*)malloc((size_t)3*nv*sizeof(float));
    m->triangles = (PAR_SHAPES_T*)malloc((size_t)3*nt*sizeof(PAR_SHAPES_T));
    if (!m->points || !m->triangles) {
        free(m->points); free(m->triangles); free(m); return NULL;
    }
    memcpy(m->points, points, (size_t)3*nv*sizeof(float));
    for (int i = 0; i < 3*nt; ++i)
        m->triangles[i] = (PAR_SHAPES_T)indices[i];
    return m;
}
static par_shapes_mesh* rt_clone(par_shapes_mesh const* s) {
    par_shapes_mesh* m = (par_shapes_mesh*)calloc(1,sizeof(*m));
    if (!m) return NULL;
    *m = *s;
    m->points = NULL; m->triangles = NULL; m->normals = NULL; m->tcoords = NULL;
    if (s->points) {
        m->points = (float*)malloc((size_t)3*s->npoints*sizeof(float));
        memcpy(m->points,s->points,(size_t)3*s->npoints*sizeof(float));
    }
    if (s->triangles) {
        m->triangles = (PAR_SHAPES_T*)malloc((size_t)3*s->ntriangles*
            sizeof(PAR_SHAPES_T));
        memcpy(m->triangles,s->triangles,(size_t)3*s->ntriangles*
            sizeof(PAR_SHAPES_T));
    }
    if (s->normals) {
        m->normals = (float*)malloc((size_t)3*s->npoints*sizeof(float));
        memcpy(m->normals,s->normals,(size_t)3*s->npoints*sizeof(float));
    }
    if (s->tcoords) {
        m->tcoords = (float*)malloc((size_t)2*s->npoints*sizeof(float));
        memcpy(m->tcoords,s->tcoords,(size_t)2*s->npoints*sizeof(float));
    }
    return m;
}
static void rt_free_source(par_shapes_mesh* m) {
    if (!m) return;
    free(m->points); free(m->triangles); free(m->normals);
    free(m->tcoords); free(m);
}
static void rt_orient_outward(par_shapes_mesh* m) {
    double vol6 = 0;
    for (int f = 0; f < m->ntriangles; ++f) {
        rt_vec a = rt_from(m->points+3*m->triangles[3*f]);
        rt_vec b = rt_from(m->points+3*m->triangles[3*f+1]);
        rt_vec c = rt_from(m->points+3*m->triangles[3*f+2]);
        vol6 += rt_dot(a, rt_cross(b,c));
    }
    if (vol6 < 0) for (int f = 0; f < m->ntriangles; ++f) {
        PAR_SHAPES_T t = m->triangles[3*f+1];
        m->triangles[3*f+1] = m->triangles[3*f+2];
        m->triangles[3*f+2] = t;
    }
}
static double rt_aabb_diagonal(par_shapes_mesh const* m) {
    rt_vec mn = rt_v(1e300,1e300,1e300), mx = rt_v(-1e300,-1e300,-1e300);
    for (int i = 0; i < m->npoints; ++i) {
        rt_vec p = rt_from(m->points+3*i);
        mn.x = PAR_MIN(mn.x,p.x); mn.y = PAR_MIN(mn.y,p.y); mn.z = PAR_MIN(mn.z,p.z);
        mx.x = PAR_MAX(mx.x,p.x); mx.y = PAR_MAX(mx.y,p.y); mx.z = PAR_MAX(mx.z,p.z);
    }
    return rt_len(rt_sub(mx,mn));
}

static par_shapes_mesh* rt_box(int alternate_diagonals) {
    static float p[24] = {
        -1.30f,-0.72f,-0.48f,  1.30f,-0.72f,-0.48f,
         1.30f, 0.72f,-0.48f, -1.30f, 0.72f,-0.48f,
        -1.30f,-0.72f, 0.48f,  1.30f,-0.72f, 0.48f,
         1.30f, 0.72f, 0.48f, -1.30f, 0.72f, 0.48f };
    int idx[36] = {
        0,2,1, 0,3,2,
        4,5,6, 4,6,7,
        0,1,5, 0,5,4,
        1,2,6, 1,6,5,
        2,3,7, 2,7,6,
        3,0,4, 3,4,7 };
    if (alternate_diagonals) {
        int alt[12] = { 0,3,1, 1,3,2,  4,5,7, 5,6,7 };
        memcpy(idx, alt, sizeof(alt));
    }
    par_shapes_mesh* m = rt_make(8,p,12,idx);
    rt_orient_outward(m);
    return m;
}
static par_shapes_mesh* rt_shuffled_box(void) {
    par_shapes_mesh* base = rt_box(0);
    static int order[12] = { 7,0,11,3,9,1,5,10,2,6,8,4 };
    par_shapes_mesh* m = (par_shapes_mesh*)calloc(1,sizeof(*m));
    m->npoints = base->npoints;
    m->ntriangles = base->ntriangles;
    m->points = (float*)malloc(sizeof(float)*3*m->npoints);
    m->triangles = (PAR_SHAPES_T*)malloc(sizeof(PAR_SHAPES_T)*3*m->ntriangles);
    memcpy(m->points,base->points,sizeof(float)*3*m->npoints);
    for (int f = 0; f < 12; ++f) for (int k = 0; k < 3; ++k)
        m->triangles[3*f+k] = base->triangles[3*order[f]+k];
    rt_free_source(base);
    return m;
}
static par_shapes_mesh* rt_tetra(void) {
    static float p[12] = {
        0.18f, 0.10f, 0.00f,
        1.72f, 0.02f, 0.24f,
        0.08f, 1.31f,-0.18f,
        0.34f, 0.42f, 1.55f };
    static int idx[12] = { 0,1,2, 0,3,1, 0,2,3, 1,3,2 };
    par_shapes_mesh* m = rt_make(4,p,4,idx);
    rt_orient_outward(m);
    return m;
}
static par_shapes_mesh* rt_octa(void) {
    static float p[18] = {
        1.18f,0,0, -0.92f,0,0,
        0,1.08f,0, 0,-0.78f,0,
        0,0,1.27f, 0,0,-0.96f };
    static int idx[24] = {
        0,2,4, 2,1,4, 1,3,4, 3,0,4,
        2,0,5, 1,2,5, 3,1,5, 0,3,5 };
    par_shapes_mesh* m = rt_make(6,p,8,idx);
    rt_orient_outward(m);
    return m;
}
static par_shapes_mesh* rt_bipyramid(int sides) {
    int nv = sides+2, nt = 2*sides;
    float* p = (float*)calloc((size_t)3*nv,sizeof(float));
    int* idx = (int*)calloc((size_t)3*nt,sizeof(int));
    for (int i = 0; i < sides; ++i) {
        double a = 2.0*PAR_PI*i/sides;
        p[3*i+0] = (float)cos(a);
        p[3*i+1] = (float)sin(a);
        p[3*i+2] = 0;
    }
    p[3*sides+2] = 0.55f;
    p[3*(sides+1)+2] = -0.62f;
    int top = sides, bottom = sides+1, t = 0;
    for (int i = 0; i < sides; ++i) {
        int j = (i+1)%sides;
        idx[t++] = i; idx[t++] = j; idx[t++] = top;
        idx[t++] = j; idx[t++] = i; idx[t++] = bottom;
    }
    par_shapes_mesh* m = rt_make(nv,p,nt,idx);
    free(p); free(idx);
    rt_orient_outward(m);
    return m;
}

/* ------------------------------------------------------------------ */
/* Index/topology audit.                                               */
/* ------------------------------------------------------------------ */

typedef struct rt_edge { int u,v,h; } rt_edge;
typedef struct rt_face_key { int a,b,c; } rt_face_key;
static int rt_edge_cmp(void const* aa, void const* bb) {
    rt_edge const* a = (rt_edge const*)aa, *b = (rt_edge const*)bb;
    if (a->u != b->u) return a->u < b->u ? -1 : 1;
    if (a->v != b->v) return a->v < b->v ? -1 : 1;
    return a->h < b->h ? -1 : a->h > b->h;
}
static int rt_face_cmp(void const* aa, void const* bb) {
    rt_face_key const* a = (rt_face_key const*)aa, *b = (rt_face_key const*)bb;
    if (a->a != b->a) return a->a < b->a ? -1 : 1;
    if (a->b != b->b) return a->b < b->b ? -1 : 1;
    if (a->c != b->c) return a->c < b->c ? -1 : 1;
    return 0;
}
static int rt_audit_topology(par_shapes_mesh const* m) {
    task_check(m != NULL, "rounded mesh exists");
    if (!m) return 0;
    task_check(m->points && m->triangles && m->normals, "mesh arrays exist");
    task_check(m->npoints >= 4 && m->ntriangles >= 4, "mesh counts positive");
    if (!m->points || !m->triangles || !m->normals) return 0;
    int H = 3*m->ntriangles;
    rt_edge* edges = (rt_edge*)malloc(sizeof(rt_edge)*(size_t)H);
    int* twin = (int*)malloc(sizeof(int)*(size_t)H);
    task_check(edges && twin, "audit allocations");
    if (!edges || !twin) { free(edges); free(twin); return 0; }
    for (int h = 0; h < H; ++h) {
        int f = h/3, k = h%3;
        int a = m->triangles[h];
        int b = m->triangles[3*f+(k+1)%3];
        task_check(a >= 0 && a < m->npoints, "index in range");
        task_check(b >= 0 && b < m->npoints, "index in range");
        edges[h].u = PAR_MIN(a,b);
        edges[h].v = PAR_MAX(a,b);
        edges[h].h = h;
        twin[h] = -1;
        rt_vec pa = rt_from(m->points+3*a);
        rt_vec pb = rt_from(m->points+3*b);
        task_check(rt_len(rt_sub(pa,pb)) > 1e-7*rt_aabb_diagonal(m),
            "no repeated indexed edge positions");
    }
    qsort(edges,(size_t)H,sizeof(*edges),rt_edge_cmp);
    int nedges = 0;
    for (int i = 0; i < H; ) {
        int j = i+1;
        while (j < H && edges[j].u == edges[i].u && edges[j].v == edges[i].v) ++j;
        task_check(j-i == 2, "each shell edge has two faces");
        if (j-i == 2) {
            int h0 = edges[i].h, h1 = edges[i+1].h;
            int f0 = h0/3,k0=h0%3,f1=h1/3,k1=h1%3;
            int a0 = m->triangles[h0];
            int b0 = m->triangles[3*f0+(k0+1)%3];
            int a1 = m->triangles[h1];
            int b1 = m->triangles[3*f1+(k1+1)%3];
            task_check(a0 == b1 && b0 == a1, "shared edges have reverse winding");
            twin[h0] = h1; twin[h1] = h0;
        }
        ++nedges;
        i = j;
    }
    task_check((long long)m->npoints - nedges + m->ntriangles == 2,
        "Euler characteristic is two");

    char* seen = (char*)calloc((size_t)H,1);
    int cycles = 0;
    task_check(seen != NULL, "fan allocation");
    if (seen) {
        for (int start = 0; start < H; ++start) {
            if (seen[start]) continue;
            ++cycles;
            int cur = start;
            while (!seen[cur]) {
                seen[cur] = 1;
                int tw = twin[cur];
                task_check(tw >= 0, "halfedge twin exists");
                int nf = tw/3, nk = (tw%3+1)%3;
                cur = 3*nf+nk;
            }
            task_check(cur == start && cycles <= m->npoints,
                "vertex face fan is one closed cycle");
        }
        task_check(cycles == m->npoints, "one face fan per output vertex");
        free(seen);
    }

    rt_face_key* faces = (rt_face_key*)malloc(sizeof(rt_face_key)*
        (size_t)m->ntriangles);
    task_check(faces != NULL, "face-key allocation");
    if (faces) {
        double scale = rt_aabb_diagonal(m);
        for (int f = 0; f < m->ntriangles; ++f) {
            int a=m->triangles[3*f], b=m->triangles[3*f+1], c=m->triangles[3*f+2];
            task_check(a!=b && b!=c && c!=a, "triangle has distinct indices");
            rt_vec A=rt_from(m->points+3*a), B=rt_from(m->points+3*b),
                C=rt_from(m->points+3*c);
            double h = rt_len(rt_cross(rt_sub(B,A),rt_sub(C,A))) /
                PAR_MAX(rt_len(rt_sub(B,A)), PAR_MAX(rt_len(rt_sub(C,A)),
                rt_len(rt_sub(C,B))));
            task_check(h > 1e-8*scale, "no zero-area output triangle");
            faces[f].a=PAR_MIN(a,PAR_MIN(b,c));
            faces[f].c=PAR_MAX(a,PAR_MAX(b,c));
            faces[f].b=a+b+c-faces[f].a-faces[f].c;
        }
        qsort(faces,(size_t)m->ntriangles,sizeof(*faces),rt_face_cmp);
        for (int f = 1; f < m->ntriangles; ++f)
            task_check(rt_face_cmp(&faces[f-1],&faces[f]) != 0,
                "no duplicate output face");
        free(faces);
    }
    free(edges); free(twin);
    return nedges;
}

/* ------------------------------------------------------------------ */
/* Sampling and Hausdorff checks.                                      */
/* ------------------------------------------------------------------ */

static double rt_mesh_to_mesh(rt_vec (*transform)(rt_vec,void*), void* ud,
    par_shapes_mesh const* a, par_shapes_mesh const* b, int subdiv) {
    double worst = 0;
    for (int f = 0; f < a->ntriangles; ++f) {
        rt_vec A = rt_from(a->points+3*a->triangles[3*f]);
        rt_vec B = rt_from(a->points+3*a->triangles[3*f+1]);
        rt_vec C = rt_from(a->points+3*a->triangles[3*f+2]);
        for (int i = 0; i <= subdiv; ++i)
        for (int j = 0; j <= subdiv-i; ++j) {
            double u=(double)i/subdiv, v=(double)j/subdiv, w=1-u-v;
            rt_vec p = rt_add(rt_add(rt_mul(A,w),rt_mul(B,u)),rt_mul(C,v));
            if (transform) p = transform(p,ud);
            worst = PAR_MAX(worst, rt_distance_mesh(p,b));
        }
    }
    return worst;
}
static rt_vec rt_identity_transform(rt_vec p, void* ud) { (void)ud; return p; }
static double rt_symmetric_haus(par_shapes_mesh const* a,
    par_shapes_mesh const* b, int subdiv) {
    return PAR_MAX(rt_mesh_to_mesh(rt_identity_transform,NULL,a,b,subdiv),
                   rt_mesh_to_mesh(rt_identity_transform,NULL,b,a,subdiv));
}

struct rt_xf { rt_vec axis; double c,s,scale; rt_vec translation; };
static rt_vec rt_apply_xf(rt_vec p, struct rt_xf const* xf, int inverse) {
    if (inverse) {
        rt_vec q = rt_sub(p, xf->translation);
        q = rt_mul(q, 1.0/xf->scale);
        double c = xf->c, s = -xf->s;
        rt_vec ax = xf->axis;
        rt_vec t1 = rt_mul(q,c);
        rt_vec t2 = rt_mul(rt_cross(ax,q),s);
        rt_vec t3 = rt_mul(ax,rt_dot(ax,q)*(1.0-c));
        return rt_add(rt_add(t1,t2),t3);
    }
    rt_vec t1 = rt_mul(p,xf->c);
    rt_vec t2 = rt_mul(rt_cross(xf->axis,p),xf->s);
    rt_vec t3 = rt_mul(xf->axis,rt_dot(xf->axis,p)*(1.0-xf->c));
    rt_vec q = rt_add(rt_add(t1,t2),t3);
    q = rt_mul(q,xf->scale);
    return rt_add(q,xf->translation);
}
static rt_vec rt_xf_forward(rt_vec p, void* ud) {
    return rt_apply_xf(p,(struct rt_xf*)ud,0);
}
static rt_vec rt_xf_inverse(rt_vec p, void* ud) {
    return rt_apply_xf(p,(struct rt_xf*)ud,1);
}

static double rt_forward_error(par_shapes_mesh const* source,
    par_shapes_mesh const* rounded, double radius) {
    double worst = 0;
    for (int f = 0; f < rounded->ntriangles; ++f) {
        rt_vec A=rt_from(rounded->points+3*rounded->triangles[3*f]);
        rt_vec B=rt_from(rounded->points+3*rounded->triangles[3*f+1]);
        rt_vec C=rt_from(rounded->points+3*rounded->triangles[3*f+2]);
        for (int i = 0; i <= 9; ++i)
        for (int j = 0; j <= 9-i; ++j) {
            double u=i/9.0, v=j/9.0, w=1-u-v;
            rt_vec p=rt_add(rt_add(rt_mul(A,w),rt_mul(B,u)),rt_mul(C,v));
            double d = rt_distance_mesh(p,source);
            worst = PAR_MAX(worst, fabs(d-radius));
        }
    }
    return worst;
}
static rt_vec rt_source_normal(par_shapes_mesh const* s, int f) {
    rt_vec a=rt_from(s->points+3*s->triangles[3*f]);
    rt_vec b=rt_from(s->points+3*s->triangles[3*f+1]);
    rt_vec c=rt_from(s->points+3*s->triangles[3*f+2]);
    return rt_norm(rt_cross(rt_sub(b,a),rt_sub(c,a)));
}
struct rt_true_edge { int a,b,f0,f1; rt_vec n0,n1; };
static int rt_build_true_edges(par_shapes_mesh const* s,
    struct rt_true_edge* out, int max_edges) {
    int H=3*s->ntriangles, n=0;
    for (int h0=0; h0<H; ++h0) for (int h1=h0+1; h1<H; ++h1) {
        int f0=h0/3,k0=h0%3,f1=h1/3,k1=h1%3;
        int a0=s->triangles[h0], b0=s->triangles[3*f0+(k0+1)%3];
        int a1=s->triangles[h1], b1=s->triangles[3*f1+(k1+1)%3];
        if (!(a0==b1 && b0==a1)) continue;
        rt_vec n0=rt_source_normal(s,f0), n1=rt_source_normal(s,f1);
        if (rt_len(rt_sub(n0,n1)) < 1e-3) continue;
        if (n < max_edges) {
            out[n].a=a0; out[n].b=b0; out[n].f0=f0; out[n].f1=f1;
            out[n].n0=n0; out[n].n1=n1; ++n;
        }
    }
    return n;
}
static double rt_reverse_error(par_shapes_mesh const* s,
    par_shapes_mesh const* rounded, double radius) {
    double worst = 0;
    /* Planar support patches. */
    for (int f = 0; f < s->ntriangles; ++f) {
        rt_vec a=rt_from(s->points+3*s->triangles[3*f]);
        rt_vec b=rt_from(s->points+3*s->triangles[3*f+1]);
        rt_vec c=rt_from(s->points+3*s->triangles[3*f+2]);
        rt_vec n=rt_source_normal(s,f);
        for (int i=0;i<=6;++i) for (int j=0;j<=6-i;++j) {
            double u=i/6.0,v=j/6.0,w=1-u-v;
            rt_vec p=rt_add(rt_add(rt_add(rt_mul(a,w),rt_mul(b,u)),rt_mul(c,v)),
                rt_mul(n,radius));
            worst=PAR_MAX(worst,rt_distance_mesh(p,rounded));
        }
    }
    /* Cylindrical true-edge patches. */
    struct rt_true_edge edges[256];
    int ne=rt_build_true_edges(s,edges,256);
    for (int e=0;e<ne;++e) {
        rt_vec a=rt_from(s->points+3*edges[e].a);
        rt_vec b=rt_from(s->points+3*edges[e].b);
        for (int it=0;it<=4;++it) {
            rt_vec base=rt_add(a,rt_mul(rt_sub(b,a),it/4.0));
            for (int j=0;j<=28;++j) {
                rt_vec n=rt_slerp(edges[e].n0,edges[e].n1,j/28.0);
                rt_vec p=rt_add(base,rt_mul(n,radius));
                worst=PAR_MAX(worst,rt_distance_mesh(p,rounded));
            }
        }
    }
    /* Spherical vertex normal cones, sampled by positive combinations. */
    srand(918273);
    for (int vi=0;vi<s->npoints;++vi) {
        rt_vec ns[64]; int m=0;
        for (int f=0;f<s->ntriangles;++f) {
            int incident=0;
            for (int k=0;k<3;++k) if ((int)s->triangles[3*f+k]==vi) incident=1;
            if (!incident) continue;
            rt_vec n=rt_source_normal(s,f);
            int exists=0;
            for (int i=0;i<m;++i) if (rt_len(rt_sub(ns[i],n))<1e-5) exists=1;
            if (!exists && m<64) ns[m++]=n;
        }
        rt_vec base=rt_from(s->points+3*vi);
        for (int z=0;z<140;++z) {
            rt_vec sum=rt_v(0,0,0);
            for (int i=0;i<m;++i) {
                double weight=-log(((double)rand()+1.0)/((double)RAND_MAX+2.0));
                sum=rt_add(sum,rt_mul(ns[i],weight));
            }
            rt_vec n=rt_norm(sum);
            worst=PAR_MAX(worst,
                rt_distance_mesh(rt_add(base,rt_mul(n,radius)),rounded));
        }
    }
    return worst;
}

static void rt_check_analytic_features(par_shapes_mesh const* s,
    par_shapes_mesh const* m, double radius, double L) {
    struct rt_true_edge edges[256];
    int ne=rt_build_true_edges(s,edges,256);
    int plane_like=0, edge_like=0, corner_like=0;
    double worst_normal = 0, worst_radius = 0, worst_place = 0;
    for (int i=0;i<m->npoints;++i) {
        rt_vec p=rt_from(m->points+3*i), n=rt_from(m->normals+3*i);
        double nl=rt_len(n);
        worst_normal=PAR_MAX(worst_normal,fabs(nl-1.0));
        rt_vec q=rt_closest_mesh(p,s);
        double d=rt_len(rt_sub(p,q));
        worst_radius=PAR_MAX(worst_radius,fabs(d-radius));
        rt_vec expected=rt_norm(rt_sub(p,q));
        double nd=rt_dot(n,expected);
        worst_normal=PAR_MAX(worst_normal,1.0-PAR_CLAMP(nd,-1,1));

        int source_normal=0;
        for (int f=0;f<s->ntriangles;++f) {
            if (rt_len(rt_sub(n,rt_source_normal(s,f))) < 2e-3)
                source_normal=1;
        }
        if (source_normal) { ++plane_like; continue; }

        int found_edge=0;
        for (int e=0;e<ne && !found_edge;++e) {
            rt_vec a=rt_from(s->points+3*edges[e].a);
            rt_vec b=rt_from(s->points+3*edges[e].b);
            rt_vec dir=rt_norm(rt_sub(b,a));
            if (fabs(rt_dot(n,dir)) > 3e-3) continue;
            double delta=acos(PAR_CLAMP(rt_dot(edges[e].n0,edges[e].n1),-1,1));
            double d0=acos(PAR_CLAMP(rt_dot(n,edges[e].n0),-1,1));
            double d1=acos(PAR_CLAMP(rt_dot(n,edges[e].n1),-1,1));
            if (fabs(d0+d1-delta) > 3e-3) continue;
            rt_vec qedge=rt_closest_segment(p,a,b);
            rt_vec analytic=rt_add(qedge,rt_mul(n,radius));
            double placement=rt_len(rt_sub(p,analytic));
            if (placement < 2e-4*L) {
                found_edge=1;
                worst_place=PAR_MAX(worst_place,placement);
            }
        }
        if (found_edge) ++edge_like;
        else {
            int found_vertex=0;
            for (int v=0;v<s->npoints;++v) {
                rt_vec base=rt_from(s->points+3*v);
                if (rt_len(rt_sub(p,rt_add(base,rt_mul(n,radius)))) < 2e-4*L)
                    found_vertex=1;
            }
            task_check(found_vertex, "spherical vertex has analytic position/normal");
            if (found_vertex) ++corner_like;
        }
    }
    task_check(worst_normal < 2e-3, "all output normals are unit analytic directions");
    task_check(worst_radius < 2e-4*L, "all output vertices lie on rounded surface");
    task_check(plane_like >= 8, "planar patch normals are present");
    task_check(edge_like >= PAR_MIN(12,ne), "cylindrical edge normals are present");
    task_check(corner_like >= s->npoints, "spherical corner patches are tessellated");
}

/* ------------------------------------------------------------------ */
/* Groups.                                                             */
/* ------------------------------------------------------------------ */

static void rt_patches(void) {
    par_shapes_mesh* box=rt_box(0);
    par_shapes_mesh* tet=rt_tetra();
    par_shapes_mesh* oct=rt_octa();
    par_shapes_mesh* sources[3]={box,tet,oct};
    char const* names[3]={"unequal box","oblique tetrahedron","four-valent octahedron"};
    for (int k=0;k<3;++k) {
        double L=rt_aabb_diagonal(sources[k]);
        float radius=(float)(0.10*L);
        float coarse=(float)(0.05*radius);
        float fine=(float)(0.015*radius);
        par_shapes_mesh* a=par_shapes_create_rounded(sources[k],radius,coarse);
        par_shapes_mesh* b=par_shapes_create_rounded(sources[k],radius,fine);
        task_check(a && b, names[k]);
        if (!a || !b) continue;
        task_check(b->ntriangles > a->ntriangles, "finer error creates more facets");
        rt_audit_topology(a);
        rt_audit_topology(b);
        rt_check_analytic_features(sources[k],b,radius,L);
        double ha=rt_symmetric_haus(a,b,2);
        task_check(ha < 0.06*radius, "two error tiers describe same rounded solid");
        par_shapes_free_mesh(a);
        par_shapes_free_mesh(b);
    }
    rt_free_source(box); rt_free_source(tet); rt_free_source(oct);
}

static void rt_topology(void) {
    par_shapes_mesh* box=rt_box(0);
    par_shapes_mesh* alt=rt_box(1);
    par_shapes_mesh* shuffled=rt_shuffled_box();
    double L=rt_aabb_diagonal(box);
    float radius=(float)(0.12*L), error=(float)(0.025*radius);
    par_shapes_mesh* a=par_shapes_create_rounded(box,radius,error);
    par_shapes_mesh* b=par_shapes_create_rounded(alt,radius,error);
    par_shapes_mesh* c=par_shapes_create_rounded(shuffled,radius,error);
    task_check(a && b && c, "box topology variants round successfully");
    if (a && b && c) {
        rt_audit_topology(a);
        rt_audit_topology(b);
        rt_audit_topology(c);
        double hdiag=rt_symmetric_haus(a,b,3);
        double hshuffle=rt_symmetric_haus(a,c,3);
        task_check(hdiag < 2e-5*L,
            "face diagonal choice does not create a rounded seam");
        task_check(hshuffle < 2e-5*L,
            "face order does not change rounded surface");
        task_check(a->npoints == b->npoints && a->ntriangles == b->ntriangles &&
            a->npoints == c->npoints && a->ntriangles == c->ntriangles,
            "coplanar diagonal is merged rather than rounded");
        par_shapes_free_mesh(b);
        par_shapes_free_mesh(c);
    }

    struct rt_xf xf;
    rt_vec axis=rt_norm(rt_v(1.0,2.0,3.0));
    double angle=0.63, scale=1.25;
    xf.axis=axis; xf.c=cos(angle); xf.s=sin(angle); xf.scale=scale;
    xf.translation=rt_v(0.35,-0.25,0.45);
    par_shapes_mesh* rotated=rt_clone(box);
    for (int i=0;i<rotated->npoints;++i) {
        rt_vec p=rt_from(rotated->points+3*i);
        rt_vec q=rt_apply_xf(p,&xf,0);
        rotated->points[3*i]=(float)q.x;
        rotated->points[3*i+1]=(float)q.y;
        rotated->points[3*i+2]=(float)q.z;
    }
    float r2=radius*(float)scale, e2=error*(float)scale;
    par_shapes_mesh* xm=par_shapes_create_rounded(rotated,r2,e2);
    task_check(xm != NULL, "rotated and scaled box rounds");
    if (xm && a) {
        rt_audit_topology(xm);
        double forward=rt_mesh_to_mesh(rt_xf_inverse,&xf,xm,a,3);
        double backward=rt_mesh_to_mesh(rt_xf_forward,&xf,a,xm,3);
        double xh=PAR_MAX(forward,backward);
        task_check(xh < 5e-5*L,
            "rotation and scaling preserve the same rounded surface");
        par_shapes_free_mesh(xm);
    }
    if (a) par_shapes_free_mesh(a);
    rt_free_source(rotated);

    par_shapes_mesh* tet=rt_tetra();
    par_shapes_mesh* oct=rt_octa();
    par_shapes_mesh* rt=par_shapes_create_rounded(tet,0.22f,0.006f);
    par_shapes_mesh* ro=par_shapes_create_rounded(oct,0.20f,0.006f);
    task_check(rt && ro, "non-orthogonal edge and four-valent corner succeed");
    if (rt) { rt_audit_topology(rt); par_shapes_free_mesh(rt); }
    if (ro) { rt_audit_topology(ro); par_shapes_free_mesh(ro); }
    rt_free_source(tet); rt_free_source(oct);
    rt_free_source(shuffled); rt_free_source(alt); rt_free_source(box);
}

static void rt_error_and_lifetime(void) {
    par_shapes_mesh* s=rt_box(0);
    double L=rt_aabb_diagonal(s);
    float radius=0.22f;
    float error=0.012f*radius;
    size_t pbytes=(size_t)3*s->npoints*sizeof(float);
    size_t ibytes=(size_t)3*s->ntriangles*sizeof(PAR_SHAPES_T);
    float* points_before=(float*)malloc(pbytes);
    PAR_SHAPES_T* indices_before=(PAR_SHAPES_T*)malloc(ibytes);
    memcpy(points_before,s->points,pbytes);
    memcpy(indices_before,s->triangles,ibytes);
    s->normals=(float*)malloc(pbytes);
    for (size_t i=0;i<(size_t)3*s->npoints;++i) s->normals[i]=(float)(i+1);

    par_shapes_mesh* m=par_shapes_create_rounded(s,radius,error);
    task_check(m != NULL, "error-reference box is generated");
    task_check(memcmp(s->points,points_before,pbytes)==0,
        "source point array is unchanged");
    task_check(memcmp(s->triangles,indices_before,ibytes)==0,
        "source index array is unchanged");
    task_check(s->normals != NULL && s->normals[3] == 4.0f,
        "source normals are ignored but preserved");
    if (m) {
        double ff=rt_forward_error(s,m,radius);
        double rr=rt_reverse_error(s,m,radius);
        double allowed=(double)error+2e-6*PAR_MAX(L,radius);
        task_check(ff <= allowed, "sampled triangle interiors meet forward Hausdorff bound");
        task_check(rr <= allowed, "plane/edge/sphere samples meet reverse Hausdorff bound");
        rt_audit_topology(m);

        task_check(m->tcoords == NULL, "rounded mesh has no UVs");
        task_check(m->normals != NULL, "rounded mesh owns normals");
        task_check(m->points != s->points && m->triangles != s->triangles,
            "result has independent arrays");

        /* Free the input before transforming/exporting the result. */
        rt_free_source(s);
        s = NULL;
        par_shapes_translate(m,0.1f,-0.2f,0.3f);
        task_check(isfinite(m->points[0]) && isfinite(m->normals[0]),
            "result remains usable after source release");
        par_shapes_export(m,"/dev/null");
        par_shapes_free_mesh(m);
    }

    free(points_before); free(indices_before);
    if (s) rt_free_source(s);
}

static void rt_bad_inputs_and_failures(void) {
    par_shapes_mesh* good=rt_box(0);
    task_check(par_shapes_create_rounded(NULL,0.2f,0.01f)==NULL,"null source");
    task_check(par_shapes_create_rounded(good,0.0f,0.01f)==NULL,"zero radius");
    task_check(par_shapes_create_rounded(good,-0.2f,0.01f)==NULL,"negative radius");
    task_check(par_shapes_create_rounded(good,0.2f,0.0f)==NULL,"zero error");
    task_check(par_shapes_create_rounded(good,0.2f,-0.01f)==NULL,"negative error");
    task_check(par_shapes_create_rounded(good,NAN,0.01f)==NULL,"NaN radius");
    task_check(par_shapes_create_rounded(good,0.2f,NAN)==NULL,"NaN error");
    task_check(par_shapes_create_rounded(good,0.001f,0.0001f)==NULL,
        "radius outside documented range");
    task_check(par_shapes_create_rounded(good,20.0f,0.01f)==NULL,
        "large radius ratio outside documented range");
    task_check(par_shapes_create_rounded(good,0.2f,0.1f)==NULL,
        "error ratio above documented range");

    par_shapes_mesh* bad=rt_clone(good);
    bad->points[0]=NAN;
    task_check(par_shapes_create_rounded(bad,0.2f,0.01f)==NULL,"non-finite point");
    rt_free_source(bad);

    bad=rt_clone(good);
    bad->triangles[0]=(PAR_SHAPES_T)(good->npoints+3);
    task_check(par_shapes_create_rounded(bad,0.2f,0.01f)==NULL,"bad index");
    rt_free_source(bad);

    bad=rt_clone(good);
    bad->ntriangles=11;
    task_check(par_shapes_create_rounded(bad,0.2f,0.01f)==NULL,"open topology");
    bad->ntriangles=12;
    bad->triangles[1]=bad->triangles[0];
    task_check(par_shapes_create_rounded(bad,0.2f,0.01f)==NULL,"degenerate triangle");
    rt_free_source(bad);

    bad=rt_clone(good);
    PAR_SHAPES_T swap=bad->triangles[1];
    bad->triangles[1]=bad->triangles[2];
    bad->triangles[2]=swap;
    task_check(par_shapes_create_rounded(bad,0.2f,0.01f)==NULL,"reversed face");
    rt_free_source(bad);

    bad=rt_clone(good);
    for (int f=0;f<bad->ntriangles;++f) {
        PAR_SHAPES_T t=bad->triangles[3*f+1];
        bad->triangles[3*f+1]=bad->triangles[3*f+2];
        bad->triangles[3*f+2]=t;
    }
    task_check(par_shapes_create_rounded(bad,0.2f,0.01f)==NULL,"inward shell");
    rt_free_source(bad);

    bad=rt_clone(good);
    bad->points[18]=0.20f; bad->points[19]=0.20f;
    task_check(par_shapes_create_rounded(bad,0.2f,0.01f)==NULL,"non-convex dent");
    rt_free_source(bad);

    bad=rt_clone(good);
    bad->points[3]=bad->points[0];
    bad->points[4]=bad->points[1];
    bad->points[5]=bad->points[2];
    task_check(par_shapes_create_rounded(bad,0.2f,0.01f)==NULL,"duplicate position");
    rt_free_source(bad);

    /* Deterministic allocation-failure recovery. */
    rt_fail_at=0; rt_alloc_calls=0; rt_live_allocations=0;
    par_shapes_mesh* ok=par_shapes_create_rounded(good,0.22f,0.012f);
    task_check(ok != NULL,"allocation-failure baseline succeeds");
    unsigned long calls=rt_alloc_calls;
    par_shapes_free_mesh(ok);
    task_check(rt_live_allocations==0,"successful result frees all intermediates");
    for (unsigned long step=1; step<=calls; ++step) {
        rt_fail_at=step;
        rt_alloc_calls=0;
        rt_live_allocations=0;
        par_shapes_mesh* failed=par_shapes_create_rounded(good,0.22f,0.012f);
        if (failed) par_shapes_free_mesh(failed);
        task_check(failed==NULL,"injected allocation failure returns NULL");
        task_check(rt_live_allocations==0,"failed construction frees every allocation");
    }
    rt_fail_at=0;
    rt_free_source(good);

    /* Index capacity.  A many-sided bipyramid at the finest supported error
       exceeds 16-bit indexing; 32-bit builds should still construct it. */
    par_shapes_mesh* big=rt_bipyramid(72);
    task_check(big != NULL,"capacity fixture exists");
    if (big) {
        par_shapes_mesh* br=par_shapes_create_rounded(big,0.22f,0.00025f);
        if (sizeof(PAR_SHAPES_T) < 4) {
            task_check(br==NULL,"16-bit overflow is rejected, never truncated");
        } else {
            task_check(br!=NULL && br->npoints > 65535,
                "32-bit indexing accepts the large tessellation");
            if (br) par_shapes_free_mesh(br);
        }
        rt_free_source(big);
    }
}

void task_cases(const char* group) {
    if (strcmp(group,"patches")==0) {
        rt_patches();
    } else if (strcmp(group,"topology")==0) {
        rt_topology();
    } else if (strcmp(group,"error")==0) {
        rt_error_and_lifetime();
        rt_bad_inputs_and_failures();
    } else {
        task_check(0,"unknown task group");
    }
}
