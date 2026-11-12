#include <stdio.h>
extern int case_selection(void);
extern int case_syntax(void);
extern int case_table_switch(void);
extern int case_compatibility(void);
int main(void) { int n;
n=case_selection(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED selection\n");return 31;} printf("TARGET_CASE selection assertions=%d PASS\n",n);
n=case_syntax(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED syntax\n");return 31;} printf("TARGET_CASE syntax assertions=%d PASS\n",n);
n=case_table_switch(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED table_switch\n");return 31;} printf("TARGET_CASE table_switch assertions=%d PASS\n",n);
n=case_compatibility(); if(n<=0) {fprintf(stderr,"EMPTY_OR_SKIPPED compatibility\n");return 31;} printf("TARGET_CASE compatibility assertions=%d PASS\n",n);
puts("TARGET_OK groups=4");return 0;}
