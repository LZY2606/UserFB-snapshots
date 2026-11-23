#include "par_shapes.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static void check(int b, const char* m) {
    if (!b) { fprintf(stderr, "BASELINE_FAIL %s\n",m); exit(2); }
}
static double volume(const par_shapes_mesh* m) {
    double v=0;
    for (int f=0;f<m->ntriangles;f++) {
        const float *a=m->points+3*m->triangles[3*f],
            *b=m->points+3*m->triangles[3*f+1], *c=m->points+3*m->triangles[3*f+2];
        v+=(double)a[0]*(b[1]*c[2]-b[2]*c[1])+(double)a[1]*(b[2]*c[0]-b[0]*c[2])+(double)a[2]*(b[0]*c[1]-b[1]*c[0]);
    }
    return v/6;
}
int main(void) {
    par_shapes_mesh *m=par_shapes_create_cube();
    check(m && m->npoints==8 && m->ntriangles==12,"cube counts");
    check(fabs(volume(m)-1)<1e-7,"cube orientation and volume");
    par_shapes_scale(m,2,3,4); par_shapes_translate(m,-3,2,-1);
    check(fabs(volume(m)-24)<1e-7,"transformed cube volume");
    par_shapes_compute_normals(m);
    for (int i=0;i<3*m->npoints;i++) check(isfinite(m->normals[i]),"normals");
    par_shapes_mesh* c=par_shapes_clone(m,NULL);
    check(c && c->points!=m->points && fabs(volume(c)-24)<1e-7,"independent clone");
    par_shapes_free_mesh(c); par_shapes_free_mesh(m);
    m=par_shapes_create_torus(12,8,0.3f);
    check(m && m->npoints==117 && m->ntriangles==192,"torus counts");
    c=par_shapes_weld(m,0.001f,NULL);
    check(c && c->npoints==96 && c->ntriangles==192,"existing weld");
    check(volume(c)>0,"welded torus orientation");
    par_shapes_free_mesh(c); par_shapes_free_mesh(m);
    puts("MESH_BASELINE cube_volume=24 torus=117/192 welded=96/192 clone=independent");
    return 0;
}
