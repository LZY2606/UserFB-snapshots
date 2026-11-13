#define main legacy_demo_main
#include "../tinyraytracer.cpp"
#undef main
#include <iostream>
#include <stdexcept>
int main(){auto [hit,p,n,m]=scene_intersect({0,0,0},{0,0,-1});auto c=cast_ray({0,0,0},{0,0,-1});auto bg=cast_ray({0,0,0},{0,1,0});if(!hit||std::abs(m.refractive_index-1.5f)>1e-6||!std::isfinite(c.x)||c.x<=0||std::abs(bg.y-.7f)>1e-6)throw std::runtime_error("legacy trace");std::cout<<"scalar glass RGB="<<c.x<<","<<c.y<<","<<c.z<<"; background="<<bg.x<<","<<bg.y<<","<<bg.z<<"; no Stokes state\n";}
