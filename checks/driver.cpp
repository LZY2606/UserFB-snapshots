#include "harness.h"
#include <iostream>
#include <set>
std::vector<Check>& checks() { static std::vector<Check> v; return v; }
int main(int argc,char**argv) {
 try {
  std::set<std::string> names; unsigned count=0;
  for(int i=1;i<argc;i++) {
   unsigned n=0;
   for(auto &c:checks()) if(std::string(c.group)==argv[i]) {
    if(!names.insert(std::string(c.group)+"/"+c.name).second) throw std::runtime_error("duplicate case");
    c.fn(); ++n; ++count; std::cout<<"PASS "<<c.group<<"/"<<c.name<<"\n";
   }
   if(!n) throw std::runtime_error(std::string("zero executed cases: ")+argv[i]);
  }
  if(!count) throw std::runtime_error("zero cases");
  if(after_all_checks) after_all_checks();
  std::cout<<"TARGET_CASES="<<count<<"\n";
 } catch(const std::exception&e) { std::cerr<<"FAIL "<<e.what()<<"\n"; return 1; }
}
