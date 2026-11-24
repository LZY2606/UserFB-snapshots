#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "tesselator.h"
static double area(int reverse) {
  TESStesselator *t=tessNewTess(NULL);
  TESSreal a[]={0,0,4,0,4,4,0,4};
  TESSreal b[]={2,0,6,0,6,4,2,4},n[]={0,0,1};
  if(reverse) {TESSreal r[]={0,4,4,4,4,0,0,0};for(int i=0;i<8;i++)b[i]=r[i];}
  tessAddContour(t,2,a,2*sizeof(TESSreal),4);tessAddContour(t,2,b,2*sizeof(TESSreal),4);
  assert(tessTesselate(t,TESS_WINDING_NONZERO,TESS_CONNECTED_POLYGONS,3,2,n));
  const TESSreal *v=tessGetVertices(t);const TESSindex *e=tessGetElements(t);double sum=0;
  for(int i=0;i<tessGetElementCount(t);i++) {
    int x=e[i*6]*2,y=e[i*6+1]*2,z=e[i*6+2]*2;
    sum+=fabs((v[y]-v[x])*(v[z+1]-v[x+1])-(v[y+1]-v[x+1])*(v[z]-v[x]))*0.5;
  }
  tessDeleteTess(t);return sum;
}
static void skinny(void) {
  TESStesselator *t=tessNewTess(NULL); TESSreal v[]={0,0,20,0,20,1,0,1},n[]={0,0,1};
  tessAddContour(t,2,v,2*sizeof(TESSreal),4);tessSetOption(t,TESS_CONSTRAINED_DELAUNAY_TRIANGULATION,1);
  assert(tessTesselate(t,TESS_WINDING_NONZERO,TESS_POLYGONS,3,2,n));
  assert(tessGetVertexCount(t)==4 && tessGetElementCount(t)==2);
  printf("BASELINE fixed-vertices PASS vertices=4 triangles=2 minimum-angle=%.6f\n",atan(1.0/20)*180/acos(-1.0));
  tessDeleteTess(t);
}
int main(void) {
  double overlap=area(0),cancel=area(1);
  assert(fabs(overlap-24)<1e-5);assert(fabs(cancel)<1e-5);
  printf("BASELINE scalar-winding PASS union=%.0f opposite=%.0f\n",overlap,cancel);skinny();return 0;
}
