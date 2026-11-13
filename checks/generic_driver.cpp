#include <cstdio>
extern int generic_expression_case(const char *, unsigned *);
int main() {
  const char *groups[]={"types","effects","facts","clone","generated","compatibility"};
  unsigned total=0;
  for(auto g:groups) { unsigned n=0; if(generic_expression_case(g,&n)!=0 || !n) return 1;
    total+=n; std::printf("PASS %s checks=%u\n",g,n); }
  std::printf("CSMITH_GENERIC_OK groups=6 checks=%u\n",total);
}
