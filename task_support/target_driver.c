#include <stdio.h>
#include <stdlib.h>
#include "case_harness.h"
static unsigned cases, checks;
void unit_case_begin(void) { ++cases; }
void unit_check(int ok,const char *expr,const char *file,int line) {
    ++checks;
    if(!ok) {fprintf(stderr,"FAIL %s:%d %s\n",file,line,expr);exit(1);}
}
static void group(const char *name,void (*fn)(void)) {
    cases=checks=0;fn();
    if(!cases || checks<cases) {fprintf(stderr,"EMPTY_GROUP %s\n",name);exit(2);}
    printf("GROUP %s cases=%u checks=%u complete\n",name,cases,checks);fflush(stdout);
}
int main(void) {
    group("mux",test_description_mux);
    group("demux",test_description_demux);
    group("continuation",test_description_continuation);
    group("invalid",test_description_invalid);
    puts("MINIMP4_DESCRIPTIONS_COMPLETE groups=4");return 0;
}
