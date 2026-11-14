#include <gtest/gtest.h>
#include <Unittests/unittests_decimater.cc>
#include <iostream>
int main(){try{if(legacy::entries().size()!=3)return 1;for(auto&e:legacy::entries()){int n=legacy::checks();e.run();if(legacy::checks()-n!=5)return 2;std::cout<<"LEGACY_PASS "<<e.name<<" checks="<<legacy::checks()-n<<"\n";}std::cout<<"LEGACY_COMPLETE cases=3 checks="<<legacy::checks()<<"\n";}catch(const std::exception&e){std::cerr<<e.what()<<"\n";return 3;}}
