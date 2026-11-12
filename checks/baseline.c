#include "ufbx.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
 const char *files[]={"data/maya_dual_quaternion_7500_ascii.fbx","data/maya_dual_quaternion_scale_7500_ascii.fbx","data/maya_dq_weights_7500_ascii.fbx","data/maya_blend_inbetween_7500_ascii.fbx"};
 size_t total=0;
 for(size_t i=0;i<4;i++) {
  ufbx_load_opts opts={0}; opts.evaluate_skinning=true;
  ufbx_error error; ufbx_scene *s=ufbx_load_file(files[i],&opts,&error); assert(s);
  ufbx_evaluate_opts eval={0}; eval.evaluate_skinning=true;
  ufbx_scene *e=ufbx_evaluate_scene(s,s->anim,.5,&eval,&error); assert(e);
  size_t count=0;
  for(size_t m=0;m<e->meshes.count;m++) {
   ufbx_mesh *mesh=e->meshes.data[m];
   if(!mesh->skin_deformers.count && !mesh->blend_deformers.count) continue;
   assert(mesh->skinned_position.exists);
   for(size_t v=0;v<mesh->num_vertices;v++) {
    ufbx_vec3 p=mesh->skinned_position.values.data[v];
    assert(isfinite(p.x) && isfinite(p.y) && isfinite(p.z)); count++;
   }
  }
  assert(count>0); total+=count;
  ufbx_free_scene(e); ufbx_free_scene(s);
 }
 printf("UFBX_DEFORM_BASELINE_OK files=4 vertices=%zu\n",total);
}
