#include <stdint.h>
#include <stddef.h>
#include "src/ogt_voxel_meshify.h"
#include <cassert>
#include <cstdio>
int main() {
  ogt_voxel_meshify_context ctx={};ogt_mesh_rgba p[256]={};
  p[1]={1,2,3,255};p[2]={101,102,103,0};
  uint8_t two[2]={1,2}, one[2]={1,1};
  ogt_mesh* m=ogt_mesh_from_paletted_voxels_greedy(&ctx,two,2,1,1,p);
  ogt_mesh* n=ogt_mesh_from_paletted_voxels_greedy(&ctx,one,2,1,1,p);
  assert(m&&n&&m->index_count==60&&n->index_count==36);
  unsigned counts[3]={};
  for(unsigned i=0;i<m->vertex_count;++i){
    const auto& v=m->vertices[i];assert(v.palette_index==1||v.palette_index==2);
    ++counts[v.palette_index];
    assert(v.color.r==p[v.palette_index].r&&v.color.a==p[v.palette_index].a);
  }
  assert(counts[1]&&counts[2]);
  ogt_mesh_destroy(&ctx,m);ogt_mesh_destroy(&ctx,n);
  std::puts("BASELINE color-split PASS colored=20 uniform=12 alpha-zero=solid");
}
