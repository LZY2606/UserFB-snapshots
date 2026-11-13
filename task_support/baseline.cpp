#include "rlottie.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>
static std::string solid(const char*name,int bm,const char*color){return std::string("{\"ty\":1,\"ind\":")+(name[0]=='t'?"1":"2")+",\"nm\":\""+name+"\",\"sw\":8,\"sh\":8,\"sc\":\""+color+"\",\"bm\":"+std::to_string(bm)+",\"ip\":0,\"op\":3,\"st\":0,\"ks\":{\"o\":{\"a\":0,\"k\":100},\"r\":{\"a\":0,\"k\":0},\"p\":{\"a\":0,\"k\":[0,0]},\"a\":{\"a\":0,\"k\":[0,0]},\"s\":{\"a\":0,\"k\":[100,100]}}}";}
static unsigned render(int bm){
 std::string json="{\"v\":\"5.5.0\",\"fr\":30,\"ip\":0,\"op\":3,\"w\":8,\"h\":8,\"layers\":["+solid("top",bm,"#808080")+","+solid("bottom",0,"#808080")+"]}";
 auto a=rlottie::Animation::loadFromData(json,"baseline-"+std::to_string(bm),"",false);assert(a);
 std::vector<uint32_t> p(64,0);rlottie::Surface s(p.data(),8,8,8*4);a->renderSync(0,s);assert((p[27]>>24)==255);return p[27]&255;
}
int main(int argc,char**){unsigned normal=render(0),screen=render(2);assert(normal==128);printf("RLOTTIE_NORMAL_BASELINE_OK gray=%u\n",normal);printf("RLOTTIE_SCREEN_CURRENT actual=%u target=192\n",screen);if(screen==128)puts("RLOTTIE_SCREEN_MISSING_REPRODUCED");if(argc>1&&screen!=192)return 1;return 0;}
