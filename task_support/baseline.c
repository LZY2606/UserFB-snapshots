#define PAR_SHAPES_IMPLEMENTATION
#include "par_shapes.h"
#include <stdio.h>
#include <math.h>
static double vol(const par_shapes_mesh* m) {
 double v=0;for(int f=0;f<m->ntriangles;f++) {
 const float* a=m->points+3*m->triangles[3*f];
 const float* b=m->points+3*m->triangles[3*f+1];
 const float* c=m->points+3*m->triangles[3*f+2];
 v+=((double)a[0]*(b[1]*c[2]-b[2]*c[1])+(double)a[1]*(b[2]*c[0]-b[0]*c[2])+(double)a[2]*(b[0]*c[1]-b[1]*c[0]))/6;
 }return v;
}
int main(void) {
 par_shapes_mesh* m=par_shapes_create_tetrahedron();
 if(!m || m->npoints!=4 || m->ntriangles!=4) return 1;
 double before=vol(m); par_shapes_invert(m,0,0);double after=vol(m);
 if(fabs(before)<0.01 || fabs(before+after)>1e-7) return 2;
 par_shapes_compute_normals(m);
 for(int i=0;i<m->npoints;i++) {const float* n=m->normals+3*i;
 if(fabs(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]-1)>1e-5) return 3;}
 par_shapes_mesh* c=par_shapes_clone(m,0);par_shapes_translate(c,3,0,0);
 par_shapes_merge(m,c);
 if(m->npoints!=8 || m->ntriangles!=8) return 4;
 printf("BASELINE invert-sign normals-unit merge points=%d faces=%d PASS\n",m->npoints,m->ntriangles);
 par_shapes_free_mesh(c);par_shapes_free_mesh(m);return 0;
}
