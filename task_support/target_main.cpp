#include "registry.hpp"
#include <set>
#include <iostream>
int main(){try{Registry r;register_task_tests(r);std::set<std::string> groups,names;
 const std::set<std::string> required={"sampling","transactions","convergence","integration"};
 if(r.cases.empty())throw std::runtime_error("zero cases");
 for(auto &c:r.cases){if(!required.count(c.group)||c.name.empty()||!names.insert(c.group+"/"+c.name).second)throw std::runtime_error("invalid registration");unsigned before=r.assertions;c.fn();if(r.assertions==before)throw std::runtime_error("case has no checks");groups.insert(c.group);std::cout<<"PASS "<<c.group<<"/"<<c.name<<"\n";}
 for(auto &g:required)if(!groups.count(g))throw std::runtime_error("missing group: "+g);
 std::cout<<"SAMPLED_NORMAL_TESTS_PASS cases="<<r.cases.size()<<" assertions="<<r.assertions<<"\n";
 }catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;} }
