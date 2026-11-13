#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
static struct {const char* name;void(*fn)(void);} cases[32];
static unsigned n,checks;
void unit_add(const char* name,void(*fn)(void)) {
 if(n==32||!name||!fn)exit(2);
 for(unsigned i=0;i<n;i++)if(!strcmp(cases[i].name,name))exit(2);
 cases[n].name=name;cases[n++].fn=fn;
}
void unit_expect(int ok,const char* expr,int line) {
 if(!ok){fprintf(stderr,"MISMATCH line %d: %s\n",line,expr);exit(42);} checks++;
}
int main(void){
 const char* groups[]={"valid","equations","encodings","workspace","work"};
 register_contract();
 if(n!=sizeof(groups)/sizeof(groups[0])){fputs("wrong case count\n",stderr);return 3;}
 for(unsigned j=0;j<sizeof(groups)/sizeof(groups[0]);j++){
 unsigned i;for(i=0;i<n;i++)if(!strcmp(groups[j],cases[i].name))break;
 if(i==n)return 4;
 unsigned before=checks;cases[i].fn();
 if(checks==before){fputs("zero checks\n",stderr);return 5;}
 printf("PASS %s %u\n",groups[j],checks-before);
 }
 puts("TARGET_COMPLETE");return 0;
}
