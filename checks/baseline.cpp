#include <cstring>
#include <cassert>
#include <iostream>
#include "vertexcm.hxx"
int main(int argc,char**argv) {
  assert(argc==2);
  Scene s; Material mat; mat.mDiffuseReflectance=Vec3f(.6f); s.mMaterials.push_back(mat);
  Triangle triangle(Vec3f(0,0,0),Vec3f(1,0,0),Vec3f(0,1,0),0);
  Ray r(Vec3f(.25f,.25f,1),Vec3f(0,0,-1),0); Isect hit(100);
  assert(triangle.Intersect(r,hit));
  BSDF<false> camera(r,hit,s); BSDF<true> light(r,hit,s);
  float c1,c2,p1,p2; Vec3f d=Normalize(Vec3f(1,0,1));
  Vec3f a=camera.Evaluate(s,d,c1,&p1), b=light.Evaluate(s,d,c2,&p2);
  assert(std::abs(a.x-.6f*INV_PI_F)<1e-6f && std::abs(a.x-b.x)<1e-6f);
  assert(std::abs(c1-std::sqrt(.5f))<1e-6f && p1>0 && p2==p1);
  std::cout << "VCM_FLAT_BSDF_OK\n";
  Scene box; box.LoadCornellBox(Vec2i(8,8),Scene::kLightCeiling);
  VertexCM renderer(box,VertexCM::kBpt,.003f,.75f,1234);
  renderer.mMaxPathLength=5; renderer.RunIteration(0);
  Framebuffer fb; renderer.GetFramebuffer(fb); fb.SavePFM(argv[1]);
  assert(renderer.WasUsed());
  assert(std::isfinite(fb.TotalLuminance()) && fb.TotalLuminance()>0);
  std::cout << "VCM_RENDER_BASELINE_OK 8x8 BPT\n";
}
