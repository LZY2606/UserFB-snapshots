#include <sstream>
#include <iostream>
#include <cassert>
#include <cmath>
#include "Scene.hpp"
#include "Camera.hpp"
#include "Image.hpp"
#include "Random.hpp"
#include "RayTracer.hpp"
using namespace hxa7241_minilight;
int main(){
 std::istringstream data("(0.1 0.2 0.3) (1 1 1)\n(-2 -2 0) (2 -2 0) (0 2 0) (.5 .5 .5) (0 0 0)\n(1 1 -1) (2 1 -1) (1 2 -1) (0 0 0) (4 3 2)\n");data.exceptions(std::ios::badbit|std::ios::eofbit);
 Scene scene(data,Vector3f(0,0,-2));Random rnd;RayTracer tracer(scene);Vector3f c=tracer.getRadiance(Vector3f(0,0,-2),Vector3f(0,0,1),rnd);assert(c[0]>0&&std::isfinite(c[0]));std::cout<<"MINILIGHT_RADIANCE "<<c[0]<<" "<<c[1]<<" "<<c[2]<<"\n";
 std::istringstream cs("(0 0 -2) (0 0 1) 45"),is("8 6");Camera cam(cs);Image image(is);cam.getFrame(scene,rnd,image);std::ostringstream out;image.getFormatted(out,1);std::string bytes=out.str();assert(bytes.find("P6\n")==0&&bytes.size()>144);unsigned long long hash=1469598103934665603ull;for(unsigned char b:bytes)hash=(hash^b)*1099511628211ull;std::cout<<"REAL_CAMERA_PPM "<<bytes.size()<<" "<<hash<<"\n";
}
