#include <stdio.h>
int contract_analytic(void);
int contract_ambiguous(void);
int contract_affine(void);
int contract_storage(void);
int main(void) { int n;
  n=contract_analytic(); if(n<=0) { fprintf(stderr,"FAIL analytic count=%d\n",n); return 1; } printf("PASS analytic %d\n",n);
  n=contract_ambiguous(); if(n<=0) { fprintf(stderr,"FAIL ambiguous count=%d\n",n); return 1; } printf("PASS ambiguous %d\n",n);
  n=contract_affine(); if(n<=0) { fprintf(stderr,"FAIL affine count=%d\n",n); return 1; } printf("PASS affine %d\n",n);
  n=contract_storage(); if(n<=0) { fprintf(stderr,"FAIL storage count=%d\n",n); return 1; } printf("PASS storage %d\n",n);
return 0; }
