#include "contract.h"
#include "gc/gc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct test { const char *group, *name; void (*fn)(void); };
static struct test cases[256];
static unsigned used, assertions;
static void fail(const char *why) { fprintf(stderr,"CONTRACT_FAIL %s\n",why); exit(42); }
void contract_add(const char *group,const char *name,void (*fn)(void)) {
  unsigned i;
  if(!group || !name || !*name || !fn || used==256) fail("invalid registration");
  if(strcmp(group,"identity") && strcmp(group,"realloc") && strcmp(group,"collection") && strcmp(group,"threads")) fail("unknown group");
  for(i=0;i<used;i++) if(!strcmp(name,cases[i].name)) fail("duplicate name");
  cases[used].group=group; cases[used].name=name; cases[used++].fn=fn;
}
void contract_expect(int condition,const char *message) {
  assertions++;
  if(!condition) fail(message?message:"expectation");
}
int main(void) {
  const char *groups[]={"identity","realloc","collection","threads"};
  unsigned g,i,ran=0,total=0;
#ifdef GC_THREADS
  const unsigned ng=4;
#else
  const unsigned ng=3;
#endif
  GC_INIT(); register_contract();
  for(g=0;g<ng;g++) {
    unsigned n=0,checks=0;
    for(i=0;i<used;i++) if(!strcmp(groups[g],cases[i].group)) {
      assertions=0; cases[i].fn();
      if(!assertions) fail("case made zero checks");
      n++; ran++; checks+=assertions;
      printf("CASE_PASS %s %s checks=%u\n",groups[g],cases[i].name,assertions);
    }
    if(!n) fail("missing required group");
    total+=checks;
    printf("GROUP_PASS %s cases=%u checks=%u\n",groups[g],n,checks);
  }
  printf("CONTRACT_DONE cases=%u groups=%u checks=%u\n",ran,ng,total);
  return 0;
}
