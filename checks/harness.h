#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
extern unsigned uf_assertions;
#define UF_CHECK(x) do { ++uf_assertions; if(!(x)) { fprintf(stderr,"ASSERT %s:%d: %s\n",__FILE__,__LINE__,#x); exit(2); } } while(0)
