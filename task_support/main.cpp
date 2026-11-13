#include "checks.h"
unsigned long task_checks=0;
int main(int argc,char**argv) {
 if(argc!=2)return 3;
 task_run(argv[1]);
 if(!task_checks)return 4;
 std::printf("TASK_GROUP %s checks=%lu complete\n",argv[1],task_checks);
}
