#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "case.h"
static const char *groups[]={"mapping","collapse","reparent","quotes"};
static unsigned counts[4];
void contract_case(const char *g){ for(unsigned i=0;i<sizeof counts/sizeof *counts;i++) if(!strcmp(g,groups[i])){counts[i]++;return;} abort();}
extern void text_anchor_contract(void);
int main(void){text_anchor_contract();unsigned n=0;for(unsigned i=0;i<sizeof counts/sizeof *counts;i++){if(!counts[i]){fprintf(stderr,"missing group %s\n",groups[i]);return 1;}n+=counts[i];}printf("TARGET_CASES_OK count=%u\n",n);return 0;}
