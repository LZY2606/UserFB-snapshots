#include <aribcaption/context.hpp>
#include <aribcaption/decoder.hpp>
#include <aribcaption/caption.hpp>
#include <vector>
#include <cstdio>
#include "sample_data.h"
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(){aribcaption::Context ctx;aribcaption::Decoder d(ctx);CHECK(d.Initialize());aribcaption::DecodeResult r;
 CHECK(d.Decode(sample_data_1,sizeof(sample_data_1),1000,r)==aribcaption::DecodeStatus::kGotCaption);CHECK(r.caption&&!r.caption->text.empty());
 std::printf("baseline upstream-text=%s regions=%zu\n",r.caption->text.c_str(),r.caption->regions.size());
 d.Flush();std::vector<unsigned char> body={0x0c,0x0e,0x41,0x9d,0x20,0x45,0x42,0x9d,0x20,0x43};
 std::vector<unsigned char> data={0x80,0xff,0xf0,0x04,0,0,0,0};unsigned group=4+5+body.size();data[6]=group>>8;data[7]=group;data.insert(data.end(),{0,0,0,(unsigned char)(5+body.size()),0x1f,0x20,0,0,(unsigned char)body.size()});data.insert(data.end(),body.begin(),body.end());
 CHECK(d.Decode(data.data(),data.size(),1000,r)==aribcaption::DecodeStatus::kGotCaption);CHECK(r.caption->wait_duration==800);
 std::printf("baseline aggregate-text=%s pts=%lld wait=%lld\n",r.caption->text.c_str(),(long long)r.caption->pts,(long long)r.caption->wait_duration);return 0;}
