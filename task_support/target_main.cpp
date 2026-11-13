#include <cstdio>
int test_aac_au_headers();int test_aac_au_timing();int test_aac_fragment_timing();
int main(){int a=test_aac_au_headers(),b=test_aac_au_timing(),c=test_aac_fragment_timing();printf("AAC_TARGET headers=%d timing=%d fragments=%d\n",a,b,c);return a>0&&b>0&&c>0?0:1;}
