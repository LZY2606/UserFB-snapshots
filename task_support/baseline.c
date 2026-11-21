#include "fixture.h"
#include <assert.h>
#include <stdint.h>
static uint64_t hashbytes(uint64_t h,const unsigned char *p,size_t n){for(size_t i=0;i<n;i++){h^=p[i];h*=UINT64_C(1099511628211);}return h;}
static int run(int mode,int format){
 h261_context_t c;assert(fixture_init(&c,format,mode));
 FILE *out=tmpfile();assert(out);unsigned total=0;
 uint64_t rh=UINT64_C(1469598103934665603);
 for(int f=0;f<3;f++){
  fixture_fill(&c,f);unsigned bits=fixture_encode(&c,f,out);assert(bits>0);total+=bits;
  size_t pixels=(size_t)c.s_reconstructed_frame.i_width*c.s_reconstructed_frame.i_height;
  rh=hashbytes(rh,c.s_reconstructed_frame.pui8_Y,pixels);rh=hashbytes(rh,c.s_reconstructed_frame.pui8_Cb,pixels/4);rh=hashbytes(rh,c.s_reconstructed_frame.pui8_Cr,pixels/4);
 }
 UInt8 *tail;UInt32 len;h261_bitstream_flush(&c.s_bitstream,&tail,&len);fwrite(tail,1,len,out);
 assert((unsigned)ftell(out)==(total+7)/8);
 rewind(out);uint64_t sh=UINT64_C(1469598103934665603);int b;while((b=fgetc(out))!=EOF){unsigned char ch=(unsigned char)b;sh=hashbytes(sh,&ch,1);}
 printf("H261_BASELINE mode=%d format=%d frames=3 bits=%u stream=%016llx recon=%016llx\n",mode,format,total,(unsigned long long)sh,(unsigned long long)rh);
 fclose(out);fixture_free(&c);return 0;
}
int main(void){run(H261_RATECTRL_MODE_QUANT,H261_SOURCE_FORMAT_QCIF);run(H261_RATECTRL_MODE_VBR,H261_SOURCE_FORMAT_QCIF);run(H261_RATECTRL_MODE_QUANT,H261_SOURCE_FORMAT_CIF);puts("H261_REAL_ENCODING_BASELINE_OK");return 0;}
