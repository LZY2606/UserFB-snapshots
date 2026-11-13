#include <stdio.h>
#include <assert.h>
#define PAR_SHAPES_IMPLEMENTATION
#include "par_shapes.h"
int main(void) {
 par_shapes_mesh *m=par_shapes_create_plane(3,2);
 assert(m && m->npoints==12 && m->ntriangles==12 && m->normals && m->tcoords);
 int edges=0;
 for(int t=0;t<m->ntriangles;t++)for(int e=0;e<3;e++){
  unsigned a=m->triangles[t*3+e],b=m->triangles[t*3+(e+1)%3]; int mates=0;
  for(int u=0;u<m->ntriangles;u++)for(int j=0;j<3;j++)
   if(m->triangles[u*3+j]==b&&m->triangles[u*3+(j+1)%3]==a)mates++;
  assert(mates<=1); if(!mates)edges++;
 }
 assert(edges==10);
 printf("PLANE points=%d faces=%d boundary=%d normals=present uv=present\n",m->npoints,m->ntriangles,edges);
 par_shapes_free_mesh(m);
 m=par_shapes_create_torus(8,5,0.25f);assert(m && m->ntriangles==80);
 printf("TORUS faces=%d existing-generator=ok\n",m->ntriangles);par_shapes_free_mesh(m);
 return 0;
}
