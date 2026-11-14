#include "common.h"
#include "structs.h"
#include "drc.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static double sample(real_t x) {
#ifdef FIXED_POINT
 return (double)x/(1u<<REAL_BITS);
#else
 return x;
#endif
}
int main(void) {
 real_t spec[1024]; drc_info *d=drc_init(REAL_CONST(1),REAL_CONST(1));
 if(!d) return 2;
 d->prog_ref_level=DRC_REF_LEVEL; d->num_bands=1; d->dyn_rng_sgn[0]=0; d->dyn_rng_ctl[0]=24;
 for(int i=0;i<1024;i++) spec[i]=REAL_CONST(1);
 drc_decode(d,spec);
 if(fabs(sample(spec[0])-2)>0.001) return 3;
 printf("DRC_NORMAL gain=%.6f\n",sample(spec[0]));
 d->ctrl2=REAL_CONST(0);
 for(int i=0;i<1024;i++) spec[i]=REAL_CONST(1);
 drc_decode(d,spec);
 printf("DRC_DISABLED observed=%.6f wanted=1.000000\n",sample(spec[0]));
 d->ctrl2=REAL_CONST(1); d->dyn_rng_ctl[0]=12;
 for(int i=0;i<1024;i++) spec[i]=REAL_CONST(1);
 drc_decode(d,spec);
 printf("DRC_FRACTION observed=%.6f wanted=1.414214\n",sample(spec[0]));
 printf("DRC_960_CANARY changed=%d\n",spec[960]!=REAL_CONST(1));
 drc_end(d);return 0;
}
