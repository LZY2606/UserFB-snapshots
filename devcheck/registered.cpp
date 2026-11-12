#include "test-harness.h"
#include "../test/sparse_sampling.cpp"
int main(){
 std::vector<std::string> required={"sparse_values","seek_and_masks","packed_timepoints","work_and_allocations","archive_and_errors"};
 try{if(checks::cases().size()!=required.size())throw std::runtime_error("wrong registered count");
 for(auto& name:required){auto it=std::find_if(checks::cases().begin(),checks::cases().end(),[&](auto& c){return c.name==name;});if(it==checks::cases().end())throw std::runtime_error("missing "+name);checks::assertions=0;it->fn();if(!checks::assertions)throw std::runtime_error("zero assertions "+name);std::cout<<"PASS "<<name<<" assertions="<<checks::assertions<<"\n";}
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}std::cout<<"OZZ_SPARSE_OK cases=5\n";
}
