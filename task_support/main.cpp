#include "registry.hpp"
#include <cstdio>
int main(){
 const char *groups[]={"equivalence","sharing","automation","stream"};
 try{
  for(auto group:groups){unsigned cases=0,checks=0;
   for(auto &e:task::entries()) if(e.group==group){task::Case c;e.fn(c);if(!c.checks)throw std::runtime_error("zero assertions");checks+=c.checks;++cases;}
   if(!cases)throw std::runtime_error("missing group");
   std::printf("GROUP %s cases=%u checks=%u DONE\n",group,cases,checks);std::fflush(stdout);
  }
  std::puts("HARMONIZER_TARGET_COMPLETE groups=4");
 }catch(const std::exception&e){std::fprintf(stderr,"target: %s\n",e.what());return 1;}
}
