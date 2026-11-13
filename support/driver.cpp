#include "check.h"
int task_assertions;
int main(int argc,char** argv){if(argc!=2)return 2;task_cases(argv[1]);if(task_assertions<5){fprintf(stderr,"insufficient assertions\n");return 1;}printf("PASS %s checks=%d\n",argv[1],task_assertions);return 0;}
