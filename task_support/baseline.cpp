#include <mutex>
#include <iostream>
#define main minpt_original_main
#include "minpt.cpp"
#undef main
static void demand(bool v,const char* s){if(!v)throw std::runtime_error(s);}
static bool close(V a,V b,F e=1e-12){return abs(a.x-b.x)<=e&&abs(a.y-b.y)<=e&&abs(a.z-b.z)<=e;}
int main() try {
 Scene sc;sc.load("task_support/scene.obj","",0);sc.build();
 demand(sc.trs.size()==4&&sc.Ls.size()==1,"scene load / BVH count");
 auto h=sc.isect({V(0,0,2),V(0,0,-1)});
 demand(bool(h)&&close(h->sp.p,V(0,0,0)),"emitter intersection");
 demand(close(h->o->ev(T::AreaL,h->sp,{},V(0,0,1)),V(2,3,5)),"front emission value");
 demand(close(h->o->ev(T::AreaL,h->sp,{},V(0,0,-1)),V()),"back emission value");
 auto floor=sc.isect({V(0,0,1),V(0,-1,0)});
 demand(bool(floor)&&close(floor->sp.p,V(0,-1,1)),"diffuse intersection");
 Rng rn(1771);V avg; int samples=5000,lights=0;
 for(int i=0;i<samples;i++){
  auto s=floor->o->samp(rn,T::NonS,floor->sp,V(0,1,0));
  demand(bool(s)&&s->t==T::D&&s->r.d.y>=0,"diffuse sampling");avg=avg+s->w;
  auto [lamp,prob]=sc.sampL(rn);auto ls=lamp.sampL(rn,sc.G,floor->sp);
  demand(prob==1,"light selection probability");
  if(ls){demand(ls->p>0&&isfinite(ls->p)&&ls->d>0,"area PDF");++lights;}
 }
 demand(close(avg/samples,V(.4,.5,.6),1e-10)&&lights>4900,"sampled content");
 const auto& light=sc.os[sc.Ls[0]];
 demand(abs(light.L.invA-1./3)<1e-12,"area distribution");
 std::ifstream f(".task-build/a.pfm",std::ios::binary);std::string magic;int w,hg;float scale;
 f>>magic>>w>>hg>>scale;f.get();std::vector<float> pixels(w*hg*3);f.read((char*)pixels.data(),pixels.size()*4);
 demand(magic=="PF"&&w==4&&hg==3&&scale==-1&&f.gcount()==(std::streamsize)pixels.size()*4,"CLI PFM layout");
 bool nonzero=false;for(float x:pixels){demand(std::isfinite(x)&&x>=0,"CLI radiance");nonzero|=x>0;}
 demand(nonzero,"CLI transport black");
 std::ifstream b(".task-build/b.pfm",std::ios::binary),a(".task-build/a.pfm",std::ios::binary);
 demand(std::string(std::istreambuf_iterator<char>(a),{})==std::string(std::istreambuf_iterator<char>(b),{}),"fixed RNG CLI replay");
 std::cout<<"SURFACE_LIGHT_OK samples="<<samples<<" area_samples="<<lights<<"\nLEGACY_CLI_OK\nBASELINE_OK\n";
} catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
