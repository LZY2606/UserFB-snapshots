#ifndef AUTHOR_LABEL_FIXTURE_H
#define AUTHOR_LABEL_FIXTURE_H
#include <cstdint>
#include <vector>
#include <string_view>
namespace fixture {
using Bytes=std::vector<uint8_t>;
inline void u32(Bytes& b,uint32_t v){do{uint8_t c=v&127;v>>=7;b.push_back(c|(v?128:0));}while(v);}
inline void str(Bytes& b,std::string_view s){u32(b,s.size());b.insert(b.end(),s.begin(),s.end());}
inline void section(Bytes& b,uint8_t id,const Bytes& payload){b.push_back(id);u32(b,payload.size());b.insert(b.end(),payload.begin(),payload.end());}
// Valid core module with one block and one loop; label-name payload uses independent literal indices.
inline Bytes sample(bool names=true,bool early=false){
 Bytes b={0,97,115,109,1,0,0,0};section(b,1,{1,0x60,0,0});section(b,3,{1,0});
 Bytes name;str(name,"name");Bytes labels={1,0,2,0};str(labels,"outer");u32(labels,1);str(labels,"inner");section(name,3,labels);
 if(names&&early)section(b,0,name);
 section(b,10,{1,10,0,0x02,0x40,0x03,0x40,0x0c,1,0x0b,0x0b,0x0b});
 if(names&&!early)section(b,0,name);return b;
}
}
#endif
