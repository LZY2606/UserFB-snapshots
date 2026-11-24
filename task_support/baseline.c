#include "lz4frame.h"
#include <stdio.h>
#include <string.h>
int main(void){
 const char in[]="frame baseline frame baseline";char packed[256],out[256];
 LZ4F_preferences_t p=LZ4F_INIT_PREFERENCES;LZ4F_dctx*d=NULL;
 p.frameInfo.contentChecksumFlag=LZ4F_contentChecksumEnabled;
 size_t n=LZ4F_compressFrame(packed,sizeof(packed),in,sizeof(in),&p),a=n,b=sizeof(out),r;
 if(LZ4F_isError(n)||LZ4F_isError(LZ4F_createDecompressionContext(&d,LZ4F_VERSION)))return 1;
 r=LZ4F_decompress(d,out,&b,packed,&a,NULL);LZ4F_freeDecompressionContext(d);
 if(r||a!=n||b!=sizeof(in)||memcmp(in,out,b))return 2;
 puts("LZ4_BASELINE_ROUNDTRIP_OK");return 0;
}
