#include <stdio.h>
extern int contract_structure(void);
extern int contract_flow(void);
extern int contract_captures(void);
extern int contract_compat(void);
int main(void) {
 { int n=contract_structure(); if(n<=0){fprintf(stderr,"empty/failed group: structure\n");return 1;} printf("PASS structure assertions=%d\n",n); }
 { int n=contract_flow(); if(n<=0){fprintf(stderr,"empty/failed group: flow\n");return 1;} printf("PASS flow assertions=%d\n",n); }
 { int n=contract_captures(); if(n<=0){fprintf(stderr,"empty/failed group: captures\n");return 1;} printf("PASS captures assertions=%d\n",n); }
 { int n=contract_compat(); if(n<=0){fprintf(stderr,"empty/failed group: compat\n");return 1;} printf("PASS compat assertions=%d\n",n); }
 puts("CHUNK_VERIFY_CONTRACT_OK");return 0;
}
