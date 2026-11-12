#ifndef GSUBCHECK_SUITE_H
#define GSUBCHECK_SUITE_H
#include <stdio.h>
extern int gc_checks;
extern int gc_failed;
extern int gc_groups;
extern int gc_suite_finished;
#define GC_CHECK(x) do { ++gc_checks; if(!(x)) { ++gc_failed; fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); } } while(0)
#define GC_GROUP_DONE() (++gc_groups)
#define GC_SUITE_DONE() (++gc_suite_finished)
int unit_run_group(const char *name);
#endif
