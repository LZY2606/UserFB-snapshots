#include "cases.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static const char *required[] = {
  "DictionaryOrder", "NestedSlices", "IndexOverflow", "Compatibility"
};
static int seen[4], total, failed, active, group_failed;
static uint64_t group_checks, all_checks;
int task_check(int condition, const char *expression, const char *file, int line) {
  if (!active || group_checks == UINT64_MAX || all_checks == UINT64_MAX) {
    failed = 1;
    return 0;
  }
  group_checks++;
  all_checks++;
  if (!condition) {
    group_failed = 1;
    fprintf(stderr, "CHECK FAILED %s:%d: %s\n", file, line, expression);
  }
  return condition;
}
void task_case(const char *name, task_fn fn) {
  size_t i;
  if (active || !name || !fn) { failed = 1; return; }
  for (i = 0; i < 4; i++) if (!strcmp(name, required[i])) break;
  if (i == 4 || seen[i]) { failed = 1; return; }
  seen[i] = 1;
  total++;
  group_checks = 0;
  group_failed = 0;
  active = 1;
  int rc = fn();
  active = 0;
  int bad = rc != 0 || group_failed || group_checks == 0;
  printf("%s %s checks=%" PRIu64 "\n", bad ? "FAIL" : "PASS", name, group_checks);
  if (bad) failed = 1;
}
int main(void) {
  task_register_tests();
  for (size_t i = 0; i < 4; i++) if (!seen[i]) failed = 1;
  printf("EXECUTED %d CHECKS %" PRIu64 "\n", total, all_checks);
  return failed || total != 4;
}
