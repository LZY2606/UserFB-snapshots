#include "common.h"
#include "structs.h"
#include "aac_fixture.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#define REQUIRE(x) do {if(!(x)){fprintf(stderr,"baseline failure line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static double energy(const real_t *p) {
    size_t i; double e=0; REQUIRE(p);
    for(i=0;i<1024;++i) e+=(double)p[i]*p[i];
    return e;
}
static NeAACDecHandle open_decoder(void) {
    const struct fixture_element es[]={{0,0},{1,0}};
    unsigned char asc[64];size_t n=fixture_asc(asc,sizeof asc,es,2);
    unsigned long rate=0;unsigned char channels=0;
    NeAACDecHandle h=NeAACDecOpen();NeAACDecConfigurationPtr c;
    REQUIRE(n&&h);c=NeAACDecGetCurrentConfiguration(h);
    c->outputFormat=FAAD_FMT_FLOAT;c->downMatrix=0;c->dontUpSampleImplicitSBR=1;
    REQUIRE(NeAACDecSetConfiguration(h,c));
    REQUIRE(NeAACDecInit2(h,asc,(unsigned long)n,&rate,&channels)==0);
    REQUIRE(rate==48000&&channels==3);return h;
}
int main(void) {
    const struct fixture_element es[]={{0,0},{1,0}},reverse[]={{1,0},{0,0}};
    const unsigned seq[]={0,1,2,2,3,0};
    unsigned i; unsigned char packet[256];size_t n;NeAACDecFrameInfo info;
    NeAACDecHandle h=open_decoder();NeAACDecStruct *state=(NeAACDecStruct*)h;
    for(i=0;i<6;++i) {
        void *pcm;unsigned long j;double pe=0;
        n=fixture_packet(packet,sizeof packet,es,2,seq[i],i%2,140+i);
        REQUIRE(n);pcm=NeAACDecDecode(h,&info,packet,(unsigned long)n);
        REQUIRE(pcm&&info.error==0&&info.channels==3);
        REQUIRE(info.samples==(i?3072u:0u));
        for(j=0;j<info.samples;++j) pe+=(double)((float*)pcm)[j]*((float*)pcm)[j];
        REQUIRE(energy(state->fb_intermed[0])>0&&energy(state->fb_intermed[1])>0);
        if(i) REQUIRE(pe>0);
        printf("AAC ordinary frame=%u sequence=%u samples=%lu pcm_energy=%.9g overlap=%.9g shape=%u\n",i,seq[i],info.samples,pe,energy(state->fb_intermed[1]),state->window_shape_prev[1]);
        if(!i) {REQUIRE(energy(state->time_out[0])>0);puts("COLD first frame: samples=0, synthesis time_out is nonzero");}
    }
    n=fixture_packet(packet,sizeof packet,reverse,2,0,0,146);REQUIRE(n);
    NeAACDecDecode(h,&info,packet,(unsigned long)n);
    REQUIRE(info.error==21);
    printf("CURRENT reordered SCE0/CPE0: error=%u (%s)\n",info.error,NeAACDecGetErrorMessage(info.error));
    NeAACDecClose(h);
    puts("CURRENT no frame-boundary PCE update API; Init2 is initialization");
    return 0;
}
