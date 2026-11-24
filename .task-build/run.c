#include "check.h"
#include <stdio.h>
#include <string.h>
static unsigned checks;
int task_check(int v,const char*e,const char*f,int l){++checks;if(!v)fprintf(stderr,"FAIL %s:%d %s\n",f,l,e);return v;}
int main(void){setvbuf(stdout,0,_IONBF,0);for(size_t i=0;i<task_case_count;i++){checks=0;printf("RUN %s %s\n",task_cases[i].group,task_cases[i].name);int r=task_cases[i].run();printf("DONE %s r=%d checks=%u\n",task_cases[i].name,r,checks);if(r)return r;}return 0;}
