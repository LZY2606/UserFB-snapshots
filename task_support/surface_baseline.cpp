#include "task_support/surface_access.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <cstdio>
#include <string>
#include <vector>
int main(){
 using namespace smallpt;
 RNG directRng(7);
 Ray direct(Vector3(50,70,81.6),Vector3(0,1,0),EPSILON_SPHERE);
 auto hit=Intersect(direct);
 if(!hit || *hit!=8 || !(direct.m_tmax>0))return 2;
 Vector3 light=Radiance(Ray(Vector3(50,70,81.6),Vector3(0,1,0),EPSILON_SPHERE),directRng);
 if((light-Vector3(12)).Norm2_squared()>1e-20)return 3;
 constexpr unsigned w=8,h=6,n=8;std::vector<Vector3> pixels(w*h);RNG rng(41);double sum=0;
 const Vector3 eye(50,52,295.6),gaze=Normalize(Vector3(0,-0.042612,-1));
 for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
  Vector3 d=Normalize(gaze+Vector3((double(x)+0.5-w/2.0)*0.09,(double(y)+0.5-h/2.0)*0.09,0));
  Vector3 c;for(unsigned k=0;k<n;++k)c+=Radiance(Ray(eye+d*130,d,EPSILON_SPHERE),rng)/double(n);
  if(c.HasNaNs() || !std::isfinite(c.Max()) || c.Min()<0)return 4;
  pixels[y*w+x]=c;sum+=c.m_x+c.m_y+c.m_z;
 }
 if(!(sum>0))return 5;
 std::remove(".task-build/legacy.ppm");WritePPM(w,h,pixels.data(),".task-build/legacy.ppm");
 std::ifstream in(".task-build/legacy.ppm");std::string magic;unsigned iw=0,ih=0,max=0;in>>magic>>iw>>ih>>max;
 if(magic!="P3"||iw!=w||ih!=h||max!=255)return 6;
 unsigned value,count=0;while(in>>value){if(value>255)return 7;++count;}if(count!=w*h*3)return 8;
 std::cout<<"SMALLPT_BASELINE emitter="<<*hit<<" hit_t="<<direct.m_tmax<<" direct="<<light.m_x<<" image_sum="<<sum<<" pixels="<<w*h<<" samples="<<w*h*n<<"\n";
 return 0;
}
