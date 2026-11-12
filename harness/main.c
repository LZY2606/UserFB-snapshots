#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "case.h"
static const char *groups[]={"prefix-raw","pointer-keys","document-reuse","linear-work"};
static unsigned counts[4];
void contract_case(const char *g){ for(unsigned i=0;i<sizeof counts/sizeof *counts;i++) if(!strcmp(g,groups[i])){counts[i]++;return;} abort();}
extern void batch_pointer_contract(void);
int main(void){batch_pointer_contract();unsigned n=0;for(unsigned i=0;i<sizeof counts/sizeof *counts;i++){if(!counts[i]){fprintf(stderr,"missing group %s\n",groups[i]);return 1;}n+=counts[i];}printf("TARGET_CASES_OK count=%u\n",n);return 0;}
