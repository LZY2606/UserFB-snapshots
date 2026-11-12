#include "check.h"
unsigned long unit_checks=0,unit_failures=0;
void reverse_branches(void);
void reverse_singular(void);
void reverse_index(void);
static int run(const char *name, void (*fn)(void)) {
    unsigned long before=unit_checks, fails=unit_failures;
    fn();
    if (unit_checks==before || unit_failures!=fails) {
        fprintf(stderr,"UNIT_GROUP_FAIL %s checks=%lu failures=%lu\n",name,unit_checks-before,unit_failures-fails);
        return 0;
    }
    printf("UNIT_GROUP_PASS %s checks=%lu\n",name,unit_checks-before);
    return 1;
}
int main(void) {
    if (!run("branches",reverse_branches)) return 1;
    if (!run("singular",reverse_singular)) return 1;
    if (!run("index",reverse_index)) return 1;
    puts("UNIT_TARGET_COMPLETE groups=3");
    return 0;
}
