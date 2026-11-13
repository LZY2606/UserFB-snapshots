#include "check.h"
unsigned long task_assertions;
int main(void) {
 if(!task_case_count) return 65;
 for(size_t i=0;i<task_case_count;++i) { unsigned long before=task_assertions; if(!task_cases[i].name||!task_cases[i].run) return 66; task_cases[i].run(); if(task_assertions==before) return 67; printf("PASS %s\n",task_cases[i].name); }
 printf("CASES=%zu ASSERTIONS=%lu\n",task_case_count,task_assertions); return 0;
}
