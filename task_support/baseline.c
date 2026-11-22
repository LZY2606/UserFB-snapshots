#define PDMP3_HEADER_ONLY
#include "pdmp3.c"
#include "frame_fixture.h"
#include <stdio.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"PDMP3_BASELINE_FAIL line=%d %s\n",__LINE__,#x);return 1;} } while(0)
static long magnitude(const unsigned char *p,size_t n) {
    long e=0;for(size_t i=0;i+1<n;i+=2){short v;memcpy(&v,p+i,2);e+=v<0?-(long)v:v;}return e;
}
static int fixed_sound(void) {
    unsigned char input[5000],out[4608];size_t total=0,done=0;int err=0;
    for(unsigned i=0;i<9;i++)total+=fixture_frame(input+total,0,2,i&1,i%3==0,i);
    pdmp3_handle *d=pdmp3_new(NULL,&err);CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    CHECK(pdmp3_feed(d,input,total)==PDMP3_OK);
    int rc=pdmp3_read(d,out,sizeof(out),&done);
    int first_status=rc;size_t first_done=done;long energy=0;long rate=0;int channels=0,encoding=0;
    if(rc==PDMP3_NEW_FORMAT) {
        CHECK(pdmp3_getformat(d,&rate,&channels,&encoding)==PDMP3_OK);
        if(!done)rc=pdmp3_read(d,out,sizeof(out),&done);
    }
    CHECK(done==4608 && magnitude(out,done)>0);energy+=magnitude(out,done);
    CHECK(pdmp3_getformat(d,&rate,&channels,&encoding)==PDMP3_OK);
    CHECK(rate==44100 && channels==2 && encoding==PDMP3_ENC_SIGNED_16);
    rc=pdmp3_read(d,out,sizeof(out),&done);
    CHECK(done==4608 && rc==PDMP3_OK && magnitude(out,done)>0);energy+=magnitude(out,done);
    printf("PDMP3_PRODUCTION frames=2 bytes=9216 energy=%ld first_status=%d first_done=%zu\n",energy,first_status,first_done);
    pdmp3_delete(d);return 0;
}
static int observe_barrier(void) {
    unsigned char input[5000],out[4608];size_t total=0,done;int err=0;
    for(unsigned i=0;i<3;i++)total+=fixture_frame(input+total,0,2,0,0,i);
    for(unsigned i=0;i<6;i++)total+=fixture_frame(input+total,1,1,0,0,i+1);
    pdmp3_handle *d=pdmp3_new(NULL,&err);CHECK(d && pdmp3_open_feed(d)==PDMP3_OK);
    CHECK(pdmp3_feed(d,input,total)==PDMP3_OK);
    int rc=pdmp3_read(d,out,sizeof(out),&done);
    int early=(rc==PDMP3_NEW_FORMAT && done!=0);
    CHECK(rc==PDMP3_NEW_FORMAT || (rc==PDMP3_OK && done>0));
    long rate=0;int channels=0,encoding=0;
    CHECK(pdmp3_getformat(d,&rate,&channels,&encoding)==PDMP3_OK);
    CHECK(rate==44100 && channels==2);
    int silent=0,seen_new=0;size_t produced=done;
    for(unsigned call=0;call<10;call++) {
        rc=pdmp3_read(d,out,sizeof(out),&done);produced+=done;
        CHECK(pdmp3_getformat(d,&rate,&channels,&encoding)==PDMP3_OK);
        if(rate==48000) {
            seen_new=1;silent=(rc!=PDMP3_NEW_FORMAT);
            printf("PDMP3_SWITCH status=%d done=%zu rate=%ld channels=%d\n",rc,done,rate,channels);
            break;
        }
    }
    CHECK(seen_new && produced>0);
    printf("PDMP3_OBSERVED data_before_notice=%d silent_format_change=%d\n",early,silent);
    pdmp3_delete(d);return 0;
}
int main(void) {
    if(fixed_sound() || observe_barrier())return 1;
    puts("PDMP3_BASELINE_OK fixed_sound=nonzero switch=observed");return 0;
}
