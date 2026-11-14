#include "task_support/fixture.hpp"
#include <iostream>
#include <iomanip>
#include <cmath>
#include <cstdint>
using namespace hxa7241_minilight;
static uint64_t hash(const std::string&s){uint64_t h=14695981039346656037ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}return h;}
int main(){
  std::istringstream ci(author_fixture::camera_text());author_fixture::input(ci);Camera camera(ci);
  std::istringstream ei(author_fixture::emitter_text());author_fixture::input(ei);Scene emitter(ei,camera.getEyePoint());
  Random rng;RayTracer rt(emitter);Vector3f value=rt.getRadiance(Vector3f(0,0,0),Vector3f(0,0,1),rng);
  if(value[0]!=2 || value[1]!=3 || value[2]!=4)return 1;
  std::cout<<"EMITTER_RADIANCE "<<value[0]<<","<<value[1]<<","<<value[2]<<"\n";
  std::istringstream si(author_fixture::diffuse_text());author_fixture::input(si);Scene diffuse(si,camera.getEyePoint());
  std::istringstream ii("4 3 ");Image image(ii);Random first;RayTracer tracer(diffuse);
  unsigned positive=0,black=0;double sum=0;
  for(int i=0;i<64;++i){auto v=tracer.getRadiance(Vector3f(0,0,0),Vector3f(0,0,1),first);if(v[0]>0)++positive;else ++black;sum+=v[0];}
  if(!positive || !black || !std::isfinite(sum))return 2;
  Random frames;camera.getFrame(diffuse,frames,image);camera.getFrame(diffuse,frames,image);
  std::ostringstream out;image.getFormatted(out,2);const auto ppm=out.str();
  if(ppm.substr(0,2)!="P6" || ppm.size()<36)return 3;
  std::cout<<"PATH_BASELINE positive="<<positive<<" black="<<black<<" sum_r="<<sum<<"\n";
  std::cout<<"LEGACY_FILM bytes="<<ppm.size()<<" fnv1a="<<std::hex<<hash(ppm)<<std::dec<<"\n";
  Random sequence;std::cout<<"DEFAULT_RNG";for(int i=0;i<4;++i)std::cout<<" "<<sequence.getInt32u();std::cout<<"\nBASELINE_OK\n";
}
