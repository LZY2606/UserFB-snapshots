#include <cstdio>
#include <cstring>
#include "upstream/tests.h"
Test* Test::s_firstTest = nullptr;
int main() {
 unsigned executed=0;
 for(auto t=Test::s_firstTest;t;t=t->next) {
  if(std::strcmp(t->getName(),"cabac")!=0)return 2;
  ++executed;
  if(!t->work(true))return 3;
 }
 if(executed!=1)return 4;
 std::printf("UPSTREAM_CABAC_PASS tests=%u bins=6000000\n",executed);
}
