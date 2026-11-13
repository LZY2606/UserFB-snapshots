#pragma once
#include <cstdio>
#include <stdexcept>
#include <string>
#include <cmath>
struct Suite {
 int cases=0, assertions=0;
 void require(bool ok,const char* why){++assertions;if(!ok)throw std::runtime_error(why);}
 template<class F> void run(const char* name,F fn){int before=assertions;fn();if(assertions==before)throw std::runtime_error("case without assertions");++cases;std::printf("CASE %s OK\n",name);}
};
void target_cases(Suite&);
