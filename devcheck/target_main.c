#include <stdio.h>
extern int case_limit(void);
extern int case_prediction(void);
extern int case_failure(void);
extern int case_compatibility(void);
int main(void) { int n;
n=case_limit(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED limit\n");return 31;} printf("TARGET_CASE limit assertions=%d PASS\n",n);
n=case_prediction(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED prediction\n");return 31;} printf("TARGET_CASE prediction assertions=%d PASS\n",n);
n=case_failure(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED failure\n");return 31;} printf("TARGET_CASE failure assertions=%d PASS\n",n);
n=case_compatibility(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED compatibility\n");return 31;} printf("TARGET_CASE compatibility assertions=%d PASS\n",n);
puts("TARGET_OK groups=4");return 0;}
