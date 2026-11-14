#define main smallpt_original_main
#include "../smallpt.cpp"
#undef main
#include <cassert>
#include <cmath>
#include <iomanip>
#include <iostream>
int main() {
 Sphere unit(1,Vec(),Vec(),Vec(.7,.6,.5),DIFF);
 assert(std::fabs(unit.intersect(Ray(Vec(0,0,3),Vec(0,0,-1)))-2)<1e-12);
 assert(std::fabs(unit.intersect(Ray(Vec(),Vec(1,0,0)))-1)<1e-12);
 assert(unit.intersect(Ray(Vec(0,0,3),Vec(0,1,0)))==0);
 unsigned short Xi[3]={17,23,41};
 Vec light=radiance(Ray(Vec(50,80,81.6),Vec(0,1,0)),0,Xi);
 assert(std::fabs(light.x-12)<1e-10 && std::fabs(light.y-12)<1e-10 && std::fabs(light.z-12)<1e-10);
 Ray cam(Vec(50,52,295.6),Vec(0,-.042612,-1).norm());
 Vec cx(8*.5135/6),cy=(cx%cam.d).norm()*.5135,sum;
 int seen[3]={0,0,0},hits=0;
 for(int y=0;y<6;++y)for(int x=0;x<8;++x){
  Vec d=(cx*((x+.5)/8-.5)+cy*((y+.5)/6-.5)+cam.d).norm();
  Ray r(cam.o+d*140,d); double t;int id;
  if(intersect(r,t,id)){++hits;++seen[spheres[id].refl];}
  for(int k=0;k<32;++k){Vec c=radiance(r,0,Xi);
   assert(std::isfinite(c.x)&&std::isfinite(c.y)&&std::isfinite(c.z));
   assert(c.x>=0&&c.y>=0&&c.z>=0);sum=sum+c*(1./32);
  }
 }
 assert(hits==48 && seen[DIFF]>0 && seen[SPEC]>0 && seen[REFR]>0);
 assert(sum.x>0 && sum.y>0 && sum.z>0);
 std::cout<<std::setprecision(15)<<"BASELINE_COMPLETE light="<<light.x<<","<<light.y<<","<<light.z
  <<" sum="<<sum.x<<","<<sum.y<<","<<sum.z<<" materials="<<seen[0]<<","<<seen[1]<<","<<seen[2]<<"\n";
}
