#include <ccd/ccd.h>
#include <cassert>
#include <cstdio>
struct Box {ccd_vec3_t center; ccd_real_t half;};
void support(const void* ptr,const ccd_vec3_t* d,ccd_vec3_t* out){
 auto& b=*static_cast<const Box*>(ptr);
 ccdVec3Set(out,ccdVec3X(&b.center)+(ccdVec3X(d)>=0?b.half:-b.half),ccdVec3Y(&b.center)+(ccdVec3Y(d)>=0?b.half:-b.half),ccdVec3Z(&b.center)+(ccdVec3Z(d)>=0?b.half:-b.half));
}
int main(){
 ccd_t ccd;CCD_INIT(&ccd);ccd.support1=ccd.support2=support;ccd.max_iterations=128;
 Box a,b;a.half=b.half=1;ccdVec3Set(&a.center,0,0,0);ccdVec3Set(&b.center,3,0,0);
 assert(!ccdGJKIntersect(&a,&b,&ccd));ccdVec3Set(&b.center,1.5,0,0);assert(ccdGJKIntersect(&a,&b,&ccd));
 ccd_real_t depth=0;ccd_vec3_t dir,pos;assert(ccdGJKPenetration(&a,&b,&ccd,&depth,&dir,&pos)==0);assert(depth>.49&&depth<.51);
 printf("BASELINE_CCD_PASS separated=1 intersecting=1 depth=%.3f\n",double(depth));
}
