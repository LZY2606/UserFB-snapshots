#include "case_harness.hpp"
#include <cstring>
#include <set>
#include <string>
static unsigned assertions=0;
std::vector<TaskCase>& task_registry(){static std::vector<TaskCase> r;return r;}
void task_check(bool ok,const char* expression,const char* file,int line){++assertions;if(!ok){std::fprintf(stderr,"ASSERT %s:%d %s\n",file,line,expression);std::exit(2);}}
int main(){
 auto& cases=task_registry();std::set<std::string> names;
 for(auto& c:cases){if(!names.insert(c.name).second){std::fprintf(stderr,"DUPLICATE CASE\n");return 3;}
  if(std::strcmp(c.group,"relocation")&&std::strcmp(c.group,"hierarchy")&&std::strcmp(c.group,"compat")){std::fprintf(stderr,"UNKNOWN GROUP\n");return 4;}}
 unsigned total=0;
 for(auto g:{"relocation","hierarchy","compat"}){unsigned n=0,start=assertions;
  for(auto& c:cases)if(std::strcmp(c.group,g)==0){unsigned before=assertions;c.fn();if(assertions==before){std::fprintf(stderr,"EMPTY CASE %s\n",c.name);return 5;}++n;++total;std::printf("CASE %s %s checks=%u\n",g,c.name,assertions-before);}
  if(!n){std::fprintf(stderr,"EMPTY GROUP %s\n",g);return 6;}
  std::printf("GROUP %s cases=%u checks=%u\n",g,n,assertions-start);
 }
 std::printf("TARGET_COMPLETE cases=%u checks=%u\n",total,assertions);return 0;
}
