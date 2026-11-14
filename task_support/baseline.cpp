#include "wfc.hpp"
#include "overlapping_wfc.hpp"
#include <cassert>
#include <cstdio>

static Propagator::PropagatorState checker_rules() {
  Propagator::PropagatorState rules(2);
  for (unsigned p=0;p<2;++p)
    for (unsigned d=0;d<4;++d) rules[p][d].push_back(1-p);
  return rules;
}
static void check_checker(const Array2D<unsigned>& a) {
  assert(a.data.size()==a.width*a.height);
  for (unsigned y=0;y<a.height;++y) for (unsigned x=0;x<a.width;++x) {
    assert(a.get(y,x)<2);
    if(x) assert(a.get(y,x)!=a.get(y,x-1));
    if(y) assert(a.get(y,x)!=a.get(y-1,x));
  }
}
int main() {
  auto rules=checker_rules();
  WFC core(false,731,{1.0,3.0},rules,3,4);
  core.remove_wave_pattern(0,0,1);
  core.propagate();
  auto r=core.run(); assert(r); check_checker(*r); assert(r->get(0,0)==0);
  WFC impossible(true,731,{1.0,3.0},rules,3,3);
  assert(!impossible.run());
  std::puts("BASELINE core PASS pinned=0 odd-torus=impossible");
  Array2D<unsigned> input(2,2);input.data={0,1,1,0};
  OverlappingWFCOptions o{true,false,5,7,1,false,2};
  OverlappingWFC<unsigned> a(input,o,731);
  auto image=a.run();assert(image);check_checker(*image);
  assert(image->height==5&&image->width==7);
  o.periodic_output=true;o.out_height=4;o.out_width=6;
  OverlappingWFC<unsigned> b(input,o,731);
  auto torus=b.run();assert(torus);check_checker(*torus);
  for(unsigned y=0;y<torus->height;++y)assert(torus->get(y,0)!=torus->get(y,torus->width-1));
  for(unsigned x=0;x<torus->width;++x)assert(torus->get(0,x)!=torus->get(torus->height-1,x));
  std::puts("BASELINE overlapping PASS open=5x7 periodic=4x6 N=2");
}
