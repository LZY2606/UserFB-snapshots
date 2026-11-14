/* MIT; author-side input fixture, not an edit-plan implementation. */
#ifndef EDIT_FIXTURE_H
#define EDIT_FIXTURE_H
#include "CatmullClark.h"
#include <stdlib.h>
static cc_Mesh *task_cube(void) {
    const int faces[6][4]={{0,2,3,1},{4,5,7,6},{0,1,5,4},{2,6,7,3},{0,4,6,2},{1,3,7,5}};
    cc_Mesh *m=ccm_Create(8,24,24,12,6);
    for(int i=0;i<8;i++) {
        m->vertexPoints[i]=(cc_VertexPoint){.x=0,.y=0,.z=0};
        m->vertexPoints[i].x=(i&1)?1:-1;
        m->vertexPoints[i].y=(i&2)?1:-1;
        m->vertexPoints[i].z=(i&4)?1:-1;
        m->vertexToHalfedgeIDs[i]=-1;
    }
    int edge=0;
    for(int f=0;f<6;f++) {
        m->faceToHalfedgeIDs[f]=4*f;
        for(int j=0;j<4;j++) {
            int h=4*f+j;
            cc_Halfedge *e=&m->halfedges[h];
            e->vertexID=faces[f][j]; e->faceID=f;e->nextID=4*f+(j+1)%4;
            e->prevID=4*f+(j+3)%4;e->twinID=-1;e->edgeID=-1;e->uvID=h;
            m->vertexToHalfedgeIDs[e->vertexID]=h;
            m->uvs[h].u=(j==1||j==2);m->uvs[h].v=(j>=2);
        }
    }
    for(int h=0;h<24;h++) if(m->halfedges[h].edgeID<0) {
        int a=m->halfedges[h].vertexID,b=m->halfedges[m->halfedges[h].nextID].vertexID;
        for(int t=h+1;t<24;t++) if(m->halfedges[t].vertexID==b && m->halfedges[m->halfedges[t].nextID].vertexID==a) {
            m->halfedges[h].twinID=t;m->halfedges[t].twinID=h;
            m->halfedges[h].edgeID=m->halfedges[t].edgeID=edge;
            break;
        }
        m->edgeToHalfedgeIDs[edge]=h;
        m->creases[edge].nextID=m->creases[edge].prevID=edge;
        m->creases[edge++].sharpness=0;
    }
    if(edge!=12) abort();
    return m;
}
#endif
