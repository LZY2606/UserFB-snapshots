/* Deterministic policy text fixture. This does not evaluate or compile it. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char *repeated_policy(unsigned depth) {
  char *a=malloc(32768), *b=malloc(32768), *out=malloc(33000);
  const char *pairs[][2]={{"c","d"},{"e","f"},{"a","c"},{"b","e"},{"d","f"},{"a","e"}};
  unsigned i; if(!a||!b||!out||depth>6) abort();
  strcpy(a,"(a & b)");
  for(i=0;i<depth;i++) {
    int n=snprintf(b,32768,"((%s %c %s) %c (%s %c %s))",a,i%2?'|':'&',pairs[i][0],i%2?'&':'|',a,i%2?'|':'&',pairs[i][1]);
    char *t=a; if(n<0||n>=32768) abort(); a=b;b=t;
  }
  snprintf(out,33000,"ALLOW { SYSCALL[123](a,b,c,d,e,f) { %s == 0x100000001 } } DEFAULT ERRNO(7)",a);
  free(a);free(b);return out;
}
