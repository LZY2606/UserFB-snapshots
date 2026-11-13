#include "test_api.h"
int main(){Suite s;try {target_cases(s);if(s.cases<4||s.assertions<8)throw std::runtime_error("need four cases and eight assertions");std::printf("TARGET_OK cases=%d assertions=%d\n",s.cases,s.assertions);return 0;}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
