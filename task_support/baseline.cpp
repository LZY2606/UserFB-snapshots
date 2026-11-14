#include <iostream>
#include <cassert>
#include "pathtracer.hxx"
#include "vertexcm.hxx"
int main(){ Scene s; s.LoadCornellBox(Vec2i(8,8),Scene::kLightCeiling|Scene::kSmallMirrorSphere); PathTracer pt(s,17); pt.mMaxPathLength=5; pt.RunIteration(0); Framebuffer f; pt.GetFramebuffer(f); float a=f.TotalLuminance(); assert(std::isfinite(a)&&a>0); std::cout<<"PT_REAL_LUMINANCE "<<a<<"\n"; VertexCM bpt(s,VertexCM::kBpt,0.003f,0.75f,17); bpt.mMaxPathLength=5; bpt.RunIteration(0); bpt.GetFramebuffer(f); float b=f.TotalLuminance(); assert(std::isfinite(b)&&b>0); std::cout<<"BPT_REAL_LUMINANCE "<<b<<"\n"; }
