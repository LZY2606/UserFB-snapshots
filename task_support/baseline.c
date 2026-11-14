#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "memwatch.h"
#define REQUIRE(x) do {if (!(x)) {fprintf(stderr,"BASELINE_DIFFERENCE line=%d\n",__LINE__); return 17;}} while(0)
int main(void) {
    unsigned char *p, *z, *grown;
    size_t i;
    mwInit();
    p = mwMalloc(19, "benign-allocation", 10);
    z = mwCalloc(4, 9, "benign-zero", 20);
    REQUIRE(p && z);
    for (i=0; i<19; ++i) p[i]=(unsigned char)(i+1);
    for (i=0; i<36; ++i) REQUIRE(z[i]==0);
    grown = mwRealloc(p, 73, "benign-grow", 30);
    REQUIRE(grown);
    for (i=0; i<19; ++i) REQUIRE(grown[i]==i+1);
    memset(grown+19, 0, 54);
    REQUIRE(mwTestBuffer("benign-check", 40, grown)==0);
    p = mwRealloc(grown, 11, "benign-shrink", 50);
    REQUIRE(p);
    for (i=0; i<11; ++i) REQUIRE(p[i]==i+1);
    REQUIRE(mwTest("benign-scan", 60, MW_TEST_ALL)==0);
    mwFree(p, "benign-free", 70);
    mwNoMansLand(MW_NML_FREE);
    mwFree(z, "benign-retain", 80);
    REQUIRE(mwTest("benign-nml", 90, MW_TEST_ALL)==0);
    mwTerm();
    puts("MEMWATCH_BASELINE_OK realloc=2 zeroed=36 retained=1");
    return 0;
}
