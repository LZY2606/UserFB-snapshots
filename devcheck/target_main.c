#include <stdio.h>
extern int case_thresholds(void);
extern int case_fifo(void);
extern int case_frames(void);
int main(void) { int n;
n=case_thresholds(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED thresholds\n");return 31;} printf("TARGET_CASE thresholds assertions=%d PASS\n",n);
n=case_fifo(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED fifo\n");return 31;} printf("TARGET_CASE fifo assertions=%d PASS\n",n);
n=case_frames(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED frames\n");return 31;} printf("TARGET_CASE frames assertions=%d PASS\n",n);
puts("TARGET_OK groups=3");return 0;}
