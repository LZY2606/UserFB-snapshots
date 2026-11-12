#include "RKSimulationModel.h"
#include <vector>
#include <cmath>
#include <cassert>
#include <cstdio>
int main(){
 RKSimulationMoog a(48000),b(48000);a.SetCutoff(2100);b.SetCutoff(2100);a.SetResonance(2.7f);b.SetResonance(2.7f);
 std::vector<float>x(4096),y;for(int i=0;i<4096;i++)x[i]=.8f*std::sin(i*.23f)+(i==0?2:0);y=x;
 a.Process(x.data(),x.size());for(int p=0;p<4096;){int n=std::min(17,4096-p);b.Process(y.data()+p,n);p+=n;}
 double energy=0;for(int i=0;i<4096;i++){assert(x[i]==y[i]);assert(std::isfinite(x[i]));energy+=x[i]*x[i];}
 assert(energy>1);std::printf("MOOG_RK_OK frames=4096 energy=%.9f last=%.9f\n",energy,x.back());
}
