#include "raytracer/world/World.hpp"
#include "raytracer/cameras/Perspective.hpp"
#include "raytracer/cameras/Parallel.hpp"
#include "raytracer/samplers/Simple.hpp"
#include "raytracer/samplers/Jittered.hpp"
#include "raytracer/geometry/Sphere.hpp"
#include "raytracer/geometry/Triangle.hpp"
#include "raytracer/materials/Matte.hpp"
#include "raytracer/tracers/Whitted.hpp"
#include "raytracer/utilities/Ray.hpp"
#include "raytracer/utilities/ShadeInfo.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
static void require(bool b){if(!b){std::fputs("production baseline assertion failed\n",stderr);std::exit(1);}}
static bool close(double a,double b){return std::abs(a-b)<1e-6;}
int main(){
 Matte material(.4f,.6f,RGBColor(.8f,.2f,.1f));World w;w.ambient=RGBColor(.5f);w.bg_color=RGBColor(.02f,.03f,.05f);
 w.camera_ptr=new Perspective(0,0,2);w.vplane.top_left=Point3D(-1,1,0);w.vplane.bottom_right=Point3D(1,-1,0);w.vplane.hres=w.vplane.vres=3;
 w.sampler_ptr=new Simple(w.camera_ptr,&w.vplane);w.tracer_ptr=new Whitted(&w);
 auto* center=new Sphere(Point3D(0,0,-3),1.5f);center->set_material(&material);w.add_geometry(center);
 auto*tri=new Triangle(Point3D(-2,-2,-6),Point3D(2,-2,-6),Point3D(0,2,-6));tri->set_material(&material);w.add_geometry(tri);
 for(int i=0;i<6;++i){auto*g=new Sphere(Point3D(8+3*i,0,-4),.5f);g->set_material(&material);w.add_geometry(g);}
 auto rays=w.sampler_ptr->get_rays(1,1);require(rays.size()==1&&close(rays[0].w,1));require(close(rays[0].o.x,0)&&close(rays[0].d.z,-1));
 auto before=w.hit_objects(rays[0]);auto light=w.tracer_ptr->trace_ray(rays[0],0);require(before.hit&&close(before.t,1.5));require(close(light.r,.16)&&close(light.g,.04)&&close(light.b,.02));
 w.build_accel();auto after=w.hit_objects(rays[0]);auto accelerated=w.tracer_ptr->trace_ray(rays[0],0);require(after.hit&&close(after.t,before.t));require(close(light.r,accelerated.r)&&close(light.g,accelerated.g)&&close(light.b,accelerated.b));
 unsigned hits=0;double sum=0;for(int y=0;y<3;++y)for(int x=0;x<3;++x)for(const auto&r:w.sampler_ptr->get_rays(x,y)){auto c=w.tracer_ptr->trace_ray(r,0);require(std::isfinite(c.r)&&std::isfinite(c.g)&&std::isfinite(c.b));sum+=r.w*(c.r+c.g+c.b);hits+=w.hit_objects(r).hit;}
 require(hits>0&&sum>0);Parallel parallel(0,0,-1);Simple ps(&parallel,&w.vplane);auto pr=ps.get_rays(0,0);require(pr.size()==1&&close(pr[0].d.z,-1)&&close(pr[0].o.x,-2.0/3));
 Jittered jitter(w.camera_ptr,&w.vplane,2);auto jr=jitter.get_rays(1,1);require(jr.size()==4);double weight=0;for(auto&r:jr)weight+=r.w;require(close(weight,1));
 std::printf("CAMERA_WORLD_WHITTED_OK t=%.6f rgb=%.6f,%.6f,%.6f hits=%u energy=%.8f\n",after.t,accelerated.r,accelerated.g,accelerated.b,hits,sum);
 puts("LEGACY_SAMPLERS_OK simple=1 jittered=4 weight=1");
}
