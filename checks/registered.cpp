#include "test-harness.h"
#include "../test/inherit_pose.cpp"
int main(){
 std::vector<std::string> required={"static_modes","animated_ancestry","pivot_translation","skin_spaces","isolation_and_errors"};
 try{if(authorcheck::cases().size()!=required.size())throw std::runtime_error("wrong registered count");
 for(auto& name:required){auto it=std::find_if(authorcheck::cases().begin(),authorcheck::cases().end(),[&](auto& c){return c.name==name;});if(it==authorcheck::cases().end())throw std::runtime_error("missing "+name);authorcheck::assertions=0;it->fn();if(!authorcheck::assertions)throw std::runtime_error("zero assertions "+name);std::cout<<"PASS "<<name<<" assertions="<<authorcheck::assertions<<"\n";}
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}std::cout<<"FBX_INHERIT_POSE_OK cases=5\n";
}
