#include "our_gl.h"
#include <cassert>
#include <iostream>
struct Flat : IShader {
 mutable int calls=0;
 std::pair<bool,TGAColor> fragment(vec3 b) const override {
  assert(std::abs(b.x+b.y+b.z-1)<1e-9); ++calls;
  return {false,{30,60,90,255}};
 }
};
int main(){
 init_viewport(0,0,16,16);init_zbuffer(16,16);
 TGAImage image(16,16,TGAImage::RGB);
 Triangle t={{-.8,-.8,0,1},{.8,-.8,0,1},{0,.8,0,1}};
 Flat shader;rasterize(t,shader,image);
 assert(shader.calls>30); assert(image.get(8,8)[0]==30);
 TGAImage tex(2,2,TGAImage::RGB);tex.set(1,1,{10,20,30,255});
 auto c=IShader::sample2D(tex,{.75,.75});assert(c[1]==20);
 std::cout<<"BASE raster_and_texture PASS\n";
}
