#include <stdio.h>
#include <float.h>
#include <math.h>
#include <datatypes/sphere.h>
#include <datatypes/hitrecord.h>
#include <datatypes/bbox.h>
#include <renderer/instance.h>
int main(void){
 struct sphere spheres[1]={{.radius=1}};
 struct lightRay unit={.start={0,0,3},.direction={0,0,-1},.type=rt_camera};
 struct lightRay fast={.start={0,0,3},.direction={0,0,-2},.type=rt_camera};
 struct hitRecord a={.distance=FLT_MAX},b={.distance=FLT_MAX};
 if(!rayIntersectsWithSphere(&unit,spheres,&a)||fabsf(a.distance-2)>1e-5f)return 2;
 if(!rayIntersectsWithSphere(&fast,spheres,&b)||!isfinite(b.distance))return 3;
 printf("BASELINE_PRODUCTION_OK unit_t=%.6g\n",a.distance);
 printf("SPHERE_PARAMETER nonunit_t=%.6g reference_t=1 point_z=%.6g status=%s\n",b.distance,b.hitPoint.z,fabsf(b.distance-1)<1e-5f?"correct":"known_bug");
 struct sphere *array=spheres;
 struct instance inst=new_sphere_instance(&array,0,0,0);
 const struct bsdfNode *nodes[1]={0};struct bsdf_buffer buf={.bsdfs=nodes};inst.bbuf=&buf;
 struct boundingBox box;struct vector center;inst.getBBoxAndCenterFn(&inst,&box,&center);
 struct hitRecord c={.distance=FLT_MAX};
 if(!inst.intersectFn(&inst,&unit,&c,0))return 4;
 struct vector onray=alongRay(&unit,c.distance);
 printf("INSTANCE_PARAMETER t=%.9g hit_z=%.9g ray_at_t_z=%.9g residual=%.9g\n",c.distance,c.hitPoint.z,onray.z,fabsf(c.hitPoint.z-onray.z));
 return 0;
}
