#include "thekla/thekla_atlas.h"
#include <cmath>
#include <iostream>
using namespace Thekla;
int main(){
 Atlas_Input_Vertex v[4]={};
 const float xy[4][2]={{0,0},{2,0},{2,1},{0,1}};
 for(int i=0;i<4;++i){v[i].position[0]=xy[i][0];v[i].position[1]=xy[i][1];v[i].normal[2]=1;v[i].uv[0]=xy[i][0];v[i].uv[1]=xy[i][1];v[i].first_colocal=i;}
 Atlas_Input_Face f[2]={{{0,1,2},0},{{0,2,3},0}};
 Atlas_Input_Mesh m={4,2,v,f};Atlas_Options o;atlas_set_default_options(&o);o.charter=Atlas_Charter_Extract;o.mapper_options.preserve_uvs=true;o.mapper_options.preserve_boundary=true;o.packer_options.witness.packing_quality=1;o.packer_options.witness.conservative=true;
 Atlas_Error e;auto *a=atlas_generate(&m,&o,&e);if(!a||e!=Atlas_Error_Success)return 2;
 if(a->index_count!=6||a->atlas_width<=0||a->atlas_height<=0)return 3;
 double area=0;
 for(int i=0;i<6;i+=3){const auto &x=a->vertex_array[a->index_array[i]],&y=a->vertex_array[a->index_array[i+1]],&z=a->vertex_array[a->index_array[i+2]];
  for(int j=0;j<3;++j){auto &t=a->vertex_array[a->index_array[i+j]];if(t.xref!=f[i/3].vertex_index[j]||!std::isfinite(t.uv[0])||!std::isfinite(t.uv[1])||t.uv[0]<0||t.uv[1]<0||t.uv[0]>a->atlas_width||t.uv[1]>a->atlas_height)return 4;}
  area+=std::abs((y.uv[0]-x.uv[0])*(z.uv[1]-x.uv[1])-(y.uv[1]-x.uv[1])*(z.uv[0]-x.uv[0]))*.5;
 }
 if(!(area>1))return 5;
 std::cout<<"BASELINE_ATLAS_OK faces=2 xref=match area="<<area<<" dimensions="<<a->atlas_width<<"x"<<a->atlas_height<<"\n";atlas_free(a);
}
