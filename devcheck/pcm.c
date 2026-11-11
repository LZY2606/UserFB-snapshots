#include <stdint.h>
#include <stdio.h>
int main(void) {
 for(int i=0;i<160*12;i++) { int16_t s=(int16_t)(((i*97)%10000)-5000);if(fwrite(&s,sizeof(s),1,stdout)!=1)return 1; }
 return 0;
}
