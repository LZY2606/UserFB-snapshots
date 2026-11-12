#include "lcms2.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"baseline failure: %s\n",#x); exit(1);} } while(0)
int main(void) {
    float table[3*3*3*3], a[3]={.2f,.3f,.4f}, b[3]={.8f,.3f,.4f};
    float fa[3],fb[3], ra[3],rb[3],target[3]={.4f,.3f,.4f};
    unsigned x,y,z,k=0;
    cmsPipeline *p=cmsPipelineAlloc(NULL,3,3);
    cmsStage *s;
    CHECK(p);
    for(x=0;x<3;x++) for(y=0;y<3;y++) for(z=0;z<3;z++) {
        table[k++]=x==1?1.f:0.f; table[k++]=y*.5f; table[k++]=z*.5f;
    }
    s=cmsStageAllocCLutFloat(NULL,3,3,3,table); CHECK(s);
    CHECK(cmsPipelineInsertStage(p,cmsAT_END,s));
    cmsPipelineEvalFloat(a,fa,p); cmsPipelineEvalFloat(b,fb,p);
    for(k=0;k<3;k++) {CHECK(fabsf(fa[k]-target[k])<1e-6f); CHECK(fabsf(fb[k]-target[k])<1e-6f);}
    CHECK(cmsPipelineEvalReverseFloat(target,ra,a,p));
    CHECK(cmsPipelineEvalReverseFloat(target,rb,b,p));
    CHECK(fabsf(ra[0]-.2f)<1e-5f && fabsf(rb[0]-.8f)<1e-5f);
    printf("FOLD_FORWARD a=(%.6f,%.6f,%.6f) b=(%.6f,%.6f,%.6f)\n",fa[0],fa[1],fa[2],fb[0],fb[1],fb[2]);
    printf("LEGACY_HINT_BRANCHES %.6f %.6f\n",ra[0],rb[0]);
    target[0]=1.2f;
    CHECK(cmsPipelineEvalReverseFloat(target,ra,a,p));
    cmsPipelineEvalFloat(ra,fa,p);
    CHECK(fabsf(fa[0]-target[0])>.1f);
    printf("LEGACY_UNREACHABLE returns=1 residual=%.6f\n",fabsf(fa[0]-target[0]));
    cmsPipelineFree(p);
    puts("LCMS_FOLD_FORWARD_AND_LEGACY_OK");
    return 0;
}
