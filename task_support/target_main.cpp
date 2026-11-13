#include <cstdio>
extern int test_rtx_sdp(),test_rtx_h264_recovery(),test_rtx_stream_isolation();
int main(){int n=0,r;r=test_rtx_sdp();if(r<=0)return 1;n+=r;puts("rtx_sdp=PASS");r=test_rtx_h264_recovery();if(r<=0)return 2;n+=r;puts("rtx_h264_recovery=PASS");r=test_rtx_stream_isolation();if(r<=0)return 3;n+=r;puts("rtx_stream_isolation=PASS");printf("target_assertions=%d\n",n);return 0;}
