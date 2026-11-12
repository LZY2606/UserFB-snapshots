#include "driver.h"
#include <stdio.h>
int main(void) {
 if (case_count != 6) return 40;
 for (size_t i=0; i<case_count; ++i) {
  if (!cases[i].run) return 41;
  int rc=cases[i].run();
  if (rc != 1) { fprintf(stderr,"CASE_FAIL %s return=%d\n",cases[i].name,rc); return 42; }
  printf("CASE_PASS %s\n",cases[i].name);
 }
 printf("EXECUTED %zu\n",case_count);
 return 0;
}
