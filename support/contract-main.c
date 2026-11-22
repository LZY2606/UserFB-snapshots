#include <stdio.h>
#include <string.h>
/* Functions are implemented in the new tests/forkserver_contract.c. */
int test_stdin_replay(const char *, const char *, const char *);
int test_file_replay(const char *, const char *, const char *);
int test_persistent_recovery(const char *, const char *, const char *);
int test_feedback_modes(const char *, const char *, const char *);
int test_protocol_errors(const char *, const char *, const char *);
int main(int argc, char **argv) {
  struct test { const char *name; int (*run)(const char *, const char *, const char *); };
  const struct test cases[] = {
    {"stdin_replay", test_stdin_replay}, {"file_replay", test_file_replay},
    {"persistent_recovery", test_persistent_recovery},
    {"feedback_modes", test_feedback_modes}, {"protocol_errors", test_protocol_errors}
  };
  if (argc != 4) return 2;
  unsigned ran = 0;
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    int rc = cases[i].run(argv[1], argv[2], argv[3]);
    if (rc) { fprintf(stderr, "FAIL %s rc=%d\n", cases[i].name, rc); return rc; }
    printf("PASS %s\n", cases[i].name);
    ran++;
  }
  if (ran != 5) return 3;
  puts("CONTRACT_CASES=5");
  return 0;
}
