/* Executes the unchanged upstream CUnit test functions. */
#include <CUnit/Basic.h>
#include <CUnit/CUnit.h>
#include <stdio.h>
void test_coordinates(void);
void test_nsearch(void);
void test_interpolate(void);
int main(void) {
  if (CU_initialize_registry()!=CUE_SUCCESS) return 1;
  CU_pSuite suite=CU_add_suite("unchanged upstream",NULL,NULL);
  if (!suite || !CU_add_test(suite,"coordinates",test_coordinates) ||
      !CU_add_test(suite,"nsearch",test_nsearch) ||
      !CU_add_test(suite,"interpolate",test_interpolate)) return 2;
  CU_basic_set_mode(CU_BRM_VERBOSE);
  CU_basic_run_tests();
  unsigned failures=CU_get_number_of_failures(), tests=CU_get_number_of_tests_run();
  unsigned assertions=CU_get_number_of_asserts();
  printf("UPSTREAM_CASES=%u FAILURES=%u ASSERTIONS=%u\n",tests,failures,assertions);
  CU_cleanup_registry();
  return failures || tests!=3 || assertions==0;
}
