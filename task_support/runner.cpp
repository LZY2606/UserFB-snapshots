#include "registry.hpp"
#include <iostream>
#include <set>
int main() {
    const std::vector<std::string> groups={"profile","offset","joins","render"};
    std::size_t all_cases=0,all_checks=0;
    try {
      std::set<std::string> names;
      for(const auto& e:task::entries()) {
        bool known=false;for(const auto& g:groups)known|=(g==e.group);
        if(!known||!names.insert(std::string(e.group)+"/"+e.name).second)
          throw std::runtime_error("unknown group or duplicate case");
      }
      for(const auto& group:groups) {
        std::size_t cases=0,checks=0;
        for(const auto& e:task::entries()) if(group==e.group) {
          task::Case c;
          try {e.fn(c);} catch(const std::exception& x) {
            std::cerr<<"CASE_FAIL "<<group<<"/"<<e.name<<" "<<x.what()<<"\n";return 1;
          }
          if(c.failed)throw std::runtime_error("case caught a failed check: "+group+"/"+e.name);
          if(!c.checks)throw std::runtime_error("case executed zero checks: "+group+"/"+e.name);
          ++cases;checks+=c.checks;
          std::cout<<"CASE_OK "<<group<<"/"<<e.name<<" checks="<<c.checks<<"\n";
        }
        if(!cases)throw std::runtime_error("missing group: "+group);
        all_cases+=cases;all_checks+=checks;
        std::cout<<"GROUP_OK "<<group<<" cases="<<cases<<" checks="<<checks<<"\n";
      }
      std::cout<<"PRESSURE_COMPLETE groups=4 cases="<<all_cases<<" checks="<<all_checks<<"\n";
    } catch(const std::exception& e) {std::cerr<<"TARGET_FAIL "<<e.what()<<"\n";return 1;}
}
