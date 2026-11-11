#include <cassert>
#include <cstdint>
#include <tuple>
#include <string>
#include <vector>
#include <iostream>
#include "examples/pbr_surface/pbr_maths.hh"
#include "examples/pbr_surface/mesh.h"
int main() {
    Mesh<float> m;
    m.vertices={0,0,0, 1,0,0, 0,1,0}; m.faces={0,1,2};
    m.facevarying_uvs={0,0, 1,0, 0,1};
    pbr_maths::PBRShaderCPU<float> s;
    pbr_maths::sampler2D<float>::pixel p={255,128,255,255};
    s.u_NormalSampler.width=s.u_NormalSampler.height=1;
    s.u_NormalSampler.pixels=&p;
    s.u_NormalSampler.boundsOperation=decltype(s.u_NormalSampler)::outOfBounds::clamp;
    s.useNormalMap=true; s.v_Normal={0,0,1}; s.v_UV=m.getTextureCoord(0,.2f,.3f);
    auto n=s.getNormal();
    assert(n.x<-.7f && n.z>.7f);
    assert(glm::length(n)>.999f);
    std::cout<<"identity UV normal="<<n.x<<","<<n.y<<","<<n.z<<" (expected positive x)\n";
}
