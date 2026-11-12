#include <stdio.h>
extern int atom_prefilter_case(const char *, unsigned *);
int main(void) {
  const char *groups[]={"coverage","intervals","priority","fallback","reuse","work"};
  unsigned total=0;
  for(unsigned i=0;i<6;i++) { unsigned n=0; if(atom_prefilter_case(groups[i],&n)!=0 || !n) return 1;
    total+=n; printf("PASS %s checks=%u\n",groups[i],n); }
  printf("ONIG_ATOM_PREFILTER_OK groups=6 checks=%u\n",total);
}
