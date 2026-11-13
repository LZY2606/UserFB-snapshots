#include "registry.hpp"
#include <set>
int main(){try{Registry r;register_task_tests(r);std::set<std::string> groups;
 const std::set<std::string> required={"huffman","blocks","palette","crossframe"};
 if(r.cases.size()<12)throw std::runtime_error("need at least 12 cases");
 for(auto &c:r.cases){int before=r.assertions;c.fn();if(r.assertions==before)throw std::runtime_error("case has no checks");groups.insert(c.group);std::cout<<"PASS "<<c.group<<"/"<<c.name<<"\n";}
 for(auto &g:required)if(!groups.count(g))throw std::runtime_error("missing group: "+g);
 std::cout<<"TASK_TESTS_PASS cases="<<r.cases.size()<<" assertions="<<r.assertions<<"\n";
 }catch(const AssertionFailure&e){std::cerr<<"TASK_ASSERT "<<e.what()<<"\n";return 42;}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;} }
