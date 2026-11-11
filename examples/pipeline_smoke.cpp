// Synthetic public smoke data; no external assets required.
#include "our_gl.h"
#include <iostream>
struct Solid : IShader {
    std::pair<bool,TGAColor> fragment(vec3) const override { return {false, {255,255,255,255}}; }
};
int main() {
    init_viewport(0,0,16,16); init_zbuffer(16,16);
    TGAImage image(16,16,TGAImage::RGB);
    Triangle t = {{-.5,-.5,0,1},{.5,-.5,0,1},{-.5,.5,0,1}};
    rasterize(t, Solid{}, image);
    int covered=0;
    for(int y=0;y<16;y++) for(int x=0;x<16;x++) covered += image.get(x,y)[0]!=0;
    image.write_tga_file("pipeline_smoke.tga");
    std::cout << "covered=" << covered << "\n";
    return covered==45 ? 0 : 1;
}
