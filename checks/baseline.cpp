#include "AmbisonicEncoder.h"
#include "AmbisonicEncoderDist.h"
#include "BFormat.h"
#include <cstdio>
#include <cmath>
int main(){spaudio::AmbisonicEncoder e;spaudio::BFormat b;if(!e.Configure(1,true,8000,0)||!b.Configure(1,true,64))return 1;spaudio::PolarPosition<float> p;p.azimuth=0;p.elevation=0;p.distance=1;e.SetPosition(p);float input[64]={1};e.Process(input,64,&b);auto w=b.GetChannelPointer(0),y=b.GetChannelPointer(1),z=b.GetChannelPointer(2),x=b.GetChannelPointer(3);if(fabs(w[0]-1)>1e-6||fabs(x[0]-1)>1e-6||fabs(y[0])>1e-6||fabs(z[0])>1e-6)return 2;spaudio::AmbisonicEncoderDist d;if(!d.Configure(1,true,8000))return 3;d.SetPosition(p);d.Refresh();d.Process(input,64,&b);double energy=0;for(int j=0;j<4;j++)for(int i=0;i<64;i++){float v=b.GetChannelPointer(j)[i];if(!std::isfinite(v))return 4;energy+=v*v;}if(energy<=0)return 5;printf("SPATIAL_BASELINE_OK channels=4 static_energy=%.9g\n",energy);}
