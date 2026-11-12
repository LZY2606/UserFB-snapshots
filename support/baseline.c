#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "chipmunk/chipmunk.h"
int main(void) {
  cpSpace *s=cpSpaceNew(); cpSpaceSetGravity(s,cpv(0,-10));
  cpBody *b=cpSpaceAddBody(s,cpBodyNew(1,cpMomentForCircle(1,0,0.5,cpvzero)));
  cpBodySetPosition(b,cpv(0,3));
  cpShape *floor=cpSpaceAddShape(s,cpSegmentShapeNew(cpSpaceGetStaticBody(s),cpv(-10,0),cpv(10,0),0));
  cpShape *ball=cpSpaceAddShape(s,cpCircleShapeNew(b,0.5,cpvzero));
  cpShapeSetFriction(floor,0.7); cpShapeSetFriction(ball,0.7);
  cpConstraint *j=cpSpaceAddConstraint(s,cpDampedSpringNew(cpSpaceGetStaticBody(s),b,cpv(0,3),cpvzero,2.5,0.2,0.1));
  for(int i=0;i<240;i++) cpSpaceStep(s,1.0/120);
  cpVect p=cpBodyGetPosition(b); assert(isfinite(p.y)&&p.y>0.3&&p.y<0.8);
  assert(isfinite(cpConstraintGetImpulse(j))); puts("BASELINE contact-and-spring PASS");
  cpSpaceRemoveConstraint(s,j); cpConstraintFree(j);
  cpSpaceRemoveShape(s,ball); cpShapeFree(ball); cpSpaceRemoveShape(s,floor); cpShapeFree(floor);
  cpSpaceRemoveBody(s,b); cpBodyFree(b); cpSpaceFree(s); return 0;
}
