#include <stdio.h>
int contract_integration(void);
int contract_partition(void);
int contract_outputs(void);
int contract_ownership(void);
int main(void) { int n;
  n=contract_integration(); if(n<=0) { fprintf(stderr,"FAIL integration count=%d\n",n); return 1; } printf("PASS integration %d\n",n);
  n=contract_partition(); if(n<=0) { fprintf(stderr,"FAIL partition count=%d\n",n); return 1; } printf("PASS partition %d\n",n);
  n=contract_outputs(); if(n<=0) { fprintf(stderr,"FAIL outputs count=%d\n",n); return 1; } printf("PASS outputs %d\n",n);
  n=contract_ownership(); if(n<=0) { fprintf(stderr,"FAIL ownership count=%d\n",n); return 1; } printf("PASS ownership %d\n",n);
return 0; }
