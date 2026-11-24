#include "checks.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned long checks;
void task_check(int value, const char* message) {
    if (!value) { fprintf(stderr,"CHECK_FAILED %s\n",message); exit(2); }
    ++checks;
}
int main(int argc, char** argv) {
    if (argc!=2) return 3;
    printf("UNIT_BEGIN %s\n",argv[1]);
    fflush(stdout);
    task_cases(argv[1]);
    if (!checks) { fputs("ZERO_CHECKS\n",stderr); return 6; }
    printf("UNIT_GROUP %s checks=%lu DONE\n",argv[1],checks);
    return 0;
}
