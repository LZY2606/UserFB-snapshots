#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#if defined(__APPLE__) && defined(__aarch64__)
/* Old Apple intrinsic spelling used by this upstream snapshot. */
#define vtbl2q_u8 vqtbl2_u8
#endif
#define MINIH264_IMPLEMENTATION
#include "minih264e.h"
int main(int argc,char **argv) {
    assert(argc==3); H264E_create_param_t p={0}; H264E_run_param_t r={0};
    p.width=p.height=64; p.gop=30; p.enableNEON=1; p.num_layers=1;
    int np,ns; assert(!H264E_sizeof(&p,&np,&ns));
    void *enc=aligned_alloc(64,(np+63)&~63),*scratch=aligned_alloc(64,(ns+63)&~63);
    unsigned char *img=aligned_alloc(64,6144); assert(enc&&scratch&&img);
    assert(!H264E_init(enc,&p));
    H264E_io_yuv_t f={{img,img+4096,img+5120},{64,32,32}};
    FILE *out=fopen(argv[1],"wb"),*ref=fopen(argv[2],"wb"); assert(out&&ref);
    r.encode_speed=5; r.qp_min=24;r.qp_max=24;r.desired_frame_bytes=2000;
    for(int t=0;t<3;t++) {
        for(int y=0;y<64;y++) for(int x=0;x<64;x++) img[y*64+x]=(x*13+y*7+t*17)&255;
        memset(img+4096,128,2048); unsigned char *coded; int n;
        assert(!H264E_encode(enc,scratch,&r,&f,&coded,&n)); assert(n>0);
        assert(fwrite(coded,1,n,out)==n); assert(fwrite(img,1,6144,ref)==6144);
    }
    fclose(out);fclose(ref);free(img);free(scratch);free(enc);
    puts("encoded 3 AVC frames at 64x64");
}
