#include <cassert>
#include <cmath>
#include <cstdio>
#include "noise/noise.h"
int main() {
  using namespace noise::module;
  Perlin control; control.SetSeed(17); control.SetOctaveCount(3);
  RidgedMulti ridges; ridges.SetSeed(11); ridges.SetOctaveCount(4);
  Const flat; flat.SetConstValue(0.15);
  Select select; select.SetSourceModule(0,flat); select.SetSourceModule(1,ridges);
  select.SetControlModule(control); select.SetBounds(-0.3,0.4); select.SetEdgeFalloff(0.12);
  Turbulence warp; warp.SetSourceModule(0,select); warp.SetSeed(31); warp.SetPower(0.17); warp.SetRoughness(3);
  RotatePoint rotate; rotate.SetSourceModule(0,warp); rotate.SetAngles(19,31,-13);
  Cache cache; cache.SetSourceModule(0,rotate);
  double sum=0;
  for(int i=0;i<30;i++) {
    double x=0.13+i*0.071,y=-0.27+i*0.033,z=0.41-i*0.017;
    double v=cache.GetValue(x,y,z); assert(std::isfinite(v));
    assert(v==cache.GetValue(x,y,z)); assert(v==rotate.GetValue(x,y,z)); sum+=v;
  }
  assert(std::abs(sum)>0.01); std::printf("BASELINE warped-ridged-graph PASS samples=30 sum=%.12g\n",sum);
}
