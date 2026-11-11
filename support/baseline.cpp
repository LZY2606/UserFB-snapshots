#include "vgm_fixture.h"
int main(int argc, char** argv) {
    using namespace fixture;
    bool warning=false;
    std::vector<short> a=render(explicit_dac(),false), b=render(explicit_dac(),true);
    if(a!=b || nonzero(a)==0) return 2;
    std::printf("VGM_EXPLICIT_DAC_OK nonzero=%u chunk_equal=1\n",nonzero(a));
    if(argc==2 && std::strcmp(argv[1],"--normal")==0) return 0;
    std::vector<short> c=render(stream_dac(),false,&warning);
    unsigned differences=0;
    for(unsigned i=0;i<a.size();++i) differences+=a[i]!=c[i];
    if(argc==2 && std::strcmp(argv[1],"--goal")==0) {
        if(differences || warning) {
            std::fprintf(stderr,"VGM_STREAM_GOAL_FAIL differing_samples=%u warning=%d\n",differences,int(warning));return 4;
        }
        std::puts("VGM_STREAM_GOAL_SMOKE_OK");return 0;
    }
    if(argc!=1) return 5;
    if(!differences || !warning) return 3;
    std::printf("VGM_STREAM_UNSUPPORTED_WITNESS differing_samples=%u warning=1\n",differences);
    return 0;
}
