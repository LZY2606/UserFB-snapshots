#include "cases.hpp"
#include <set>
int main() {
 const std::set<std::string> required={"typed_tuple_model","scope_and_namespace","missing_vs_unique","forward_keyref","semantic_mutants"};
 std::set<std::string> seen;
 try {
  for (auto c: tcase::cases()) {
   if (!seen.insert(c.name).second) throw std::runtime_error("duplicate case");
   int before=tcase::assertions(); c.fn();
   if (before==tcase::assertions()) throw std::runtime_error("empty case");
   std::cout<<"PASS "<<c.name<<"\n";
  }
  for(auto n:required) if (!seen.count(n)) throw std::runtime_error("missing case "+n);
  std::cout<<"TARGET_OK cases="<<seen.size()<<" assertions="<<tcase::assertions()<<"\n";
 } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<"\n"; return 1; }
}
