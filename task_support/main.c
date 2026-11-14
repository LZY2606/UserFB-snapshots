#include <stdio.h>
#include <stdlib.h>
#include "check.h"
static unsigned long checks;
void task_check(int ok,const char *m,const char *f,int l){++checks;if(!ok){fprintf(stderr,"CONTENT_MISMATCH %s:%d %s\n",f,l,m);exit(42);}}
int main(int argc,char **argv){if(argc!=2)return 64;printf("BEGIN %s\n",argv[1]);fflush(stdout);if(task_group(argv[1])!=1||checks==0)return 65;printf("DONE %s checks=%lu\n",argv[1],checks);puts("PROCESS_COMPLETE");return 0;}
