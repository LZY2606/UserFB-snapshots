#include "registry.hpp"
int main(int argc,char**argv) {
 if(argc!=2) return 2; Registry r; register_task_tests(r); unsigned n=0, assertions=0;
 try { for(auto &e:r.entries) if(e.group==argv[1]) { Case c; e.fn(c); if(!c.assertions) throw std::runtime_error("zero assertions"); ++n; assertions+=c.assertions; std::cout<<"PASS "<<e.group<<"/"<<e.name<<" assertions="<<c.assertions<<"\n"; }
 if(!n) throw std::runtime_error("zero matching tests");
 std::cout<<"GROUP "<<argv[1]<<" cases="<<n<<" assertions="<<assertions<<" COMPLETE\n"; }
 catch(const std::exception&e) { std::cerr<<"FAIL "<<argv[1]<<": "<<e.what()<<"\n"; return 1; } return 0;
}
