#include "scene.h"
#include "renderer.h"
#include <cmath>
#include <fstream>
#include <iostream>
int main(){
 std::ofstream o(".task-build/tri.obj");
 o << "v -2 -2 0\nv 2 -2 0\nv 0 2 0\nf 1 2 3\n";o.close();
 Material emit(EMIT,Vec(1,1,1),Vec(2,3,4));
 Sphere lamp(Vec(0,0,0),1,emit);Scene s;s.add(&lamp);
 unsigned short xi[3]={1,2,3};Ray r(Vec(0,-3,0),Vec(0,1,0));
 ObjectIntersection h=s.intersect(r);Vec col=s.trace_ray(r,0,xi);
 if(!h.hit||fabs(h.u-2)>1e-10||fabs(col.x-2)>1e-10||fabs(col.y-3)>1e-10)return 2;
 Mesh mesh(Vec(),".task-build/tri.obj",Material());
 ObjectIntersection m=mesh.get_intersection(Ray(Vec(.1,.1,-2),Vec(0,0,1)));
 if(!m.hit||fabs(m.u-2)>1e-10)return 3;
 Camera cam(Vec(0,-3,0),Vec(),8,8);Renderer renderer(&s,&cam);renderer.render(1);
 renderer.save_image(".task-build/static.png");std::ifstream f(".task-build/static.png",std::ios::binary|std::ios::ate);
 if(!f||f.tellg()<40)return 4;
 std::cout << "STATIC sphere_distance="<<h.u<<" mesh_distance="<<m.u<<" rgb="<<col.x<<","<<col.y<<","<<col.z<<"\nBASELINE_COMPLETE\n";
}
