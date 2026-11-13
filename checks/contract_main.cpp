#include "contract.hpp"
#include <iostream>
#include <set>
#include <string>
#include <vector>
struct Case {std::string group,name;std::function<void()> run;};
static std::vector<Case> cases;
static unsigned assertions;
void contract_add(const char*g,const char*n,std::function<void()> f){
 if(!g||!n||!*n||!f)throw std::runtime_error("invalid registration");
 cases.push_back({g,n,f});
}
void contract_expect(bool ok,const char*why){++assertions;if(!ok)throw std::runtime_error(why);}
int main(){try{
 register_contract(); std::set<std::string> names;unsigned total=0,count=0;
 for(auto&c:cases){if(c.group!="pcm"&&c.group!="framing"&&c.group!="errors"&&c.group!="resources")throw std::runtime_error("unknown group");
 if(!names.insert(c.group+"/"+c.name).second)throw std::runtime_error("duplicate case");}
 for(auto group:{"pcm","framing","errors","resources"}){unsigned ran=0,checks=0;
  for(auto&c:cases)if(c.group==group){assertions=0;c.run();if(!assertions)throw std::runtime_error("no assertions");
   ++ran;checks+=assertions;std::cout<<"CASE_PASS "<<group<<" "<<c.name<<" "<<assertions<<std::endl;}
  if(!ran)throw std::runtime_error(std::string("missing group ")+group);
  total+=checks;count+=ran;std::cout<<"GROUP_DONE "<<group<<" "<<ran<<" "<<checks<<std::endl;
 }
 std::cout<<"IT_CONTRACT_COMPLETE "<<count<<" "<<total<<std::endl;return 0;
}catch(const std::exception&e){std::cerr<<"CONTRACT_FAIL "<<e.what()<<std::endl;return 1;}}
