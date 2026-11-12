#include <stdio.h>
#include <stdlib.h>
#include "check.h"
void aes_vectors(void);
void aes_lanes(void);
void aes_ctr(void);
static int checks,failed;
void unit_check(int ok,const char *expr,const char *file,int line){
    ++checks;if(!ok){++failed;fprintf(stderr,"%s:%d failed: %s\n",file,line,expr);}
}
int main(void){
    void(*groups[])(void)={aes_vectors,aes_lanes,aes_ctr};
    const char*names[]={"aes-vectors","aes-lanes","aes-ctr"};
    int i;
    for(i=0;i<3;i++){checks=0;groups[i]();if(checks==0||failed){fprintf(stderr,"%s empty/failed\n",names[i]);return 1;}printf("PASS %s checks=%d\n",names[i],checks);}
    puts("TINYCRYPT_BITSLICE_TARGET_OK");return 0;
}
