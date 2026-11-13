#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "pathtracer.hxx"
#include "vertexcm.hxx"
#define CHECK(x) do { if(!(x)){std::fprintf(stderr,"baseline failed line %d: %s\n",__LINE__,#x);std::exit(1);} }while(0)
static float run(AbstractRenderer& r){
 r.mMaxPathLength=4;
 for(int i=0;i<8;i++)r.RunIteration(i);
 Framebuffer f;r.GetFramebuffer(f);float l=f.TotalLuminance();CHECK(std::isfinite(l)&&l>0);return l;
}
int main(){
 Scene s;s.LoadCornellBox(Vec2i(8,8),Scene::kLightCeiling|Scene::kGlossyFloor);
 Camera &c=s.mCamera;
 const Vec2f p(2.25f,4.75f);Ray r=c.GenerateRay(p);
 CHECK((r.org-c.mPosition).LenSqr()==0);CHECK(std::fabs(r.dir.LenSqr()-1)<1e-5f);
 Vec2f q=c.WorldToRaster(r.org+3.f*r.dir);CHECK(std::fabs(q.x-p.x)<1e-4f&&std::fabs(q.y-p.y)<1e-4f);
 PathTracer pt(s,1234);VertexCM bpt(s,VertexCM::kBpt,.003f,.75f,1234);
 VertexCM lt(s,VertexCM::kLightTrace,.003f,.75f,1234);VertexCM vcm(s,VertexCM::kVcm,.003f,.75f,1234);
 float ptl=run(pt),bptl=run(bpt),ltl=run(lt),vcml=run(vcm);
 std::printf("SMALLVCM_BASELINE_OK camera=ok pt=%.9g bpt=%.9g lt=%.9g vcm=%.9g\n",ptl,bptl,ltl,vcml);
}
