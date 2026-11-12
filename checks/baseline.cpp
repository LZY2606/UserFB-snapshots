#include <pe-parse/parse.h>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace peparse;
static void put(std::vector<uint8_t>&b,size_t o,uint64_t x,unsigned n){for(unsigned i=0;i<n;i++)b[o+i]=(x>>(8*i))&255;}
static int sec(void*p,const VA&,const std::string&,const image_section_header&,const bounded_buffer*){++*static_cast<int*>(p);return 0;}
int main(){
 std::vector<uint8_t>b(0x420,0);put(b,0,0x5a4d,2);put(b,0x3c,0x80,4);put(b,0x80,0x4550,4);
 put(b,0x84,0x14c,2);put(b,0x86,1,2);put(b,0x94,224,2);put(b,0x96,0x102,2);
 size_t o=0x98;put(b,o,0x10b,2);put(b,o+16,0x1000,4);put(b,o+28,0x400000,4);
 put(b,o+32,0x1000,4);put(b,o+36,0x200,4);put(b,o+56,0x2000,4);put(b,o+60,0x200,4);put(b,o+68,3,2);put(b,o+92,16,4);
 put(b,o+96+4*8,0x400,4);put(b,o+96+4*8+4,32,4);
 size_t s=o+224;put(b,s,0x617461642e,5);put(b,s+8,0x200,4);put(b,s+12,0x1000,4);put(b,s+16,0x200,4);put(b,s+20,0x200,4);put(b,s+36,0x40000040,4);
 for(size_t i=0x200;i<0x400;i++)b[i]=uint8_t(i);
 put(b,0x400,12,4);put(b,0x404,0x200,2);put(b,0x406,2,2);put(b,0x408,0x64636261,4);
 put(b,0x410,16,4);put(b,0x414,0x200,2);put(b,0x416,0x9999,2);
 parsed_pe*p=ParsePEFromPointer(b.data(),b.size());if(!p)throw std::runtime_error(GetPEErrString());
 int sections=0;IterSec(p,sec,&sections);VA entry=0;std::vector<uint8_t>cert;
 if(!GetEntryPoint(p,entry)||!GetDataDirectoryEntry(p,DIR_SECURITY,cert))throw std::runtime_error(GetPEErrString());
 if(sections!=1||entry!=0x401000||cert!=std::vector<uint8_t>(b.begin()+0x400,b.end()))throw std::runtime_error("readback");
 DestructParsedPE(p);std::cout<<"PE_BASELINE_OK cert_bytes="<<cert.size()<<" sections="<<sections<<" entry=0x"<<std::hex<<entry<<"\n";
}
