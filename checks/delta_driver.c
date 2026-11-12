#include <stdio.h>
extern int overlay_delta_case(const char *, unsigned *);
int main(void) {
  const char *groups[]={"diff","external","local","renumber","roundtrip","reject"};
  unsigned total=0;
  for(unsigned i=0;i<6;i++) { unsigned n=0; if(overlay_delta_case(groups[i],&n)!=0 || !n) return 1;
    total+=n; printf("PASS %s checks=%u\n",groups[i],n); }
  printf("OVERLAY_DELTA_OK groups=6 checks=%u\n",total);
}
