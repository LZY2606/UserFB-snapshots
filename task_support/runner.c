#include "checks.h"
int main(void){Check c={0};test_projection(&c);if(c.assertions==0)return 1;printf("PASS projection %d\n",c.assertions);c.assertions=0;test_material(&c);if(!c.assertions)return 1;printf("PASS material %d\n",c.assertions);c.assertions=0;test_pipeline(&c);if(!c.assertions)return 1;printf("PASS pipeline %d\n",c.assertions);return 0;}
