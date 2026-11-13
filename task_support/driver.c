#include "cases.h"
#include <string.h>
unsigned long target_checks;
int main(void){
 const char *groups[]={"normal","boundary","integration"};
 if(target_count<6||target_count>10000)return 3;
 for(size_t i=0;i<target_count;i++){
  int found=0;for(int g=0;g<3;g++)if(!strcmp(target_cases[i].group,groups[g]))found=1;
  if(!found||!target_cases[i].name||!target_cases[i].run)return 3;
  for(size_t j=0;j<i;j++)if(!strcmp(target_cases[i].name,target_cases[j].name))return 3;
 }
 for(int g=0;g<3;g++){
  unsigned long checks=0;size_t count=0;
  for(size_t i=0;i<target_count;i++)if(!strcmp(target_cases[i].group,groups[g])){
   unsigned long before=target_checks;target_cases[i].run();
   if(target_checks==before)return 4;
   checks+=target_checks-before;count++;
   printf("CASE %s %s CHECKS %lu\n",groups[g],target_cases[i].name,target_checks-before);
  }
  if(!count||!checks)return 5;
  printf("GROUP %s CASES %zu CHECKS %lu COMPLETE\n",groups[g],count,checks);fflush(stdout);
 }
 puts("TARGET_ALL_COMPLETE");return 0;
}
