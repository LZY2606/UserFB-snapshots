/* Author-generated legal Thumb code; analyzed as bytes, never executed. */
#include <capstone/capstone.h>
#include <stdio.h>
int main(void){
 const unsigned char code[]={0x0c,0xbf,0x01,0x20,0x02,0x21,0x03,0x22};
 csh h=0;cs_insn *a=0;size_t n;
 if(cs_open(CS_ARCH_ARM,CS_MODE_THUMB,&h)!=CS_ERR_OK)return 1;
 if(cs_option(h,CS_OPT_DETAIL,CS_OPT_ON)!=CS_ERR_OK)return 2;
 n=cs_disasm(h,code,sizeof(code),0x1000,0,&a);
 if(n!=4||a[0].id!=ARM_INS_IT||a[1].detail->arm.cc!=ARM_CC_EQ||a[2].detail->arm.cc!=ARM_CC_NE||a[3].detail->arm.cc!=ARM_CC_AL||a[1].detail->arm.update_flags||a[2].detail->arm.update_flags||!a[3].detail->arm.update_flags)return 3;
 printf("BASELINE_OK count=%zu IT=0x%llx cc=%u,%u,%u flags=%u,%u,%u\n",n,(unsigned long long)a[0].address,(unsigned)a[1].detail->arm.cc,(unsigned)a[2].detail->arm.cc,(unsigned)a[3].detail->arm.cc,a[1].detail->arm.update_flags,a[2].detail->arm.update_flags,a[3].detail->arm.update_flags);
 cs_free(a,n);cs_close(&h);return 0;
}
