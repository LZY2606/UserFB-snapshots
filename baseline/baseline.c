#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "tesselator.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"baseline failed line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static void run(const char *name, const float *points, int n, int copies, int rule, int want_contours, double want_area, int want_generated) {
    TESStesselator *t = tessNewTess(NULL);
    const float normal[] = {0,0,1};
    CHECK(t);
    for(int i=0;i<copies;i++) tessAddContour(t,2,points,2*sizeof(float),n);
    CHECK(tessTesselate(t,rule,TESS_BOUNDARY_CONTOURS,3,2,normal));
    const float *v=tessGetVertices(t);
    const int *e=tessGetElements(t), *ix=tessGetVertexIndices(t);
    double area=0; int generated=0;
    for(int c=0;c<tessGetElementCount(t);c++) {
        int b=e[2*c], count=e[2*c+1];
        for(int j=0;j<count;j++) {
            int k=b+j, next=b+(j+1)%count;
            area += ((double)v[2*k]*v[2*next+1]-(double)v[2*k+1]*v[2*next])/2;
            generated += ix[k]==TESS_UNDEF;
        }
    }
    CHECK(tessGetElementCount(t)==want_contours);
    CHECK(fabs(area-want_area)<1e-6);
    CHECK(generated==want_generated);
    printf("BASELINE %s contours=%d vertices=%d generated=%d area=%.1f\n",name,tessGetElementCount(t),tessGetVertexCount(t),generated,area);
    tessDeleteTess(t);
}
static void partial(void) {
    const float a[]={0,0,4,0,4,4,0,4}, b[]={2,0,6,0,6,4,2,4};
    const float normal[]={0,0,1};
    TESStesselator *t=tessNewTess(NULL); CHECK(t);
    tessAddContour(t,2,a,2*sizeof(float),4);
    tessAddContour(t,2,b,2*sizeof(float),4);
    CHECK(tessTesselate(t,TESS_WINDING_NONZERO,TESS_BOUNDARY_CONTOURS,3,2,normal));
    CHECK(tessGetElementCount(t)==1);
    const float *v=tessGetVertices(t); const int *ix=tessGetVertexIndices(t);
    const int *e=tessGetElements(t); double area=0;
    for(int j=0;j<e[1];j++) {
        int k=e[0]+j, next=e[0]+(j+1)%e[1];
        printf("CURRENT_SEGMENT (%.1f,%.1f)->(%.1f,%.1f) vertex_index=%d\n",v[2*k],v[2*k+1],v[2*next],v[2*next+1],ix[k]);
        area+=((double)v[2*k]*v[2*next+1]-(double)v[2*k+1]*v[2*next])/2;
    }
    CHECK(fabs(area-24)<1e-6);
    printf("BASELINE partial-overlap contours=1 vertices=%d area=24.0\n",tessGetVertexCount(t));
    tessDeleteTess(t);
}
int main(void) {
    const float box[]={0,0,4,0,4,4,0,4};
    const float bow[]={0,0,4,4,0,4,4,0};
    run("single",box,4,1,TESS_WINDING_NONZERO,1,16,0);
    run("coincident-nonzero",box,4,2,TESS_WINDING_NONZERO,1,16,0);
    run("coincident-odd",box,4,2,TESS_WINDING_ODD,0,0,0);
    run("crossing",bow,4,1,TESS_WINDING_ODD,2,8,2);
    partial();
    puts("LIBTESS_CURRENT_GEOMETRY_OK");
    return 0;
}
