#include "check.h"
unsigned task_checks;
int main(void) { task_cases(); if (!task_checks) { fputs("no assertions\n",stderr);return 3; } printf("TARGET checks=%u\n",task_checks);return 0; }
