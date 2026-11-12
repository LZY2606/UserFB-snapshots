#include "case_harness.hpp"
#include "target_cases.cpp"
int main(){
const char* required[] = {"division","clock","lifecycle","compatibility"};
unsigned seen=0;
for(auto &c:fb_cases()){unsigned bit=0;for(unsigned i=0;i<sizeof(required)/sizeof(*required);++i)if(!std::strcmp(c.group,required[i]))bit=1u<<i; if(!bit)return 2; fb_checks=0; c.fn();if(!fb_checks){std::fprintf(stderr,"NO_ASSERTIONS %s\n",c.name);return 3;}seen|=bit;std::printf("PASS %s/%s checks=%u\n",c.group,c.name,fb_checks);}
if(seen != 15){std::fprintf(stderr,"MISSING_GROUPS cases=%zu mask=%u\n",fb_cases().size(),seen);return 4;}
std::printf("TARGET_OK cases=%zu\n",fb_cases().size());}
