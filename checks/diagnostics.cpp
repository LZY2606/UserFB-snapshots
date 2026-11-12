#include "pbrt.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
void Error(const char* f,...){va_list v;va_start(v,f);vfprintf(stderr,f,v);va_end(v);}
void Warning(const char* f,...){va_list v;va_start(v,f);vfprintf(stderr,f,v);va_end(v);}
void Severe(const char* f,...){va_list v;va_start(v,f);vfprintf(stderr,f,v);va_end(v);abort();}
