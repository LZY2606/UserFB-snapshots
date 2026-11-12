#pragma once
#include "Ap4.h"
#include "Ap4Processor.h"
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace seed {
using Bytes=std::vector<AP4_UI08>;
inline void require(bool ok,const char* msg) { if(!ok){std::fprintf(stderr,"SEED_ASSERT: %s\n",msg);std::exit(2);} }
inline void u32(Bytes& b,AP4_UI32 x){for(int i=3;i>=0;--i)b.push_back((AP4_UI08)(x>>(8*i)));}
inline AP4_UI32 get(const Bytes& b,size_t p){require(p+4<=b.size(),"u32 bounds");return (AP4_UI32(b[p])<<24)|(AP4_UI32(b[p+1])<<16)|(AP4_UI32(b[p+2])<<8)|b[p+3];}
inline void append(Bytes& a,const Bytes& b){a.insert(a.end(),b.begin(),b.end());}
inline Bytes box(const char* type,Bytes body={}){Bytes b;u32(b,(AP4_UI32)body.size()+8);b.insert(b.end(),type,type+4);append(b,body);return b;}
inline Bytes words(std::initializer_list<AP4_UI32> a){Bytes b;for(auto x:a)u32(b,x);return b;}
inline Bytes fragment(unsigned seq,Bytes data){
 Bytes traf=box("tfhd",words({0x020000,1}));
 // moof size: 8 + mfhd(16) + traf(8+tfhd(16)+trun(28)) = 76
 append(traf,box("trun",words({0x00000301,1,84,100,(AP4_UI32)data.size()})));
 Bytes moof=box("mfhd",words({0,seq}));append(moof,box("traf",traf));
 Bytes result=box("moof",moof);append(result,box("mdat",data));return result;
}
inline Bytes index(unsigned time,unsigned first,std::initializer_list<AP4_UI32> sizes,bool indirect){
 Bytes b=words({0,1,1000,time,first,(AP4_UI32)sizes.size()});
 for(auto n:sizes){u32(b,n|(indirect?0x80000000u:0));u32(b,100);u32(b,0x90000000u);}
 return box("sidx",b);
}
inline Bytes movie(bool indexed){
 Bytes a=fragment(1,{'A','B','C'}),b=fragment(2,{'D','E','F','G','H'});
 Bytes result=box("moov");
 if(indexed){
  // Two child indexes occupy 44 bytes each. Their media live beyond both boxes.
  Bytes root=words({0,1,1000,0,0,2});
  for(unsigned i=0;i<2;++i){u32(root,0x80000000u|44);u32(root,100);u32(root,0x90000000u);}
  append(result,box("sidx",root));
  append(result,index(0,44,{(AP4_UI32)a.size()},false));
  append(result,index(100,(unsigned)a.size(),{(AP4_UI32)b.size()},false));
 }
 append(result,a);append(result,b);return result;
}
struct Box {size_t offset,size;AP4_UI32 type;};
inline std::vector<Box> scan(const Bytes& b){std::vector<Box> v;size_t p=0;while(p<b.size()){auto n=get(b,p);require(n>=8&&n<=b.size()-p,"box bounds");v.push_back({p,n,get(b,p+4)});p+=n;}return v;}
inline Bytes processed(const Bytes& bytes,bool split){
 auto in=new AP4_MemoryByteStream(bytes.data(),(AP4_Size)bytes.size());
 auto out=new AP4_MemoryByteStream();
 class Handler:public AP4_Processor::FragmentHandler {
  AP4_Result ProcessSample(AP4_DataBuffer& in,AP4_DataBuffer& out) override {
   auto n=in.GetDataSize();auto r=out.SetDataSize(n+2);if(AP4_FAILED(r))return r;
   std::memcpy(out.UseData(),in.GetData(),n);out.UseData()[n]=0xAA;out.UseData()[n+1]=0xBB;return AP4_SUCCESS;
  }
 };
 class Processor:public AP4_Processor {
  FragmentHandler* CreateFragmentHandler(AP4_TrakAtom*,AP4_TrexAtom*,AP4_ContainerAtom*,AP4_ByteStream&,AP4_Position) override {return new Handler();}
 } processor;
 AP4_Result r;
 if(split){Bytes init=box("moov");auto init_stream=new AP4_MemoryByteStream(init.data(),(AP4_Size)init.size());in->Seek(8); // fragment stream has its own byte origin
  Bytes fragment_bytes(bytes.begin()+8,bytes.end());auto frags=new AP4_MemoryByteStream(fragment_bytes.data(),(AP4_Size)fragment_bytes.size());
  r=processor.Process(*frags,*out,*init_stream);frags->Release();init_stream->Release();
 }else{r=processor.Process(*in,*out);}
 require(AP4_SUCCEEDED(r),"Processor real sample path");
 Bytes result(out->GetData(),out->GetData()+out->GetDataSize());in->Release();out->Release();return result;
}
inline unsigned count(const Bytes& b,AP4_UI32 type){unsigned n=0;for(auto x:scan(b))if(x.type==type)++n;return n;}
inline void verify_media(const Bytes& b){
 unsigned n=0;for(auto x:scan(b))if(x.type==AP4_ATOM_TYPE_MDAT){Bytes expected=n?Bytes{'D','E','F','G','H',0xAA,0xBB}:Bytes{'A','B','C',0xAA,0xBB};
 require(n<2&&x.size==expected.size()+8,"processed mdat size");
 require(std::memcmp(b.data()+x.offset+8,expected.data(),expected.size())==0,"handler output bytes");++n;}
 require(n==2&&count(b,AP4_ATOM_TYPE_MOOF)==2,"both fragments executed");
}
}
