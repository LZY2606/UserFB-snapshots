#include "our_gl.h"
#include <iostream>
#include <stdexcept>
extern std::vector<double> zbuffer;
struct Solid:IShader { TGAColor c; Solid(TGAColor v):c(v){} std::pair<bool,TGAColor> fragment(vec3)const override{return {false,c};}};
int main(){init_viewport(0,0,16,16);init_zbuffer(16,16);TGAImage f(16,16,TGAImage::RGBA); Triangle a={vec4{-1,-1,.2,1},vec4{1,-1,.2,1},vec4{0,1,.2,1}};Solid s({0,0,255,128});rasterize(a,s,f);auto c=f.get(8,8);if(c[2]!=255||std::abs(zbuffer[8+8*16]-.2)>1e-12)throw std::runtime_error("opaque raster");std::cout<<"raster red="<<int(c[2])<<" alpha="<<int(c[3])<<" depth="<<zbuffer[136]<<"; alpha does not blend\n";}
