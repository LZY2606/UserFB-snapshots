#include "contract.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct entry { const char *group, *name; contract_case fn; };
static struct entry cases[512];
static int ncases, assertions;
static const char *current_group, *current_name;
void contract_add(const char *group, const char *name, contract_case fn) {
    if (!group || !name || !*name || !fn || ncases == 512) exit(3);
    if (strcmp(group,"paging") && strcmp(group,"timing") && strcmp(group,"streaming")) exit(3);
    for (int i=0;i<ncases;++i)
        if (!strcmp(cases[i].group, group) && !strcmp(cases[i].name, name)) exit(3);
    cases[ncases++] = (struct entry){ group, name, fn };
}
void contract_check(int ok, const char *expr, const char *file, int line) {
    if (!current_group) exit(3);
    ++assertions;
    if (!ok) {
        fprintf(stderr,"FAIL %s/%s %s:%d %s\n",current_group,current_name,file,line,expr);
        exit(1);
    }
}
int main(void) {
    const char *groups[] = {"paging", "timing", "streaming"};
    setvbuf(stdout, NULL, _IONBF, 0);
    register_contract();
    if (!ncases) return 2;
    for (int g=0;g<3;++g) {
        int count=0, before=assertions;
        current_group=groups[g];
        for (int i=0;i<ncases;++i) if (!strcmp(cases[i].group,groups[g])) {
            int old=assertions;
            current_name=cases[i].name;
            cases[i].fn();
            if (assertions == old) return 2;
            ++count;
            printf("CASE %s %s assertions=%d done=1\n",groups[g],current_name,assertions-old);
        }
        if (!count) return 2;
        printf("GROUP %s cases=%d assertions=%d done=1\n",groups[g],count,assertions-before);
    }
    printf("COMPLETE groups=3 cases=%d assertions=%d done=1\n",ncases,assertions);
    return 0;
}
