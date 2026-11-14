#include "renderer.h"
#include <cstdio>
#include <tuple>
struct Vertex{Vec4 p;};
struct Fragment{Vec4 position;VARYING(position);};
struct Uniforms{};
int main(){
 using FB=Framebuffer<RGBAF,Depth>;FB fb(8,8);fb.clear();
 Program<Vertex,Fragment,Uniforms,FB> p;
 p.onVertex([](const Uniforms&,const Vertex&v,Fragment&f){f.position=v.p;});
 p.onFragment([](const Uniforms&,const Fragment&,FB::TargetFragments& out){std::get<0>(out)=Vec4(4,2,1,1);});
 Buffer<Vertex>b;b.vertices={{{-.8f,-.7f,0,1}},{{.65f,-.7f,0,1}},{{-.1f,.8f,0,1}}};
 Renderer r(8,8);r.options().culling=false;r.draw(p,b,fb);
 int covered=0;for(auto c:fb.target<0>().data()){if(c.x==4 && c.y==2)covered++;else if(c.x!=0)return 1;}
 if(covered!=18)return 2;
 puts("HDR_BASELINE_OK covered=18 max_linear=4");return 0;}
