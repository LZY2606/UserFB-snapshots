#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
extern int history_contract(const char *suite, unsigned seed, int case_id);
static int number(const char *s, unsigned *out) {
  char *end; unsigned long n;
  if (!s || *s < '0' || *s > '9') return 0;
  errno=0; n=strtoul(s,&end,10);
  if (errno || *end || n>UINT_MAX) return 0;
  *out=(unsigned)n; return 1;
}
int main(int argc,char **argv) {
  const char *groups[]={"model","branch","repeat","projection"};
  const char *selected=NULL; unsigned seed=1398, one=0;
  int single=0, suites=0; long total=0;
  for(int i=1;i<argc;i++) {
    if(i+1>=argc) return 64;
    if(!strcmp(argv[i],"--seed")) {if(!number(argv[++i],&seed))return 64;}
    else if(!strcmp(argv[i],"--case")) {if(!number(argv[++i],&one)||one>INT_MAX)return 64;single=1;}
    else if(!strcmp(argv[i],"--suite")) selected=argv[++i];
    else return 64;
  }
  for(int g=0;g<4;g++) {
    long assertions=0; int executed=0;
    if(selected&&strcmp(selected,groups[g]))continue;
    int count=single?1:32;
    for(int c=0;c<count;c++) {
      int id=single?(int)one:c;
      int n=history_contract(groups[g],seed,id);
      if(n<=0) {fprintf(stderr,"FAILED_OR_EMPTY suite=%s seed=%u case=%d assertions=%d\n",groups[g],seed,id,n);return 1;}
      assertions+=n; executed++;
    }
    if(executed!=count||assertions<=0)return 1;
    printf("PASS %s cases=%d assertions=%ld\n",groups[g],executed,assertions);
    suites++; total+=assertions;
  }
  if(!suites||total<=0)return 1;
  printf("ONIG_RESTORATION_CONTRACT_OK suites=%d assertions=%ld\n",suites,total);
  return 0;
}
