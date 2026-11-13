#include "cases.h"
#include <cstdio>
unsigned long task_assertions;
int main() {
 std::vector<OfflineCase> cases; register_cases(cases);
 if(cases.empty()) return 65;
 for(auto &c:cases) { auto before=task_assertions; if(!c.first||!c.second) return 66; try { c.second(); } catch(const std::exception &e) { std::fprintf(stderr,"FAIL %s: %s\n",c.first,e.what()); return 1; } if(task_assertions==before) return 67; std::printf("PASS %s\n",c.first); }
 std::printf("CASES=%zu ASSERTIONS=%lu\n",cases.size(),task_assertions); return 0;
}
