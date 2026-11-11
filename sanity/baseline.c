#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "datatypes/camera.h"
#include "renderer/samplers/sampler.h"
int main(void) {
 struct camera cam={.FOV=60,.width=16,.height=12,.focus_distance=5,
 .forward={0,0,1},.right={1,0,0},.up={0,1,0},.is_blender=true};
 cam_recompute_optics(&cam); cam_update_pose(&cam,&cam.orientation,&cam.position);
 struct sampler *a=sampler_new(),*b=sampler_new(); assert(a&&b);
 double worst=0;
 for(int p=0;p<8;p++) {
  sampler_init(a,Random,p,8,37);sampler_init(b,Random,p,8,37);
  cam.aperture=0; struct lightRay pin=cam_get_ray(&cam,3,4,a);
  cam.aperture=.02f; struct lightRay lens=cam_get_ray(&cam,3,4,b);
  struct vector x=alongRay(&pin,5/pin.direction.z);
  struct vector y=alongRay(&lens,(5-lens.start.z)/lens.direction.z);
  double err=fabs(x.x-y.x)+fabs(x.y-y.y)+fabs(x.z-y.z);
  assert(isfinite(err)&&err<2e-5); if(err>worst)worst=err;
  assert(sampler_dimension(a)==sampler_dimension(b));
 }
 printf("BASELINE focus_plane_z=5 samples=8 max_error=%.8g sampler_tail=equal\n",worst);
 sampler_destroy(a);sampler_destroy(b);return 0;
}
