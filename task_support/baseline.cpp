#include "legacy.hpp"
#include <iostream>
#include <cassert>
int main(){ auto [hit,p,n,m]=scene_intersect({0,0,0},vec3{-3,0,-16}.normalized()); assert(hit); vec3 c=cast_ray({0,0,0},vec3{-3,0,-16}.normalized()); assert(std::isfinite(c.x)&&c.x>0); std::cout<<"REAL_CAST_RGB "<<c.x<<" "<<c.y<<" "<<c.z<<"\n"; double sum=0; for(int y=0;y<6;y++)for(int x=0;x<8;x++){vec3 d=vec3{float(x)-3.5f,2.5f-float(y),-6}.normalized(); vec3 q=cast_ray({0,0,0},d); sum+=q.x+q.y+q.z; } assert(sum>0); std::cout<<"RECURSIVE_IMAGE_SUM "<<sum<<"\n"; }
