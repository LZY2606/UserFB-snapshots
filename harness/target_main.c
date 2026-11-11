/* Test dispatcher only; it contains no atlas implementation. zlib license. */
#include <stdio.h>
#include <string.h>
#include "case.h"
int main(void) {
    const char *required[]={"layout_without_residency","page_order_and_pixels","cache_and_iterators","capacity_and_create_failure","gl2_page_lifetime"};
    size_t i,j,executed=0;
    if(paged_case_count==0 || paged_case_count>128){fputs("invalid case count\n",stderr);return 2;}
    for(i=0;i<paged_case_count;i++) {
        if(!paged_cases[i].name || !paged_cases[i].run){fputs("invalid case entry\n",stderr);return 2;}
        for(j=0;j<i;j++)if(!strcmp(paged_cases[i].name,paged_cases[j].name)){fputs("duplicate case\n",stderr);return 2;}
    }
    for(i=0;i<sizeof required/sizeof required[0];i++) {
        for(j=0;j<paged_case_count;j++)if(!strcmp(required[i],paged_cases[j].name))break;
        if(j==paged_case_count){fprintf(stderr,"missing case: %s\n",required[i]);return 2;}
    }
    for(i=0;i<paged_case_count;i++) {
        if(paged_cases[i].run()!=0){fprintf(stderr,"FAIL %s\n",paged_cases[i].name);return 1;}
        printf("PASS %s\n",paged_cases[i].name);executed++;
    }
    printf("executed=%zu\nFONTSTASH_PAGED_OK\n",executed);return 0;
}
