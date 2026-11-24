#include <pe-parse/parse.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
int main(){
 for(const auto *name:{"samples/cedar32.pe","samples/cedar64.pe"}){
  std::ifstream f(name,std::ios::binary);if(!f)return 10;
  std::vector<std::uint8_t>b((std::istreambuf_iterator<char>(f)),{});
  if(b.empty())return 11;
  auto p=peparse::ParsePEFromPointer(b.data(),b.size());unsigned count=0;
  if(p){peparse::IterDebugs(p,[](void*c,const std::uint32_t&,const peparse::bounded_buffer*){++*static_cast<unsigned*>(c);return 0;},&count);peparse::DestructParsedPE(p);}
  std::cout<<name<<" current_parse="<<(p?"accepted":"rejected")<<" debug_records="<<count<<"\n";
  if(std::string(name).find("32")!=std::string::npos){if(!p||count!=0)return 12;}else if(p)return 13;
 }
 std::cout<<"PE_DEBUG_FIXED_INPUT_BASELINE_OK\n";
}
