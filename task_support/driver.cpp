#include "check.h"
#include <cstdio>
#include <cstring>
#include <exception>
static unsigned checks;
static bool failed;
bool task_check(bool value,const char* expr,const char* file,int line) {
    ++checks;
    if(!value) {failed=true;std::fprintf(stderr,"FAIL %s:%d %s\n",file,line,expr);}
    return value;
}
int main() {
    const char* groups[]={"identity","flow","provenance"};
    if(!task_case_count||task_case_count>10000)return 2;
    unsigned total=0;
    for(const char* group:groups) {
        unsigned cases=0,sum=0;
        for(std::size_t i=0;i<task_case_count;++i) {
            const auto& t=task_cases[i];
            if(!t.group||!t.name||!t.run)return 3;
            if(std::strcmp(t.group,group))continue;
            checks=0;
            int status;
            try {status=t.run();} catch(const std::exception& e) {
                std::fprintf(stderr,"uncaught %s\n",e.what());return 4;
            }catch(...){return 4;}
            if(status||failed||!checks)return 5;
            std::printf("CASE %s %s checks=%u\n",group,t.name,checks);
            ++cases;sum+=checks;++total;
        }
        if(!cases||!sum)return 6;
        std::printf("GROUP %s cases=%u checks=%u COMPLETE\n",group,cases,sum);
    }
    if(total!=task_case_count)return 7;
    std::printf("TARGET_COMPLETE cases=%u\n",total);
}
