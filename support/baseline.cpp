#include <cstring>
#include <cstdio>
#include <vector>
#include "Cpp/Source/Test.cpp"
int main(){InitializeTest();UpdateTest(0,0,24,16,0);std::vector<float> pixels(24*16*4);int rays=0;DrawTest(0,0,24,16,pixels.data(),rays,0);double sum=0;for(int i=0;i<24*16;i++)for(int c=0;c<3;c++){float v=pixels[i*4+c];if(!std::isfinite(v)||v<0)return 1;sum+=v;}ShutdownTest();if(rays<384||sum<=0)return 2;printf("CPU_RENDER pixels=384 rays=%d sum=%.9f\n",rays,sum);return 0;}
