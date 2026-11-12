#include "opj_includes.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
 opj_tcd_t tcd={0};opj_tcd_tilecomp_t tile={0};opj_tcd_resolution_t res[2]={{0}};
 OPJ_INT32 a[5]={-3,4,-1,8,2}, want[5]={0,3,6,6,8};
 tcd.thread_pool=opj_thread_pool_create(0);assert(tcd.thread_pool);tcd.whole_tile_decoding=OPJ_TRUE;
 tile.x1=5;tile.y1=1;tile.numresolutions=2;tile.minimum_num_resolutions=2;tile.resolutions=res;tile.data=a;tile.data_size=sizeof(a);
 res[0].x1=3;res[0].y1=1;res[1].x1=5;res[1].y1=1;
 assert(opj_dwt_encode(&tcd,&tile));for(int i=0;i<5;i++)assert(a[i]==want[i]);
 puts("OPENJPEG_FORWARD_53_PASS");
 assert(opj_dwt_decode(&tcd,&tile,2));OPJ_INT32 original[5]={-3,4,-1,8,2};
 for(int i=0;i<5;i++)assert(a[i]==original[i]);
 puts("OPENJPEG_INVERSE_53_PASS");opj_thread_pool_destroy(tcd.thread_pool);
}
