#include "suite.h"
#include <cstdlib>
#include <iostream>
namespace nes_task {
static unsigned checks;
void check(bool ok,const char* expr,const char* file,int line) {
    ++checks;
    if(!ok) { std::cerr<<file<<':'<<line<<" failed "<<expr<<'\n'; std::exit(1); }
}
static void group(const char* name,void (*f)()) {
    checks=0;
    f();
    if(!checks) { std::cerr<<"no content checks in "<<name<<'\n'; std::exit(2); }
    std::cout<<"TARGET_GROUP "<<name<<" checks="<<checks<<" PASS\n";
}
}
int main() {
    nes_task::group("dac",target_dac);
    nes_task::group("timeline",target_timeline);
    nes_task::group("nsf",target_nsf);
    std::cout<<"TARGET_NES_NONLINEAR_COMPLETE groups=3\n";
}
