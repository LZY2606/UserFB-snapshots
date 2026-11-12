#pragma once
#include <cstdio>
#include <cstdlib>
#include <cmath>
extern unsigned uf_assertions;
#define UF_CHECK(x) do { ++uf_assertions; if(!(x)) { std::fprintf(stderr,"ASSERT %s:%d: %s\n",__FILE__,__LINE__,#x); std::exit(2); } } while(0)
