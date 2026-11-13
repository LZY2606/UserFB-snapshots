#include <stdio.h>
extern int normal_h264(void);
extern int test_h264_don_order(void);
extern int test_h264_interleaved_fragments(void);
extern int test_h264_mtap_timestamps(void);
int main(void){int n=0,r;if(!normal_h264())return 1;r=test_h264_don_order();if(r<=0)return 2;n+=r;puts("don_order=PASS");r=test_h264_interleaved_fragments();if(r<=0)return 3;n+=r;puts("interleaved_fragments=PASS");r=test_h264_mtap_timestamps();if(r<=0)return 4;n+=r;puts("mtap_timestamps=PASS");printf("target_assertions=%d\n",n);return 0;}
