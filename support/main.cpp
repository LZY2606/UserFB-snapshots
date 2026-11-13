#include "check.hpp"
#include <iostream>
#include <set>
int main() {
  try {
    std::vector<UFCase> cases; register_tests(cases);
    std::set<std::string> names;
    size_t total = 0;
    for(const char* group : {"deflate", "rows", "framing"}) {
      size_t n = 0, before = unit_checks();
      for(const UFCase& c : cases) {
        if(!c.name || !c.group || !c.run) throw std::runtime_error("invalid case");
        if(std::string(c.group) != group) continue;
        if(!names.insert(c.name).second) throw std::runtime_error("duplicate case name");
        size_t start = unit_checks(); c.run();
        if(start == unit_checks()) throw std::runtime_error("zero content assertions");
        ++n; ++total; std::cout << "CASE " << c.name << " PASS\n";
      }
      if(!n) throw std::runtime_error(std::string("empty group: ") + group);
      std::cout << "GROUP " << group << " PASS cases=" << n << " checks=" << unit_checks() - before << "\n";
    }
    if(total != cases.size()) throw std::runtime_error("unexecuted cases");
    std::cout << "LODEPNG_TARGET_COMPLETE cases=" << total << " checks=" << unit_checks() << "\n";
  } catch(const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
  return 0;
}
