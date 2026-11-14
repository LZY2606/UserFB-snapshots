#include <pe-parse/parse.h>
#include "fixtures.h"
#include <iostream>
#include <string>
int main(int argc,char**argv){
 auto b=fixtures::pe();auto*p=peparse::ParsePEFromPointer(b.data(),b.size());
 if(!p)return 1;peparse::VA ep=0;std::uint8_t byte=0;
 bool ok=peparse::GetEntryPoint(p,ep)&&ep==0x401000&&peparse::ReadByteAtVA(p,ep,byte)&&byte==0xc3;
 peparse::DestructParsedPE(p);if(!ok)return 2;std::cout<<"PE_PRODUCTION_OK entry=401000 byte=c3\n";
 if(argc>1&&std::string(argv[1])=="--before"){
  for(bool wide:{false,true}){auto t=fixtures::te(wide);p=peparse::ParsePEFromPointer(t.data(),t.size());
   if(p){peparse::DestructParsedPE(p);return 3;}std::cout<<"TE"<<(wide?64:32)<<"_CURRENTLY_REJECTED err="<<peparse::GetPEErr()<<"\n";
  }
 }
 return 0;
}
