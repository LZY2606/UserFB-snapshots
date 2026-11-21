#pragma once
#include <cstdio>
#include <cerrno>
#ifndef _MSC_VER
inline int fopen_s(FILE** out,const char* name,const char* mode){
 if(!out)return EINVAL;
 *out=std::fopen(name,mode);
 return *out?0:errno;
}
#endif
