#include "checks.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned assertions;
void task_check(int condition,const char *message) {
    ++assertions;
    if(!condition){fprintf(stderr,"ASSERT %s\n",message);exit(2);}
}
int main(void) {
    const char *groups[]={"numeric","planning","workspace","consumers"};
    for(unsigned i=0;i<4;i++) {
        unsigned before=assertions;
        int done=task_cases(groups[i]);
        if(done!=1 || assertions==before) {
            fprintf(stderr,"INCOMPLETE %s result=%d assertions=%u\n",groups[i],done,assertions-before);return 3;
        }
        printf("GROUP %s assertions=%u\n",groups[i],assertions-before);
    }
    printf("PRIME_TESTS_COMPLETE assertions=%u\n",assertions);return 0;
}
