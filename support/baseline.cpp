#define main upstream_demo_main
#include "../tinyraytracer.cpp"
#undef main
#include <cassert>
#include <iostream>
int main(){
 auto [hit,t]=ray_sphere_intersect({-3,0,0},{0,0,-1},spheres[0]);
 assert(hit&&std::abs(t-14)<1e-5);
 auto [scenehit,p,n,m]=scene_intersect({-3,0,0},{0,0,-1});
 assert(scenehit&&std::abs(p.z+14)<1e-5);
 assert(std::abs(n.norm()-1)<1e-5);
 vec3 c=cast_ray({0,0,0},vec3{-3,0,-16}.normalized());
 assert(std::isfinite(c.x)&&std::isfinite(c.y)&&std::isfinite(c.z));
 assert(c.norm()>0);
 vec3 sky=cast_ray({0,0,0},{0,1,0});
 assert(std::abs(sky.y-.7)<1e-5);
 std::cout<<"BASE surface_and_recursive_trace PASS\n";
}
