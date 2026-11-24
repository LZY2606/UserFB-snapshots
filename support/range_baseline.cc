#include "common/dwarf/dwarf2reader.h"
#include <cstdio>
using namespace google_breakpad;
struct Handler : RangeListHandler {
 int count=0; bool done=false; uint64_t low=0,high=0;
 void AddRange(uint64_t b,uint64_t e) override{count++;low=b;high=e;}
 void Finish() override{done=true;}
};
int main(){
 const uint8_t bytes[]={2,0,0,0,6,0,0,0,0,0,0,0,0,0,0,0};
 ByteReader reader(ENDIANNESS_LITTLE);reader.SetAddressSize(4);reader.SetOffsetSize(4);
 RangeListReader::CURangesInfo cu;cu.version_=4;cu.base_address_=0x1000;cu.buffer_=bytes;cu.size_=sizeof(bytes);
 Handler h;RangeListReader ranges(&reader,&cu,&h);
 if(!ranges.ReadRanges(DW_FORM_sec_offset,0) || h.count!=1 || !h.done || h.low!=0x1002 || h.high!=0x1006)return 1;
 printf("BASELINE range=0x1002..0x1006 count=1\n");
}
