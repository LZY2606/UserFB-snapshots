#include "task_support/fixture.h"
#include <math.h>
#include <stdio.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"baseline failure line %d: %s\n",__LINE__,#x);return 1;} } while(0)
int main(void) {
    cc_Mesh *m=task_cube();
    for(int e=0;e<m->edgeCount;e++) CHECK(m->edgeToHalfedgeIDs[e]>m->halfedges[m->edgeToHalfedgeIDs[e]].twinID);
    cc_Subd *a=ccs_Create(m,2),*b=ccs_Create(m,2);
    ccs_Refine_Gather(a);ccs_Refine_Gather(b);
    CHECK(ccm_VertexCountAtDepth(m,1)==26);
    CHECK(ccm_VertexCountAtDepth(m,2)==98);
    for(int i=0;i<8;i++) for(int c=0;c<3;c++)
        CHECK(fabsf(ccs_VertexPoint(a,i,1).array[c]-m->vertexPoints[i].array[c]*5.0f/9.0f)<2e-6f);
    for(int i=0;i<8;i++)for(int c=0;c<3;c++) CHECK(fabsf(ccs_VertexPoint(a,i,2).array[c]-m->vertexPoints[i].array[c]*55.0f/108.0f)<2e-6f);
    puts("BASELINE cube analytical corners PASS");
    int count=ccs_CumulativeVertexCount(a);
    for(int i=0;i<count;i++) for(int c=0;c<3;c++) CHECK(fabsf(a->vertexPoints[i].array[c]-b->vertexPoints[i].array[c])<2e-6f);
    puts("BASELINE gather deterministic two depths PASS");
    float before=ccs_VertexPoint(a,0,2).x;
    m->vertexPoints[0].x-=.4f;
    ccs_Refine_Gather(a);ccs_Refine_Gather(b);
    CHECK(fabsf(ccs_VertexPoint(a,0,2).x-before)>1e-3f);
    for(int i=0;i<count;i++)for(int c=0;c<3;c++) CHECK(fabsf(a->vertexPoints[i].array[c]-b->vertexPoints[i].array[c])<3e-6f);
    CHECK(ccs_HalfedgeVertexUv(a,0,2).u>=0);
    puts("BASELINE cage edit changes production subdivision PASS");
    ccs_Release(a);ccs_Release(b);ccm_Release(m);return 0;
}
