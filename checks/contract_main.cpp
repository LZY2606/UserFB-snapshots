#include "contract.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
struct Case {std::string group,name;std::function<void()> run;};
static std::vector<Case> cases;
static unsigned assertions;
void contract_add(const char *g,const char *n,std::function<void()> f) {
    if(!g||!n||!*n||!f) throw std::runtime_error("invalid registration");
    for(const auto &c:cases)if(c.group==g&&c.name==n)throw std::runtime_error("duplicate case");
    cases.push_back({g,n,f});
}
void contract_expect(bool ok,const char *reason) {++assertions;if(!ok)throw std::runtime_error(reason);}
int main() {try {
    register_contract();unsigned total=0,checks=0;
    for(auto group:{"envelope","graph","clock","lifetime"}) {
        unsigned count=0,groupChecks=0;
        for(const auto &c:cases) if(c.group==group) {
            assertions=0;c.run();if(!assertions)throw std::runtime_error("zero assertions");
            ++count;++total;groupChecks+=assertions;checks+=assertions;
            std::cout<<"PASS "<<group<<"/"<<c.name<<" "<<assertions<<std::endl;
        }
        if(!count)throw std::runtime_error(std::string("missing group ")+group);
        std::cout<<"GROUP_DONE "<<group<<" "<<count<<" "<<groupChecks<<std::endl;
    }
    if(total!=cases.size()) throw std::runtime_error("unknown group");
    std::cout<<"CONTRACT_COMPLETE "<<total<<" "<<checks<<std::endl;return 0;
} catch(const std::exception &e) {std::cerr<<e.what()<<std::endl;return 1;}}
