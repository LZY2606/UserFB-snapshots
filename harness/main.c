#include "suite.h"
#include <stdio.h>
#include <string.h>
int main(void){Suite s={0};unsigned i,j,seen=0;const char*names[]={"history","window","wrapper","compatibility"};register_tests(&s);if(s.size!=4)return 1;
for(i=0;i<s.size;i++){Check c={0};for(j=0;j<4;j++)if(!strcmp(names[j],s.names[i]))break;if(j==4||(seen&(1u<<j)))return 1;seen|=1u<<j;s.funcs[i](&c);if(c.checks<3||c.failures)return 1;printf("GROUP %s PASS checks=%u\n",s.names[i],c.checks);}
puts("LIBDEFLATE_TARGET_COMPLETE groups=4");return 0;}
