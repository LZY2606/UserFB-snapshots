#include "check.h"
#include <stdio.h>
int task_check(int v,const char*e,const char*f,int l){return v;}
#include "../tests/location_views_contract.c"
int main(void){static struct vrec rs[64]; int n=0; fprintf(stderr,"plain start\n"); int r=collect_path(".task-build/plain.elf","plain",rs,64,&n); fprintf(stderr,"plain r=%d n=%d\n",r,n); for(int i=0;i<n;i++) fprintf(stderr,"%d %s %llx %s\n",i,rs[i].name,(unsigned long long)rs[i].addroff,rs[i].description); return 0;}
