#include "suite.h"
#include <iostream>
#include <set>
int main(){try{Suite s;register_tests(s);std::set<std::string> expected={"unicode","compressed","pruning","storage"};if(s.cases.size()!=expected.size())throw std::runtime_error("wrong group count");
for(auto &v:s.cases){if(!expected.erase(v.first))throw std::runtime_error("unknown or duplicate group");Check c;v.second(c);if(c.count()<3)throw std::runtime_error("group needs 3 checks");std::cout<<"GROUP "<<v.first<<" PASS checks="<<c.count()<<"\n";}
if(!expected.empty())return 1;std::cout<<"MARISA_TARGET_COMPLETE groups=4\n";return 0;}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 1;}}
