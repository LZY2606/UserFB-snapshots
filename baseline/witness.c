#include "microtar.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char raw[512];
static int rd(mtar_t *t, void *p, unsigned n) {
  if (t->pos > sizeof raw || n > sizeof raw - t->pos) return MTAR_EREADFAIL;
  memcpy(p, raw + t->pos, n); return 0;
}
static int sk(mtar_t *t, unsigned p) { (void)t; return p <= sizeof raw ? 0 : MTAR_ESEEKFAIL; }
int main(int argc, char **argv) {
  mtar_t t = {0}; mtar_header_t h; unsigned sum = 0, i;
  (void)argv;
  memset(raw, 0, sizeof raw); memcpy(raw, "ok", 2);
  memcpy(raw+100, "0000644", 7); memcpy(raw+108, "0000000", 7);
  memcpy(raw+116, "0000000", 7); memcpy(raw+124, "00000000000", 11);
  memcpy(raw+136, "00000000000", 11); raw[156]='0';
  if (argc > 1) memset(raw+157, 'L', sizeof raw-157);
  memset(raw+148, ' ', 8);
  for (i=0; i<512; ++i) sum += raw[i];
  snprintf((char*)raw+148, 8, "%06o", sum); raw[155]=' ';
  t.read=rd; t.seek=sk;
  assert(mtar_read_header(&t, &h)==0);
  assert(!strcmp(h.name,"ok") && h.size==0);
  puts("MICROTAR normal header PASS"); return 0;
}
