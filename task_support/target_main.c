#include <stdio.h>
extern int normal_rtmp(void),test_rtmp_write_resume(void),test_rtmp_write_headers(void),test_rtmp_write_lifecycle(void);
int main(void){int n=0,r;if(!normal_rtmp())return 1;r=test_rtmp_write_resume();if(r<=0)return 2;n+=r;puts("write_resume=PASS");r=test_rtmp_write_headers();if(r<=0)return 3;n+=r;puts("write_headers=PASS");r=test_rtmp_write_lifecycle();if(r<=0)return 4;n+=r;puts("write_lifecycle=PASS");printf("target_assertions=%d\n",n);return 0;}
