#include <tidy.h>
#include <tidybuffio.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned walk(TidyDoc d, TidyNode n, int level) {
    unsigned k=0;
    for (;n;n=tidyGetNext(n)) {
        const char *name=tidyNodeGetName(n);
        if (name) { printf("%*s%s\n",level,"",name); ++k; }
        k+=walk(d,tidyGetChild(n),level+1);
    }
    return k;
}
int main(void) {
    TidyDoc d=tidyCreate();
    TidyBuffer err; tidyBufInit(&err); tidySetErrorBuffer(d,&err);
    tidyOptSetBool(d,TidyMark,no);
    tidyOptSetBool(d,TidyDropEmptyElems,no);
    tidyOptSetBool(d,TidyMakeClean,yes);
    const char *html="<!doctype html><html><head><title>x</title></head><body>"
        "<template id='t'><div><meta name='inside' content='y'>"
        "<style>.x{color:red}</style><p style='color:blue'>N</p></div></template>"
        "<p id='after'>B</p></body></html>";
    assert(tidyParseString(d,html)>=0);
    assert(tidyCleanAndRepair(d)>=0);
    assert(tidyGetBody(d)); assert(walk(d,tidyGetRoot(d),0)>5);
    TidyBuffer out; tidyBufInit(&out);
    assert(tidySaveBuffer(d,&out)>=0);
    assert(strstr((char*)out.bp,"id='after'") || strstr((char*)out.bp,"id=\"after\""));
    puts((char*)out.bp); puts("TIDY_BASELINE_OK");
    tidyBufFree(&out); tidyBufFree(&err); tidyRelease(d);
    return 0;
}
