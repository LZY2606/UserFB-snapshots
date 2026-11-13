#include "registry.hpp"
#include <set>
int main(int argc,char**argv){try{
 Registry r;register_task_tests(r);const char*groups[]={"parity","geometry","compatibility"};
 std::set<std::string>names;for(auto&e:r.entries){
  if(e.name.empty()||e.name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.:-")!=std::string::npos||!names.insert(e.name).second)throw std::runtime_error("invalid or duplicate name");
  if(e.group!="parity"&&e.group!="geometry"&&e.group!="compatibility")throw std::runtime_error("invalid group");
  if(!e.fn)throw std::runtime_error("null test");
 }
 bool list=argc==2&&std::string(argv[1])=="--list";
 for(auto g:groups){unsigned n=0,c=0;for(auto&e:r.entries)if(e.group==g){
  if(list){printf("REGISTER %s %s\n",g,e.name.c_str());++n;continue;}
  Case t;e.fn(t);if(!t.checks)throw std::runtime_error("zero assertions");c+=t.checks;++n;
  printf("CASE %s %s checks=%u\n",g,e.name.c_str(),t.checks);
 }if(!n)throw std::runtime_error("missing group");if(!list)printf("GROUP %s cases=%u checks=%u\n",g,n,c);}
 puts(list?"TASK_LIST_COMPLETE":"TASK_SUITE_COMPLETE");return 0;
 }catch(const std::exception&e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
