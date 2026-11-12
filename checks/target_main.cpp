#include "suite.h"
#include <iostream>
#include <set>
int main(int argc,char** argv){try{bool geometry_only=(argc==2&&std::string(argv[1])=="--geometry-only");if(argc!=1&&!geometry_only)throw std::runtime_error("invalid mode");Suite s;register_tests(s);unsigned executed=0;std::set<std::string> required={"exact","seams","intervals","layouts","mutants"};std::set<std::string> seen;
 for(auto& c:s.cases){if(!required.count(c.name)||!seen.insert(c.name).second)throw std::runtime_error("unknown/duplicate group");if(geometry_only&&c.name=="mutants")continue;auto n=s.assertions;c.fn(s);if(s.assertions-n<3)throw std::runtime_error("empty/skipped group: "+c.name);std::cout<<"PASS "<<c.name<<" assertions="<<(s.assertions-n)<<"\n";}
 if(seen!=required)throw std::runtime_error("missing groups");for(const auto& c:s.cases)if(!geometry_only||c.name!="mutants")++executed;std::cout<<"TARGET_OK groups="<<executed<<" assertions="<<s.assertions<<"\n";
 }catch(const ContentFailure& e){std::cerr<<"CONTENT_FAILURE "<<e.what()<<"\n";return 3;}
 catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
