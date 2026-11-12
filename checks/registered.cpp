#include "test-harness.h"
#include "../test/phase_sync.cpp"
int main(){
 std::vector<std::string> required={"phase_pose","crossings_and_loops","reverse_and_pause","lifecycle_and_chain","invalid_and_legacy"};
 try{if(authorcheck::cases().size()!=required.size())throw std::runtime_error("wrong registered count");
 for(auto& name:required){auto it=std::find_if(authorcheck::cases().begin(),authorcheck::cases().end(),[&](auto& c){return c.name==name;});if(it==authorcheck::cases().end())throw std::runtime_error("missing "+name);authorcheck::assertions=0;it->fn();if(!authorcheck::assertions)throw std::runtime_error("zero assertions "+name);std::cout<<"PASS "<<name<<" assertions="<<authorcheck::assertions<<"\n";}
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}std::cout<<"PHASE_SYNC_OK cases=5\n";
}
