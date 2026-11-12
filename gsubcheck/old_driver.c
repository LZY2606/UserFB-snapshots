/* Run the upstream TT_TEST body without changing its checks. Only its
   Windows system-font filename is redirected to our generated font. */
#include <stdio.h>
#include <string.h>
static FILE *gc_old_fopen(const char *name,const char *mode) {
  return fopen(strncmp(name,"c:/windows/fonts/",17)==0 ? "gsubcheck/.build/geometry.ttf" : name,mode);
}
#define STB_RECT_PACK_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define TT_TEST
#define fopen gc_old_fopen
#include "tests/test_truetype.c"
