#include <stdio.h>
int contract_fixed(void);
int contract_islands(void);
int contract_repack(void);
int contract_c_api(void);
int main(void) { int n;
  n=contract_fixed(); if(n<=0) { fprintf(stderr,"FAIL fixed count=%d\n",n); return 1; } printf("PASS fixed %d\n",n);
  n=contract_islands(); if(n<=0) { fprintf(stderr,"FAIL islands count=%d\n",n); return 1; } printf("PASS islands %d\n",n);
  n=contract_repack(); if(n<=0) { fprintf(stderr,"FAIL repack count=%d\n",n); return 1; } printf("PASS repack %d\n",n);
  n=contract_c_api(); if(n<=0) { fprintf(stderr,"FAIL c_api count=%d\n",n); return 1; } printf("PASS c_api %d\n",n);
return 0; }
