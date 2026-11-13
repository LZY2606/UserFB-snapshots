#include "registry.hpp"
#include <set>
int main() {
  try {
    Registry r; register_task_tests(r);
    const std::vector<std::string> groups={"envelope","intervals","traversal","render"};
    std::set<std::string> names;
    for(const auto& e:r.entries) {
      bool found=false; for(const auto& g:groups) if(g==e.group) found=true;
      if(!found||e.name.empty()||!names.insert(e.group+":"+e.name).second)
        throw std::runtime_error("invalid or duplicate test registration");
    }
    for(const auto& group:groups) {
      unsigned count=0,checks=0;
      for(const auto& e:r.entries) if(e.group==group) {
        Case c; e.run(c);
        if(!c.checks) throw std::runtime_error("zero assertions: "+e.name);
        ++count; checks+=c.checks;
        std::cout<<"CASE "<<group<<" "<<e.name<<" checks="<<c.checks<<std::endl;
      }
      if(!count) throw std::runtime_error("empty group: "+group);
      std::cout<<"GROUP "<<group<<" cases="<<count<<" checks="<<checks<<std::endl;
    }
    std::cout<<"TARGET_COMPLETE"<<std::endl;
  } catch(const std::exception& e) { std::cerr<<"TARGET_FAILURE "<<e.what()<<std::endl; return 3; }
}
