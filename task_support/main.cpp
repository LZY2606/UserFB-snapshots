#include "registry.hpp"
#include <exception>
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    try {
        Registry r; register_task_tests(r); size_t cases=0,checks=0;
        for(size_t i=0;i<r.entries.size();i++) {
            Registry::Entry& e=r.entries[i];
            if(e.group!=argv[1]) continue;
            Case c; int rc=e.run(c);
            if(rc!=0 || c.checks==0) return 3;
            ++cases;checks+=c.checks;
            std::printf("CASE_COMPLETE %s %s checks=%zu\n",argv[1],e.name.c_str(),c.checks);
        }
        if(cases<2 || checks<2) return 4;
        std::printf("GROUP_COMPLETE %s cases=%zu checks=%zu\n",argv[1],cases,checks);
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"case failure: %s\n",e.what()); return 1; }
}
