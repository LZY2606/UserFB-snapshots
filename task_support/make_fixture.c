#include <stdio.h>
int main(void){const unsigned char code[]={0xc2,0x20,0xa9,0x34,0x12,0xe2,0x20,0xa9,0x56,0x60};FILE*f=fopen(".task-build/state.bin","wb");if(!f)return 1;int bad=fwrite(code,1,sizeof(code),f)!=sizeof(code);return fclose(f)||bad;}
