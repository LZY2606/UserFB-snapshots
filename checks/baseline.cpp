#include "volumes/volumegrid.h"
#include <cassert>
#include <cstdio>
int main(){float density[8]={1,2,3,4,5,6,7,8};
 VolumeGridDensity g(Spectrum(.25f),Spectrum(.75f),0,Spectrum(0),BBox(Point(0,0,0),Point(1,1,1)),Scale(2,3,4),2,2,2,density);
 assert(std::abs(g.Density(Point(.5f,.5f,.5f))-4.5f)<1e-6f);
 assert(g.Density(Point(-1,0,0))==0);
 Ray r(Point(1,1.5f,-2),Vector(0,0,2),0,4);float t0,t1;assert(g.IntersectP(r,&t0,&t1));assert(t0==1&&t1==3);
 auto tau=g.tau(r,.001f,.5f);float rgb[3];tau.ToRGB(rgb);assert(std::abs(rgb[0]-18)<.03f);
 float srgb[3];g.sigma_t(Point(1,1.5f,2),Vector(0,0,1),0).ToRGB(srgb);assert(std::abs(srgb[0]-4.5f)<1e-6f);
 std::printf("PBRT_GRID_BASELINE_OK density=4.5 interval=1,3 tau=%.6f\n",rgb[0]);
}
