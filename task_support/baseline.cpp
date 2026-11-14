#include "TriangleDrawer.h"
#include <cstdio>
#include <limits>
#include <cmath>
using namespace al::graphics;
static Triangle tri(double w, u32 color) {
  Triangle t(3,3,0, 3,20,0, 20,3,0,color);
  for(int i=0;i<3;++i) t[i].pos.w()=w;
  t.setTexUVs(0.25f,0.25f,0.25f,0.25f,0.25f,0.25f);
  return t;
}
int main() {
  const u32 red=ImageData::makePixel(240,20,10,255);
  const u32 green=ImageData::makePixel(10,200,30,255);
  for(int a=0;a<4;++a) {
    ImageData image(32,24),texture(2,2);image.fill(0);texture.fill(0);
    ZBuffer z(32,24);z.fill(std::numeric_limits<double>::infinity());
    TriangleDrawer d(&z);d.algo=(TriangleDrawer::Algo)a;
    d.lightsStyle=TriangleDrawer::LIGHTS_STYLE_NONE;
    d.drawMode=TriangleDrawer::DRAW_SOLID;
    d.triangle(image,tri(3,red));d.triangle(image,tri(6,green));
    if(image.getPixel(7,7)!=red || std::abs(z.unsafeGet(7,7)-3)>1e-6) return 10+a;
    d.drawMode=TriangleDrawer::DRAW_PERSPECTIVE;d.textureImageData=&texture;
    d.triangle(image,tri(1,green));
    if(image.getPixel(7,7)!=red || std::abs(z.unsafeGet(7,7)-3)>1e-6) return 20+a;
    d.drawMode=TriangleDrawer::DRAW_SOLID;d.triangle(image,tri(2,green));
    if(image.getPixel(7,7)!=green || std::abs(z.unsafeGet(7,7)-2)>1e-6) return 30+a;
    std::printf("ALGO %d depth=2 alpha_hole_preserved=1\n",a);
  }
  std::puts("BASELINE_OK four_algorithms solid_depth alpha_discard");
}
