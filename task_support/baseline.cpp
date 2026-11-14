#include "Test.h"
#include <vector>
#include <cmath>
#include <iostream>
#include <iomanip>
int main() {
    const int w=24,h=16;
    std::vector<float> first(w*h*4,0),second(w*h*4,0);
    InitializeTest();
    UpdateTest(0,0,w,h,0);
    int rays=0,again=0;
    DrawTest(0,0,w,h,first.data(),rays,0);
    DrawTest(0,0,w,h,second.data(),again,0);
    ShutdownTest();
    if(rays<=w*h*4 || rays!=again || first!=second) {
        std::cerr<<"FAIL legacy deterministic production draw\n"; return 2;
    }
    double sum=0;
    for(int i=0;i<w*h;++i) for(int c=0;c<3;++c) {
        float value=first[i*4+c];
        if(!std::isfinite(value)||value<0) return 3;
        sum+=value;
    }
    if(sum<=1) return 4;
    int count,objectSize,materialSize,cameraSize;
    GetObjectCount(count,objectSize,materialSize,cameraSize);
    if(count!=46 || materialSize!=36) return 5;
    std::cout<<std::setprecision(12)<<"CPU_RENDER spheres="<<count<<" rays="<<rays<<" rgb_sum="<<sum<<" material_bytes="<<materialSize<<"\n";
    std::cout<<"BASELINE_OK real_cpu_trace_repeat_equal\n";
}
