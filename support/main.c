#include "test_api.h"
int main(void){struct suite s={0};target_cases(&s);if(s.cases<4||s.assertions<8||s.assertions==s.last){fprintf(stderr,"need four nonempty cases and eight assertions\n");return 2;}printf("TARGET_OK cases=%d assertions=%d\n",s.cases,s.assertions);return 0;}
