#include "CuTest.h"
#include <stdio.h>
void insert_knot_once(CuTest *);
void rmf_vectors_of_frames(CuTest *);
int main(void) {
 CuSuite *suite=CuSuiteNew();
 CuSuiteAdd(suite,CuTestNew("insert_knot_once",insert_knot_once));
 CuSuiteAdd(suite,CuTestNew("rmf_vectors_of_frames",rmf_vectors_of_frames));
 CuSuiteRun(suite);
 printf("OLD_TESTS cases=%d failed=%d\n",suite->count,suite->failCount);
 int result=suite->failCount?1:0;
 CuSuiteDelete(suite);
 return result;
}
