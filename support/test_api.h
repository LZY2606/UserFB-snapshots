#pragma once
#include <stdio.h>
#include <stdlib.h>
struct suite {int cases,assertions,last;};
static inline void require(struct suite*s,int ok,const char*why){s->assertions++;if(!ok){fprintf(stderr,"FAIL: %s\n",why);exit(1);}}
static inline void case_begin(struct suite*s,const char*name){if(s->cases&&s->assertions==s->last){fprintf(stderr,"empty case\n");exit(1);}s->cases++;s->last=s->assertions;printf("CASE %s\n",name);}
void target_cases(struct suite*);
