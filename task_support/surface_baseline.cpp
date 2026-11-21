#include "RayTracer.h"
#include "BSP.h"
#include "Ray.h"
#include "Intersection.h"
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <cstdio>
int main(){
 RayTracer r(16,12,3,1,1);
 std::istringstream scene("cameraPosition 0 0 6 cameraLookAt 0 0 0 cameraScreenWidth 8 sphere 0 0 0 1 FlatColor 0.8 0.4 0.2 null light 0 3 5 0.8");
 r.readScene(scene);
 Ray ray(Vector(0.1,0.15,6),Vector(0,0,-1),3,r.startingMaterial);
 Intersection h=r.bsp->getClosestIntersection(ray);
 double expected=6-0.001-std::sqrt(1-0.1*0.1-0.15*0.15);
 if(!h.didIntersect || std::abs(h.distance-expected)>1e-10) return 2;
 std::remove(".task-build/surface.tga");
 r.traceRays(".task-build/surface.tga");
 std::ifstream in(".task-build/surface.tga",std::ios::binary);
 std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),{});
 if(bytes.size()!=18+16*12*3 || bytes[2]!=2 || bytes[12]!=16 || bytes[14]!=12) return 3;
 unsigned sum=0;for(std::size_t i=18;i<bytes.size();++i)sum+=bytes[i];
 if(sum==0 || r.raysCast<16*12) return 4;
 std::cout<<"SURFACE_BASELINE hit_t="<<h.distance<<" rays="<<r.raysCast<<" pixel_sum="<<sum<<" bytes="<<bytes.size()<<"\n";
 return 0;
}
