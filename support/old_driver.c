#define main upstream_testbed_main
#include "../testbed/testcms2.c"
#undef main
int main(void) {
    cmsPlugin(&DebugMemHandler);
    cmsSetLogErrorHandler(FatalErrorQuit);
#define RUN(fn) do { if (!(fn())) { fprintf(stderr,"OLD_FAIL %s\n",#fn); return 1;} printf("OLD_PASS %s\n",#fn); } while(0)
    RUN(Check3DinterpolationFloatTetrahedral);
    RUN(Check3DinterpolationFloatTrilinear);
    RUN(CheckReverseInterpolation3x3);
    RUN(CheckReverseInterpolation4x3);
    RUN(Check3DinterpGranular);
    cmsUnregisterPlugins();
    puts("LCMS_OLD_SELECTED_5_OK");
    return 0;
}
