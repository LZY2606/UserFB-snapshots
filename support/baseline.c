#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define PAR_SHAPES_IMPLEMENTATION
#include "par_shapes.h"
static int checks;
#define CHECK(x) do{if(!(x)){fprintf(stderr,"baseline failure line %d: %s\n",__LINE__,#x);exit(1);}++checks;}while(0)
static void wave(float const* uv,float* p,void* unused){(void)unused;p[0]=uv[0];p[1]=uv[1];p[2]=.07f*sinf(6.2831853f*uv[0])*sinf(6.2831853f*uv[1]);}
int main(void){
 par_shapes_mesh* p=par_shapes_create_parametric(wave,8,6,0);CHECK(p);CHECK(p->npoints==63);CHECK(p->ntriangles==96);CHECK(p->normals&&p->tcoords);
 for(int i=0;i<p->npoints;i++){float q[3];wave(p->tcoords+2*i,q,0);for(int j=0;j<3;j++)CHECK(fabsf(q[j]-p->points[3*i+j])<1e-6f);}
 for(int i=0;i<3*p->ntriangles;i++)CHECK(p->triangles[i]<p->npoints);
 par_shapes_export(p,"build/parametric.obj");par_shapes_free_mesh(p);
 p=par_shapes_create_torus(12,8,.3f);CHECK(p);CHECK(p->npoints==117);CHECK(p->ntriangles==192);
 for(int row=0;row<=8;row++)for(int j=0;j<3;j++)CHECK(fabsf(p->points[3*(row*13)+j]-p->points[3*(row*13+12)+j])<1e-5f);
 par_shapes_export(p,"build/torus.obj");par_shapes_free_mesh(p);
 printf("BASELINE uniform-parametric torus UV OK checks=%d\n",checks);return 0;
}
