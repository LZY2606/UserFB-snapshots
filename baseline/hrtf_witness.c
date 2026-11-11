/* Starting behavior only: no spherical triangulation or new API. */
#include "mysofa.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  struct MYSOFA_ATTRIBUTE type = {NULL, "Type", "cartesian"};
  float positions[18] = {1,0,0, 0,1,0, 0,0,1, -1,0,0, 0,-1,0, 0,0,-1};
  float ir[36] = {0}, delay[12], rate=48000;
  struct MYSOFA_HRTF h = {0};
  h.I=1; h.C=3; h.R=2; h.E=1; h.N=3; h.M=6;
  h.SourcePosition=(struct MYSOFA_ARRAY){positions,18,&type};
  h.DataIR=(struct MYSOFA_ARRAY){ir,36,NULL};
  h.DataDelay=(struct MYSOFA_ARRAY){delay,12,NULL};
  h.DataSamplingRate=(struct MYSOFA_ARRAY){&rate,1,NULL};
  for (int m=0;m<6;m++) {
    for (int i=0;i<3;i++) {ir[m*6+i]=(m==i); ir[m*6+3+i]=(m==i)?2:0;}
    delay[m*2]=(float)m; delay[m*2+1]=(float)(m+10);
  }
  struct MYSOFA_LOOKUP *lookup=mysofa_lookup_init(&h);
  if (!lookup) return 1;
  struct MYSOFA_NEIGHBORHOOD *neighbors=mysofa_neighborhood_init(&h,lookup);
  if (!neighbors) return 2;
  float fir[6], delays[2];
  float exact[3]={1,0,0};
  int nearest=mysofa_lookup(lookup,exact);
  float *out=mysofa_interpolate(&h,exact,nearest,mysofa_neighborhood(neighbors,nearest),fir,delays);
  if (memcmp(out,ir,6*sizeof(float)) || delays[0]!=0 || delays[1]!=10) return 3;
  puts("BASELINE_EXACT_MATCH_OK");
  float c[3]={2/sqrtf(6),1/sqrtf(6),1/sqrtf(6)};
  nearest=mysofa_lookup(lookup,c);
  out=mysofa_interpolate(&h,c,nearest,mysofa_neighborhood(neighbors,nearest),fir,delays);
  printf("BASELINE_LEGACY_DIRECTION=%.6f,%.6f,%.6f delay=%.6f,%.6f\n",out[0],out[1],out[2],delays[0],delays[1]);
  if (!isfinite(out[0]) || !isfinite(out[1]) || !isfinite(out[2])) return 4;
  if (fabsf(out[0]-0.5f)+fabsf(out[1]-0.25f)+fabsf(out[2]-0.25f)<0.01f) return 5;
  puts("BASELINE_LEGACY_DIFFERS_FROM_RAY_BARYCENTRIC");
  mysofa_neighborhood_free(neighbors); mysofa_lookup_free(lookup);
  return 0;
}
