#include "suite.h"
#include <iostream>
#include <set>
int main(){try{Suite s;register_tests(s);std::set<std::string> required={"orthographic","perspective","clipped","states","compatibility"};std::set<std::string> seen;
 for(auto& c:s.cases){if(!required.count(c.name)||!seen.insert(c.name).second)throw std::runtime_error("unknown/duplicate group");auto n=s.assertions;c.fn(s);if(s.assertions-n<3)throw std::runtime_error("empty/skipped group: "+c.name);std::cout<<"PASS "<<c.name<<" assertions="<<(s.assertions-n)<<"\n";}
 if(seen!=required)throw std::runtime_error("missing groups");std::cout<<"TARGET_OK groups="<<seen.size()<<" assertions="<<s.assertions<<"\n";
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
