#include "our_gl.h"
#include "model.h"
#include <cassert>
#include <fstream>
#include <iostream>
extern std::vector<double> zbuffer;
struct Flat : IShader {
    bool drop; TGAColor color;
    Flat(bool d,TGAColor c):drop(d),color(c){}
    std::pair<bool,TGAColor> fragment(vec3) const override { return {drop,color}; }
};
int main() {
    std::ofstream f(".task-build/plane.obj");
    f << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\nvn 0 0 1\nf 1/1/1 2/2/1 3/3/1\nf 1/1/1 3/3/1 4/4/1\n";f.close();
    Model mesh(".task-build/plane.obj");assert(mesh.nfaces()==2 && mesh.nverts()==4);
    init_viewport(0,0,16,16);init_zbuffer(16,16);
    TGAImage image(16,16,TGAImage::RGB);
    Triangle far={{-.8,-.8,0,1},{.8,-.8,0,1},{0,.8,0,1}};
    Triangle near={{-.8,-.8,.5,1},{.8,-.8,.5,1},{0,.8,.5,1}};
    Flat red(false,{{0,0,255,255}}), green(false,{{0,255,0,255}}), hole(true,{{255,0,0,255}});
    rasterize(far,red,image);assert(image.get(8,8)[2]==255 && zbuffer[136]==0);
    rasterize(near,hole,image);assert(image.get(8,8)[2]==255 && zbuffer[136]==0);
    rasterize(near,green,image);assert(image.get(8,8)[1]==255 && zbuffer[136]==.5);
    rasterize(far,red,image);assert(image.get(8,8)[1]==255);
    std::cout << "BASELINE_OK faces=2 foreground_depth=0.5 discard_preserved=1\n";
}
