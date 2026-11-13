#ifndef CHECK_CHECK_H
#define CHECK_CHECK_H
#include <stdio.h>
#include <stdlib.h>
typedef struct { unsigned checks; } unit_context;
typedef struct { const char *group; const char *name; void (*run)(unit_context *); } unit_case;
#define CHECK(c,e) do { (c)->checks++; if(!(e)) { fprintf(stderr,"CHECK failed %s:%d: %s\n",__FILE__,__LINE__,#e); exit(1); } } while(0)
extern const unit_case unit_cases[];
extern const unsigned unit_case_count;
#endif
