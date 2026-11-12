#include "source/xatlas/xatlas_c.h"
int c_header_sanity(void) {
 xatlasPackOptions p;xatlasPackOptionsInit(&p);
 xatlasAtlas *a=xatlasCreate();
 int ok=a && p.padding==0 && p.rotateCharts;
 xatlasDestroy(a);return ok;
}
