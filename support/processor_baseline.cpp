#include "fixture.hpp"
int main(int argc,char** argv){
 bool compat=argc>1&&std::strcmp(argv[1],"compat")==0;
 for(bool split:{false,true}){
  auto plain=seed::processed(seed::movie(false),split);seed::verify_media(plain);
  std::printf("PROCESSOR_UNINDEXED_%s samples=2 transformed=2 OK\n",split?"split":"combined");
  if(!compat){auto bytes=seed::movie(true);auto out=seed::processed(bytes,split);seed::verify_media(out);
   auto before=seed::count(bytes,AP4_ATOM_TYPE_SIDX),after=seed::count(out,AP4_ATOM_TYPE_SIDX);
   seed::require(before==3&&after==(split?3u:0u),"current multi-SIDX behavior");
   if(split){unsigned i=0,old_size=0,actual_size=0;auto boxes=seed::scan(out);for(size_t k=0;k<boxes.size();++k){auto x=boxes[k];if(x.type==AP4_ATOM_TYPE_SIDX&&++i==2)old_size=seed::get(out,x.offset+32)&0x7fffffff;if(x.type==AP4_ATOM_TYPE_MOOF&&!actual_size)actual_size=(unsigned)(x.size+boxes[k+1].size);}
    seed::require(old_size==87&&actual_size==89,"split stale index size witness");
    std::printf("WITNESS_SPLIT_STALE_SIDX indexed_size=%u actual_media_size=%u transformed=2\n",old_size,actual_size);
   }else{std::printf("WITNESS_COMBINED_SIDX_LOSS input=%u output=%u transformed=2\n",before,after);}
  }
 }
 std::puts("BASELINE_PROCESSOR_COMPLETED");return 0;
}
