#include "ufbx.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
    ufbx_error error;
    ufbx_scene *scene=ufbx_load_file("data/maya_anim_linear_7700_ascii.fbx",NULL,&error);
    assert(scene && scene->anim_stacks.count);
    ufbx_bake_opts opts={0}; opts.step_handling=UFBX_BAKE_STEP_HANDLING_ADJACENT_DOUBLE;
    ufbx_baked_anim *bake=ufbx_bake_anim(scene,scene->anim,&opts,&error);
    assert(bake && bake->nodes.count==1);
    ufbx_baked_node *bn=&bake->nodes.data[0];
    double times[]={nextafter(.5,-INFINITY),.5,nextafter(.5,INFINITY)};
    double expected[]={0,2,0};
    for(size_t i=0;i<3;i++) {
        ufbx_vec3 v=ufbx_evaluate_baked_vec3(bn->translation_keys,times[i]);
        ufbx_scene *sample=ufbx_evaluate_scene(scene,scene->anim,times[i],NULL,&error);
        assert(sample && fabs(v.z-expected[i])<1e-9);
        assert(fabs(sample->nodes.data[bn->typed_id]->local_transform.translation.z-v.z)<1e-9);
        printf("BASELINE step[%zu] z=%.0f\n",i,v.z); ufbx_free_scene(sample);
    }
    printf("BASELINE nodes=%zu baked=%zu\n",scene->nodes.count,bake->nodes.count);
    ufbx_free_baked_anim(bake); ufbx_free_scene(scene); return 0;
}
