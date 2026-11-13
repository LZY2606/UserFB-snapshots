#include "PerlinNoise.hpp"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#define CHECK(x) do { if(!(x)) throw std::runtime_error(#x); ++count; }while(0)
int main(){
 int count=0; double checksum=0;
 for(unsigned seed: {0u,12345u,98765u}) {
  siv::PerlinNoise n(seed), copy; copy.deserialize(n.serialize());
  for(int k=0;k<13;++k){
   double x=-3.125+k*0.3125,y=1.0625-k*0.1875;
   double v=n.noise2D(x,y); CHECK(std::isfinite(v)); CHECK(v==copy.noise2D(x,y));
   CHECK(v==n.noise3D(x,y,SIVPERLIN_DEFAULT_Z));
   CHECK(std::abs(v-n.noise2D(x+256,y-256))<1e-12);
   double sum=0,amp=1,den=0,xx=x,yy=y;
   for(int j=0;j<5;++j){sum+=amp*n.noise2D(xx,yy);den+=amp;xx*=2;yy*=2;amp*=.625;}
   CHECK(std::abs(sum-n.octave2D(x,y,5,.625))<1e-13);
   CHECK(std::abs(sum/den-n.normalizedOctave2D(x,y,5,.625))<1e-13);
   checksum+=v;
  }
 }
 std::ofstream img("build/point-noise.pgm");img<<"P2\n32 32\n255\n";
 siv::PerlinNoise n(42u);for(int y=0;y<32;++y)for(int x=0;x<32;++x)img<<int(n.noise2D_01(x*.09375,y*.09375)*255)<<' ';
 CHECK(bool(img));
 std::cout<<std::setprecision(17)<<"BASELINE point-slice periodic octave state OK checks="<<count<<" checksum="<<checksum<<"\n";
}
