#include <stdio.h>
#include <string.h>
#include "session_driver.h"
int main(void) {
  static const char *names[] = {"association", "head_reuse", "handoff"};
  static const uint8_t seeds[][8] = {{0}, {1,2,3,4,5,6,7,8}, {255,0,17,3,4,0,8,9}};
  unsigned passed[SESSION_GROUPS] = {0};
  size_t count = 0, i, j;
  const struct session_case *cases = session_cases(&count);
  if (!cases || count == 0 || count > 4096) { fputs("empty test registry\n", stderr); return 1; }
  for (i = 0; i < count; ++i) {
    if (!cases[i].name || !*cases[i].name || !cases[i].run ||
        (unsigned)cases[i].group >= SESSION_GROUPS) return 2;
    for (j = 0; j < i; ++j) if (!strcmp(cases[i].name, cases[j].name)) return 3;
    if (cases[i].run() != 1) { fprintf(stderr,"failed/skipped: %s\n",cases[i].name); return 4; }
    ++passed[cases[i].group];
    printf("PASS case=%s group=%s\n", cases[i].name, names[cases[i].group]);
  }
  for (i = 0; i < SESSION_GROUPS; ++i) {
    if (!passed[i]) { fprintf(stderr,"zero executed group=%s\n",names[i]); return 5; }
    printf("EXECUTED group=%s cases=%u\n", names[i], passed[i]);
  }
  for (i = 0; i < sizeof(seeds)/sizeof(seeds[0]); ++i)
    if (LLVMFuzzerTestOneInput(seeds[i],sizeof(seeds[i])) != 0) return 6;
  puts("HTTP_SESSION_CONTRACT_OK");
  return 0;
}
