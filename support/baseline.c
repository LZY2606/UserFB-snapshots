#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define JC_VORONOI_IMPLEMENTATION
#include "jc_voronoi.h"
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"baseline failure line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
int main(void) {
    jcv_rect box={{0,0},{1,1}};
    jcv_point points[]={{.15,.2},{.8,.25},{.5,.85}};
    jcv_diagram d; memset(&d,0,sizeof(d));
    jcv_diagram_generate(3,points,&box,0,&d);
    CHECK(d.numsites==3);
    const jcv_site* sites=jcv_diagram_get_sites(&d);
    double total=0; int edges=0, internal=0;
    for(int i=0;i<d.numsites;++i) {
        jcv_edge_iter it; jcv_edge e; double a2=0; int count=0;
        jcv_site_get_edges(&d,&sites[i],&it);
        while(jcv_edge_next(&it,&e)) {
            a2+=e.pos[0].x*e.pos[1].y-e.pos[1].x*e.pos[0].y;
            if(e.sites[1]) ++internal;
            ++count;
        }
        CHECK(count>=3); CHECK(a2>0); total+=a2/2; edges+=count;
    }
    CHECK(fabs(total-1)<1e-8); CHECK(internal>0);
    printf("ORIGINAL_DIAGRAM_OK sites=%d directed_edges=%d area=%.9f\n",d.numsites,edges,total);
    jcv_diagram_free(&d);
    jcv_point images[9]; int k=0;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) images[k++]=(jcv_point){.18+x,.31+y};
    jcv_rect cover={{-1,-1},{2,2}}; memset(&d,0,sizeof(d));
    jcv_diagram_generate(9,images,&cover,0,&d);
    sites=jcv_diagram_get_sites(&d); int found=0,central_edges=0;
    for(int i=0;i<d.numsites;++i) if(sites[i].index==4) {
        jcv_edge_iter it; jcv_edge e; found=1;
        jcv_site_get_edges(&d,&sites[i],&it);
        while(jcv_edge_next(&it,&e)) { CHECK(e.sites[1]!=NULL); ++central_edges; }
    }
    CHECK(found); CHECK(central_edges==4);
    printf("IMAGE_SWEEP_PRIMITIVE_OK central_edges=%d; no periodic quotient API supplied\n",central_edges);
    jcv_diagram_free(&d);
    return 0;
}
