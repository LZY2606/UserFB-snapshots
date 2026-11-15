#include <stdio.h>
extern int case_scans(void);
extern int case_refinement(void);
extern int case_geometry(void);
extern int case_compatibility(void);
int main(void) { int n;
n=case_scans(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED scans\n");return 31;} printf("TARGET_CASE scans assertions=%d PASS\n",n);
n=case_refinement(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED refinement\n");return 31;} printf("TARGET_CASE refinement assertions=%d PASS\n",n);
n=case_geometry(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED geometry\n");return 31;} printf("TARGET_CASE geometry assertions=%d PASS\n",n);
n=case_compatibility(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED compatibility\n");return 31;} printf("TARGET_CASE compatibility assertions=%d PASS\n",n);
puts("TARGET_OK groups=4");return 0;}
