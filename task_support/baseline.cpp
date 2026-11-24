#include "PDB_PCH.h"
#include "PDB.h"
#include "PDB_RawFile.h"
#include "PDB_DirectMSFStream.h"
#include "PDB_CoalescedMSFStream.h"
#include "PDB_ModuleSymbolStream.h"
#include "small_fixture.h"
#include <cstdio>
#include <utility>
#define REQUIRE(x) do {if(!(x)){std::fprintf(stderr,"baseline line=%d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(){
    std::vector<fixture::Bytes> streams(9);
    // C13 module-symbol signature, a standalone object-name record, then alignment.
    fixture::u32(streams[6],4);fixture::u16(streams[6],13);fixture::u16(streams[6],0x1101);fixture::u32(streams[6],0);
    const char name[]="module";streams[6].insert(streams[6].end(),name,name+sizeof(name));streams[6].push_back(0);
    streams[8].resize(1100);for(size_t i=0;i<1100;++i)streams[8][i]=static_cast<unsigned char>((i*37+19)&255);
    auto image=fixture::msf(streams);
    REQUIRE(PDB::ValidateFile(image.data(),image.size())==PDB::ErrorCode::Success);
    auto original=PDB::CreateRawFile(image.data());auto raw=std::move(original);
    REQUIRE(raw.GetStreamCount()==9);
    auto direct=raw.CreateMSFStream<PDB::DirectMSFStream>(8);
    fixture::Bytes read(1030);direct.ReadAtOffset(read.data(),read.size(),29);
    REQUIRE(std::equal(read.begin(),read.end(),streams[8].begin()+29));
    PDB::CoalescedMSFStream window(direct,1030,29);
    REQUIRE(std::memcmp(window.GetDataAtOffset<unsigned char>(0),read.data(),read.size())==0);
    PDB::ModuleSymbolStream symbols(raw,6,static_cast<uint32_t>(streams[6].size()));
    unsigned count=0;symbols.ForEachSymbol([&](const PDB::CodeView::DBI::Record* r){if(r->header.kind==PDB::CodeView::DBI::SymbolRecordKind::S_OBJNAME)++count;});
    REQUIRE(count==1);REQUIRE(symbols.FindRecord(PDB::CodeView::DBI::SymbolRecordKind::S_OBJNAME)!=nullptr);
    std::printf("BASELINE_OK streams=9 scatter=1030 module_symbols=%u\n",count);
}
