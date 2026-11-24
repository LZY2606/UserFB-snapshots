#include "contract_api.h"
#include <cstdio>
#include <cstring>
#include <stdexcept>
static struct {const char* name;void (*fn)();} cases[5];
static size_t count=0, assertions=0;
static bool failed=false;
void location_add_case(const char* name,void (*fn)()) {
 if(count==5 || !name || !fn){failed=true;throw std::runtime_error("invalid registration");}
 for(size_t i=0;i<count;i++)if(!std::strcmp(cases[i].name,name)){failed=true;throw std::runtime_error("duplicate case");}
 cases[count++]={name,fn};
}
void location_expect(int condition,const char* message) {
 ++assertions;
 if(!condition){failed=true;throw std::runtime_error(message?message:"assertion failed");}
}
int main(int argc,char** argv) {
 if(argc!=2)return 2;
 const char* expected[]={"legacy","indexed","defaults","cu_context","samples"};
 try {
  register_location_cases();
  if(count!=5 || failed)throw std::runtime_error("expected five groups");
  for(const char* name:expected){
   size_t i=0;while(i<count && std::strcmp(cases[i].name,name))++i;
   if(i==count)throw std::runtime_error("missing group");
   assertions=0;cases[i].fn();
   if(!assertions || failed)throw std::runtime_error("empty group or recorded failure");
   std::printf("PASS location/%s assertions=%zu\n",name,assertions);
  }
 } catch(const std::exception& e){std::fprintf(stderr,"contract failed: %s\n",e.what());return 1;}
 if(failed)return 1;
 FILE* receipt=std::fopen(argv[1],"wb");if(!receipt)return 3;
 if(std::fputs("LOCATION_GROUPS_COMPLETED=5\n",receipt)<0)return 3;
 if(std::fclose(receipt))return 3;
 return 0;
}
