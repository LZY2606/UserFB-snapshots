#include <stdio.h>
extern int case_arithmetic(void);
extern int case_stream(void);
extern int case_parameters(void);
int main(void) { int n;
n=case_arithmetic(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED arithmetic\n");return 31;} printf("TARGET_CASE arithmetic assertions=%d PASS\n",n);
n=case_stream(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED stream\n");return 31;} printf("TARGET_CASE stream assertions=%d PASS\n",n);
n=case_parameters(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED parameters\n");return 31;} printf("TARGET_CASE parameters assertions=%d PASS\n",n);
puts("TARGET_OK groups=3");return 0;}
