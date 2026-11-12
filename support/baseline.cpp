#include "Test.h"
#include <vector>
#include <cmath>
#include <cassert>
#include <iostream>
int main(){
 const int w=24,h=16;std::vector<float> pixels(w*h*4,0);
 InitializeTest();UpdateTest(0,0,w,h,0);int rays=0;
 DrawTest(0,0,w,h,pixels.data(),rays,0);assert(rays>w*h*4);
 double sum=0;for(int i=0;i<w*h;++i) for(int c=0;c<3;++c){
  float f=pixels[i*4+c];assert(std::isfinite(f)&&f>=0);sum+=f;
 }
 assert(sum>1);ShutdownTest();
 std::cout<<"BASE cpu_trace PASS\n";
}
