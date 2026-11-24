#pragma once
#include "small_fixture.h"
// Input serialization only. No target reader or analysis implementation.
namespace fixture {
inline void analysis_record(Bytes& stream, unsigned kind, const Bytes& payload) {
    u16(stream, static_cast<unsigned>(payload.size()+2)); u16(stream,kind);
    stream.insert(stream.end(),payload.begin(),payload.end());
    while(stream.size()%4) stream.push_back(0);
}
inline Bytes analysis_module(const char* object, unsigned section, unsigned offset, unsigned type) {
    Bytes stream; u32(stream,4);
    Bytes obj;u32(obj,0);obj.insert(obj.end(),object,object+std::strlen(object)+1);
    analysis_record(stream,0x1101,obj); // S_OBJNAME, ends at logical offset 24
    Bytes ns(979,'n');ns.push_back(0);
    analysis_record(stream,0x1124,ns); // S_UNAMESPACE padding, ends at offset 1008
    Bytes data;u32(data,type);u32(data,offset);u16(data,section);
    const char name[]="state";data.insert(data.end(),name,name+sizeof(name));
    analysis_record(stream,0x110d,data); // S_GDATA32: name at 1022 crosses page boundary
    return stream;
}
inline std::vector<Bytes> analysis_streams(bool changed=false) {
    std::vector<Bytes> streams(9);
    streams[6]=analysis_module("alpha.obj",1,changed?0x60:0x40,0x1000);
    streams[7]=analysis_module("beta.obj",2,0x88,0x1001);
    return streams;
}
}
