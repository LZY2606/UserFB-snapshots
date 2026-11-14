#ifndef DV_TASK_FIXTURE_H
#define DV_TASK_FIXTURE_H
#include "libdv/dv.h"
#include "libdv/headers.h"
#include <string.h>
/* Benign synthetic DIF container. Video payload is opaque and never decoded. */
static void task_make_dv(uint8_t *p, int pal, int frame_number) {
    size_t n=pal?144000:120000;
    for(size_t i=0;i<n;i++)p[i]=(uint8_t)((i*13u+i/97u+frame_number*7u)&255u);
    time_t when=946684800;
    _dv_write_meta_data(p,frame_number,pal,0,&when);
}
#endif
