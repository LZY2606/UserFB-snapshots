#include "cases.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
void target_check(int passed, const char *expr, int line) {
    ++checks;
    if (!passed) {
        fprintf(stderr, "CHECK_FAILED line=%d expr=%s\n", line, expr);
        exit(1);
    }
}
int main(void) {
    const char *groups[] = {"optimizer", "stream", "container"};
    size_t i, j, g, total = 0;
    if (target_count < 6 || target_count > 1000) return 2;
    for (i = 0; i < target_count; ++i) {
        const TargetCase *c = &target_cases[i];
        if (!c->name || !*c->name || !c->group || !c->run) return 3;
        for (j = 0; j < i; ++j)
            if (!strcmp(c->name, target_cases[j].name)) return 4;
        for (g = 0; g < 3 && strcmp(c->group, groups[g]); ++g) {}
        if (g == 3) return 5;
    }
    for (g = 0; g < 3; ++g) {
        size_t cases = 0;
        unsigned before = checks;
        for (i = 0; i < target_count; ++i) {
            if (strcmp(target_cases[i].group, groups[g])) continue;
            unsigned one = checks;
            target_cases[i].run();
            if (checks == one) return 6;
            printf("CASE_PASS %s %s checks=%u\n", groups[g], target_cases[i].name, checks-one);
            ++cases;
            ++total;
        }
        if (cases < 2) return 7;
        printf("GROUP_DONE %s cases=%zu checks=%u\n", groups[g], cases, checks-before);
    }
    printf("TARGET_DONE cases=%zu checks=%u\n", total, checks);
    return 0;
}
