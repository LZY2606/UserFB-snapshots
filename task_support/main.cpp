#include "registry.hpp"
int main(){try{Registry r;register_task_tests(r);const char* groups[]={"modes","isolation","matte","reuse"};
 for(auto g:groups){unsigned n=0,c=0;for(auto&e:r.entries)if(e.group==g){
  for(auto&f:r.entries)if(&e!=&f&&e.name==f.name)throw std::runtime_error("duplicate test name");
  Case t;e.fn(t);if(!t.checks)throw std::runtime_error("zero assertions");c+=t.checks;++n;
 }if(!n)throw std::runtime_error("missing group");printf("GROUP %s cases=%u checks=%u\n",g,n,c);}
 puts("TASK_SUITE_COMPLETE");return 0;}catch(const std::exception&e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
