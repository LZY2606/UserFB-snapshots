#include <stdio.h>
extern int contract_chunks(void);
extern int contract_nested(void);
extern int contract_callbacks(void);
extern int contract_failure(void);
int main(void){
{int n=contract_chunks();if(n<=0){fprintf(stderr,"empty/failed chunks\n");return 1;}printf("PASS chunks assertions=%d\n",n);}
{int n=contract_nested();if(n<=0){fprintf(stderr,"empty/failed nested\n");return 1;}printf("PASS nested assertions=%d\n",n);}
{int n=contract_callbacks();if(n<=0){fprintf(stderr,"empty/failed callbacks\n");return 1;}printf("PASS callbacks assertions=%d\n",n);}
{int n=contract_failure();if(n<=0){fprintf(stderr,"empty/failed failure\n");return 1;}printf("PASS failure assertions=%d\n",n);}
puts("RESP3_STREAM_CONTRACT_OK");return 0;}
