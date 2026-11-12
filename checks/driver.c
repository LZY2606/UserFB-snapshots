#include <stdio.h>
int contract_transport(void);
int contract_closure(void);
int contract_surface(void);
int contract_limits(void);
int main(void) { int n;
  n=contract_transport(); if(n<=0) { fprintf(stderr,"FAIL transport count=%d\n",n); return 1; } printf("PASS transport %d\n",n);
  n=contract_closure(); if(n<=0) { fprintf(stderr,"FAIL closure count=%d\n",n); return 1; } printf("PASS closure %d\n",n);
  n=contract_surface(); if(n<=0) { fprintf(stderr,"FAIL surface count=%d\n",n); return 1; } printf("PASS surface %d\n",n);
  n=contract_limits(); if(n<=0) { fprintf(stderr,"FAIL limits count=%d\n",n); return 1; } printf("PASS limits %d\n",n);
return 0; }
