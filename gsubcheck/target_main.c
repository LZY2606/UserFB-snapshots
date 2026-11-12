#include <stdlib.h>
#include <string.h>
#include "suite.h"
int gc_checks=0,gc_failed=0,gc_groups=0,gc_suite_finished=0;
static int current_group=0,normal_return=0;
static const char *names[]={"lookup_plan","context_edits","filter_and_render","errors_and_legacy"};
/* exit(0) inside a test must not bypass the remaining groups. */
static void completion_guard(void) {
  if(!normal_return || current_group!=4 || gc_groups!=4 || gc_suite_finished!=1) {
    fputs("INCOMPLETE_TARGET\n",stderr);fflush(NULL);_Exit(91);
  }
}
int main(void) {
  int i;atexit(completion_guard);
  for(i=0;i<4;i++) {
    int before=gc_checks,failed_before=gc_failed,groups_before=gc_groups;
    int n=unit_run_group(names[i]);
    if(n<=0 || n!=gc_checks-before || gc_failed!=failed_before || gc_groups!=groups_before+1) {
      fprintf(stderr,"BAD_GROUP %s return=%d checks=%d failed=%d groups=%d\n",names[i],n,gc_checks-before,gc_failed-failed_before,gc_groups-groups_before);return 1;
    }
    current_group++;printf("GROUP_OK %s checks=%d\n",names[i],n);
  }
  if(gc_failed || gc_checks<=0 || gc_suite_finished!=1)return 1;
  normal_return=1;printf("TARGET_OK groups=4 checks=%d\n",gc_checks);return 0;
}
