#include <cmath>
#include <cstring>
#include <cstdio>
#include <memory>
#include "MVerb.h"
int main(){
 auto a=std::make_unique<MVerb<double>>(); auto b=std::make_unique<MVerb<double>>();
 for(auto v:{a.get(),b.get()}){v->setSampleRate(48000);v->setParameter(MVerb<double>::PREDELAY,0);v->setParameter(MVerb<double>::DENSITY,.5);v->setParameter(MVerb<double>::DECAY,.8);v->setParameter(MVerb<double>::SIZE,.6);v->setParameter(MVerb<double>::EARLYMIX,1);}
 double delta=0,energy=0; double in[2]={0,0},oa[2],ob[2];
 double* ip[2]={in,in+1};double* ap[2]={oa,oa+1};double* bp[2]={ob,ob+1};
 for(int n=0;n<30000;n++){
  if(n==12000)b->setParameter(MVerb<double>::SIZE,.6);
  in[0]=n<10000?.2*std::sin(.071*n):0;in[1]=n<10000?.1*std::sin(.053*n):0;
  a->process(ip,ap,1);b->process(ip,bp,1);
  for(int c=0;c<2;c++){if(!std::isfinite(oa[c])||!std::isfinite(ob[c]))return 2;energy+=oa[c]*oa[c];if(n>=12000)delta+=std::abs(oa[c]-ob[c]);}
 }
 if(!(energy>1 && delta>.01))return 3;
 std::printf("MVERB_BASELINE frames=30000 finite=1 tail_change=%.9g energy=%.9g\n",delta,energy);
}
