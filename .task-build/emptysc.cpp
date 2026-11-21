#include <cstdio>
#include <cmath>
#include <iostream>
#include "fssr/iso_octree.h"
#include "fssr/iso_surface.h"
#include "fssr/stream_surface.h"
static void try_layout(int kind){
 fssr::IsoOctree t; t.set_max_level(4);
 if(kind==0){ // two opposing normals at same point -> basis cancels
   for(int k=0;k<2;++k){ fssr::Sample s; s.pos=math::Vec3f(0,0,0); s.normal=math::Vec3f(k? -1.f:1.f,0,0); s.scale=0.4f; s.confidence=1; s.color=math::Vec3f(.5f,.5f,.5f); t.insert_sample(s);} }
 else if(kind==1){ // single sample, far corner normal
   fssr::Sample s; s.pos=math::Vec3f(0,0,0); s.normal=math::Vec3f(0,0,1); s.scale=0.5f; s.confidence=1; s.color=math::Vec3f(.5f,.5f,.5f); t.insert_sample(s);}
 t.limit_octree_level(); t.compute_voxels();
 fssr::IsoSurface iso(&t,fssr::INTERPOLATION_CUBIC); auto m=iso.extract_mesh();
 std::cout<<"kind "<<kind<<" v="<<m->get_vertices().size()<<" t="<<m->get_faces().size()/3<<"\n";
}
int main(){try_layout(0);try_layout(1);}
