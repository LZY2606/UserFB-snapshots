#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <exception>
struct FBCase {const char *group,*name;void(*fn)();};
static std::vector<FBCase>& fb_cases(){static std::vector<FBCase> v;return v;}
static unsigned fb_checks;
struct FBReg { FBReg(const char*g,const char*n,void(*f)()){fb_cases().push_back({g,n,f});}};
#define FB_TEST(G,N) static void N(); static FBReg reg_##N(G,#N,N); static void N()
#define FB_CHECK(X) do {++fb_checks; if(!(X)){std::fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#X);std::exit(1);}} while(0)
