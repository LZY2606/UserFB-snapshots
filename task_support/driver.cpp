#include "check.hpp"
#include <iostream>
unsigned task_assertions=0;
int main(){try{for(auto f:{contract_closure,contract_groups,contract_views}){auto n=task_assertions;if(f()<=0||task_assertions==n)throw std::runtime_error("empty group");}std::cout<<"GROUP_VIEW_OK assertions="<<task_assertions<<"\n";}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
