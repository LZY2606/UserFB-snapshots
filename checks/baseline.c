#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "par_shapes.h"
int main(void) {
 par_shapes_mesh *m=par_shapes_create_torus(12,20,0.25f);
 assert(m && m->npoints==273 && m->ntriangles==480);
 for(int i=0;i<m->npoints;i++) {
  float *n=m->normals+3*i;
  assert(isfinite(n[0]) && fabsf(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]-1)<0.002f);
 }
 par_shapes_mesh *w=par_shapes_weld(m,0.001f,0);
 assert(w && w->npoints < m->npoints && w->ntriangles==480);
 par_shapes_free_mesh(w); par_shapes_free_mesh(m);
 puts("PAR_TORUS_WELD_OK vertices=273 triangles=480");
}
