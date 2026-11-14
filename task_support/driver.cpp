#include "check.hpp"
#include <iostream>
namespace contract { unsigned assertions=0; std::vector<Entry>& entries(){ static std::vector<Entry> e; return e; } }
int main() {
 const std::vector<std::string> required={"replay","chain","transport"};
 if(contract::entries().size()!=required.size()) return 21;
 for(const auto& name: required) {
  contract::Entry* found=nullptr;
  for(auto& entry:contract::entries()) if(entry.name==name) { if(found) return 22; found=&entry; }
  if(!found) return 23;
  contract::assertions=0;
  std::cout<<"BEGIN "<<name<<std::endl;
  try { found->fn(); } catch(const std::exception& e) { std::cerr<<e.what()<<std::endl; return 24; }
  if(contract::assertions==0) return 25;
  std::cout<<"DONE "<<name<<" "<<contract::assertions<<std::endl;
 }
 std::cout<<"ALL_DONE"<<std::endl;
}
