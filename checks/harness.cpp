#include <stdio.h>
#include <string.h>
extern int ipv6_pseudo_case(const char *, unsigned *);
int main(void){
const char *groups[]={"routing","home","combined","malformed","roundtrip"};
for(unsigned i=0;i<sizeof(groups)/sizeof(groups[0]);i++){unsigned checks=0;int status=ipv6_pseudo_case(groups[i],&checks);if(status||checks==0){fprintf(stderr,"FAIL %s status=%d checks=%u\n",groups[i],status,checks);return 1;}printf("GROUP_PASS %s checks=%u\n",groups[i],checks);}
return 0;
}
