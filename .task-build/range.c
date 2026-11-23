#include <stdio.h>
#include <stdint.h>
#include <limits.h>
int main(void){
  // Max internal sample: 8-tap luma filter coeff sum = 64, input 255, output after >>6 = 255*64 = 16320 internal (x64 scale again?)
  // hi output = filter>>6 where filter max ~ 64*255=16320, so hi sample up to 16320 (already the x64 internal)
  int internal_max = 255*64; // 16320
  int weight_extreme = (1<<7) + 127; // denom 7, delta +127 -> 255
  long long v = (long long)internal_max * weight_extreme;
  printf("internal_max=%d weight_max=%d product=%lld int32max=%d fits_int32=%d\n",
    internal_max, weight_extreme, v, INT32_MAX, v <= INT32_MAX);
  // negative weight extreme
  int wmin = (1<<7) - 128; // 0
  long long vn = (long long)(-internal_max) * 255;
  printf("neg product=%lld fits=%d\n", vn, vn >= INT32_MIN);
  return 0;}
