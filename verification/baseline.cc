#include "renderer/drcs_renderer.hpp"
#include "renderer/bitmap.hpp"
#include <cassert>
#include <cstdio>
int main() {
  using namespace aribcaption;
  DRCS drcs; drcs.width=3; drcs.height=1; drcs.depth=3; drcs.depth_bits=2;
  drcs.pixels={0x18}; // Three MSB-first samples: 0, 1, 2; final two bits padding.
  Bitmap bitmap(3,1,PixelFormat::kRGBA8888);
  DRCSRenderer renderer;
  assert(renderer.DrawDRCS(drcs,CharStyle::kCharStyleDefault,ColorRGBA(255,255,255,255),
       ColorRGBA(0),0,3,1,bitmap,0,0));
  auto a0=bitmap.GetPixelAt(0,0)->a,a1=bitmap.GetPixelAt(1,0)->a,a2=bitmap.GetPixelAt(2,0)->a;
  // Sample 1 of depth 3 has coverage floor(255 * 1 / 2) = 127, which becomes
  // 125 after the existing color-alpha and integer blending. It must not vanish.
  assert(a0==0 && a1==125 && a2==255);
  printf("baseline: depth=3 samples 0,1,2 produce alpha %u,%u,%u\n",a0,a1,a2);
}
