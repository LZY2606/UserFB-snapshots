#include <cstdio>
#include <cmath>
#include <map>
#include <vector>
#include <iostream>
#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"
#include "fssr/stream_surface.h"
static void sphere(fssr::IsoOctree* t, math::Vec3f c, float r, float sc, int n, int m){
  for(int i=1;i<n;++i)for(int j=0;j<m;++j){double a=3.141592653589793*i/n,b=6.283185307179586*j/m;fssr::Sample s;s.normal=math::Vec3f(std::sin(a)*std::cos(b),std::sin(a)*std::sin(b),std::cos(a));s.pos=c+math::Vec3f(s.normal*r);s.color=math::Vec3f(.2f,.5f,.8f);s.scale=sc;s.confidence=1.f;t->insert_sample(s);} }
static int comps(mve::TriangleMesh::Ptr m){ std::vector<int>p(m->get_vertices().size()); for(size_t i=0;i<p.size();++i)p[i]=i; std::function<int(int)>f=[&](int x){while(p[x]!=x){p[x]=p[p[x]];x=p[x];}return x;}; auto u=[&](int a,int b){a=f(a);b=f(b);if(a!=b)p[a]=b;}; auto&F=m->get_faces(); for(size_t i=0;i<F.size();i+=3){u(F[i],F[i+1]);u(F[i+1],F[i+2]);} int c=0; for(size_t i=0;i<p.size();++i)if(f(i)==(int)i)c++; return c; }
int main(){
 for(int trial=0;trial<6;++trial){
  float gap = 0.6f + 0.4f*trial; float r=0.4f, sc=0.08f;
  fssr::IsoOctree t; t.set_max_level(6);
  sphere(&t, math::Vec3f(-gap/2,0,0), r, sc, 20, 40);
  sphere(&t, math::Vec3f( gap/2,0,0), r, sc, 20, 40);
  t.limit_octree_level(); t.compute_voxels();
  fssr::IsoSurface iso(&t,fssr::INTERPOLATION_CUBIC); auto m=iso.extract_mesh();
  std::cout<<"trial "<<trial<<" gap="<<gap<<" v="<<m->get_vertices().size()<<" t="<<m->get_faces().size()/3<<" comps="<<comps(m)<<" levels="<<t.get_num_levels()<<"\n";
 }
}
