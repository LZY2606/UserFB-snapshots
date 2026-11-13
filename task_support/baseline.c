#define CGLTF_IMPLEMENTATION
#include "../cgltf.h"
#include <math.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"baseline:%d: %s\n",__LINE__,#x);exit(2);} } while(0)
int main(void) {
 cgltf_options opts={0};cgltf_data *d=NULL;
 CHECK(cgltf_parse_file(&opts,"scene.gltf",&d)==cgltf_result_success);
 CHECK(cgltf_load_buffers(&opts,d,"scene.gltf")==cgltf_result_success);
 CHECK(cgltf_validate(d)==cgltf_result_success);
 CHECK(d->nodes_count==5 && d->meshes_count==1 && d->skins_count==1);
 float v[16];cgltf_uint j[4];
 CHECK(cgltf_accessor_read_float(&d->accessors[0],1,v,3));CHECK(v[0]==1&&v[1]==0&&v[2]==0);
 CHECK(cgltf_accessor_read_uint(&d->accessors[1],2,j,4));CHECK(j[0]==0&&j[1]==1);
 CHECK(cgltf_accessor_read_float(&d->accessors[2],1,v,4));
 CHECK(fabs(v[0]-128.0/255)<1e-6 && fabs(v[1]-127.0/255)<1e-6);
 puts("BASELINE_INTERLEAVED_SKIN_INPUT_OK");
 CHECK(cgltf_accessor_read_float(&d->accessors[5],1,v,3));CHECK(v[2]==2);
 CHECK(cgltf_accessor_read_float(&d->accessors[5],0,v,3));CHECK(v[0]==0&&v[1]==0&&v[2]==0);
 cgltf_node_transform_world(&d->nodes[2],v);CHECK(v[12]==2);
 CHECK(d->skins[0].joints[1]==&d->nodes[3]);
 CHECK(cgltf_accessor_read_float(&d->accessors[4],1,v,16));CHECK(v[13]==-3);
 CHECK(d->nodes[0].weights[0]==0&&d->nodes[1].weights[0]==0.5f);
 puts("BASELINE_SPARSE_MORPH_BINDINGS_OK");
 cgltf_free(d);return 0;
}
