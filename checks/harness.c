#include "harness.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned checks;
void check_assert(int ok, const char *file, int line) {
  if (!ok) { fprintf(stderr,"assertion: %s:%d\n",file,line); exit(1); }
  ++checks;
}
int main(void) {
  const char *groups[] = {"priority","assertions","shortest","completeness","isolation"};
  unsigned i, before;
  for (i=0; i<sizeof(groups)/sizeof(groups[0]); ++i) {
    before=checks;
    check_case(groups[i]);
    if (checks==before) { fprintf(stderr,"empty group: %s\n",groups[i]); return 2; }
    printf("CASE %s checks=%u PASS\n",groups[i],checks-before);
  }
  printf("TARGET_OK groups=%u checks=%u\n",i,checks);
  return 0;
}
