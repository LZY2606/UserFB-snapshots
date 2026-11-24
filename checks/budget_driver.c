#include "budget_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
int redos_budget_case(const char *group, unsigned seed, int case_id);
static unsigned checks, failures;
void budget_check_impl(int ok, const char *expr, const char *file, int line) {
  if (ok) checks++;
  else {
    failures++;
    fprintf(stderr,"ASSERT_FAIL %s:%d %s\n",file,line,expr);
  }
}
static int number(const char *s, unsigned *out) {
  char *end; unsigned long n;
  if (!*s || *s=='-' || *s=='+') return 0;
  errno=0; n=strtoul(s,&end,10);
  if(errno || *end || n>UINT_MAX) return 0;
  *out=(unsigned)n; return 1;
}
int main(int argc, char **argv) {
  const char *groups[]={"corpus","search","calls","reuse"};
  unsigned seed=2677, one=0; int case_filter=0, group_filter=-1;
  for(int i=1;i<argc;i++) {
    if(i+1>=argc) return 2;
    const char *key=argv[i++], *value=argv[i];
    if(!strcmp(key,"--seed")) {if(!number(value,&seed)) return 2;}
    else if(!strcmp(key,"--case")) {if(!number(value,&one) || one>=24) return 2; case_filter=1;}
    else if(!strcmp(key,"--group")) {
      for(int j=0;j<4;j++) if(!strcmp(value,groups[j])) group_filter=j;
      if(group_filter<0) return 2;
    } else return 2;
  }
  unsigned total=0, ng=0;
  for(int g=0;g<4;g++) {
    if(group_filter>=0 && group_filter!=g) continue;
    unsigned sum=0, cases=0;
    for(int c=0;c<24;c++) {
      if(case_filter && (unsigned)c!=one) continue;
      checks=failures=0;
      printf("BEGIN %s seed=%u case=%d\n",groups[g],seed,c); fflush(stdout);
      int rc=redos_budget_case(groups[g],seed,c);
      if(rc || failures || !checks) {
        fprintf(stderr,"CASE_FAIL group=%s seed=%u case=%d rc=%d checks=%u failures=%u\n",groups[g],seed,c,rc,checks,failures);
        return 1;
      }
      printf("CASE_OK %s seed=%u case=%d checks=%u\n",groups[g],seed,c,checks);
      sum+=checks; cases++;
    }
    if(!sum || !cases) return 1;
    printf("GROUP_OK %s cases=%u checks=%u\n",groups[g],cases,sum); total+=sum; ng++;
  }
  printf("ONIG_BUDGET_TARGET_OK groups=%u checks=%u\n",ng,total);
  return ng && total ? 0:1;
}
