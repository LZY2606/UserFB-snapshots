#include <cassert>
#include <cmath>
#include <iostream>
#include "libs/yocto/yocto_scene.h"
#include "libs/yocto/yocto_shading.h"
int main() {
    using namespace yocto;
    scene_data scene;
    material_data material;
    material.type=material_type::reflective; material.color={.8f,.7f,.6f};
    material.roughness=.5f;
    material_point point; point.color=material.color; point.roughness=material.roughness*material.roughness;
    assert(std::abs(point.roughness-.25f)<1e-6);
    vec3f n={0,0,1},wo={0,0,1};
    auto wi=sample_reflective(point.color,point.roughness,n,wo,{.13f,.2f});
    auto pdf=sample_reflective_pdf(point.color,point.roughness,n,wo,wi);
    auto f=eval_reflective(point.color,point.roughness,n,wo,wi);
    assert(wi.z>0 && pdf>0 && f.x>0 && std::isfinite(pdf));
    auto a=eval_reflective(point.color,point.roughness,n,wo,normalize(vec3f{1,0,1}));
    auto b=eval_reflective(point.color,point.roughness,n,wo,normalize(vec3f{0,1,1}));
    assert(length(a-b)<1e-6);
    std::cout<<"isotropic metal: alpha="<<point.roughness<<", pdf="<<pdf<<", rotated-diff="<<length(a-b)<<"\n";
}
