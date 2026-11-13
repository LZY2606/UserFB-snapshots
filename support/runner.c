#include "check.h"
#include <string.h>
size_t unit_assertions=0;
int main(void){
 size_t n=0; const unit_case *c=unit_cases(&n); size_t g[3]={0,0,0};
 const char *groups[]={"normal","boundary","integration"};
 if(!c||n<6||n>10000){fprintf(stderr,"invalid case count %zu\n",n);return 2;}
 for(size_t i=0;i<n;i++){
  if(!c[i].name||!*c[i].name||!c[i].group||!c[i].run)return 2;
  for(size_t j=0;j<i;j++)if(!strcmp(c[i].name,c[j].name))return 2;
  int k; for(k=0;k<3;k++)if(!strcmp(c[i].group,groups[k]))break;
  if(k==3)return 2; g[k]++;
 }
 for(int k=0;k<3;k++)if(!g[k])return 2;
 for(size_t i=0;i<n;i++){
  printf("RUN %s/%s\n",c[i].group,c[i].name);fflush(stdout);
  size_t before=unit_assertions; int r=c[i].run();if(unit_assertions==before){fprintf(stderr,"NO_ASSERTIONS %s\n",c[i].name);return 2;}if(r){fprintf(stderr,"FAIL %s code=%d\n",c[i].name,r);return 1;}
  printf("PASS %s/%s\n",c[i].group,c[i].name);
 }
 printf("TARGET COMPLETE cases=%zu groups=3\n",n);return 0;
}
