#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "check.h"
static struct {const char* name;void(*fn)();} cases[32];
static unsigned n,checks;
void unit_add(const char* name,void(*fn)()) {
 if(n==32||!name||!fn)std::exit(2);
 for(unsigned i=0;i<n;i++)if(!std::strcmp(cases[i].name,name))std::exit(2);
 cases[n].name=name;cases[n++].fn=fn;
}
void unit_expect(bool ok,const char* expr,int line) {
 if(!ok){std::fprintf(stderr,"MISMATCH line %d: %s\n",line,expr);std::exit(42);} checks++;
}
int main(){
 const char* groups[]={"camera","geometry","specular","texture","pipeline"};
 register_contract();
 if(n!=5){std::fputs("wrong case count\n",stderr);return 3;}
 for(unsigned j=0;j<5;j++){
 unsigned i;for(i=0;i<n;i++)if(!std::strcmp(groups[j],cases[i].name))break;
 if(i==n)return 4;
 unsigned before=checks;cases[i].fn();
 if(checks==before){std::fputs("zero checks\n",stderr);return 5;}
 std::printf("PASS %s %u\n",groups[j],checks-before);
 }
 std::puts("TARGET_COMPLETE");return 0;
}
