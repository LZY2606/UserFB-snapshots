#include "Ap4.h"
#include "Ap4LinearReader.h"
#include "Ap4SyntheticSampleTable.h"
#include "Ap4TfhdAtom.h"
#include "Ap4TfdtAtom.h"
#include "Ap4TrexAtom.h"
#include "Ap4TrunAtom.h"
#include "Ap4TfraAtom.h"
#include "Ap4MfroAtom.h"
#include "Ap4MfhdAtom.h"
#include <cstdio>
#include <cstring>

#define REQUIRE(x) do { if (!(x)) { std::fprintf(stderr,"FAIL line=%d %s\n",__LINE__,#x); return 1; } } while (0)

int main() {
    AP4_ByteStream* original = NULL;
    REQUIRE(AP4_SUCCEEDED(AP4_FileByteStream::Create("Test/Data/video-h264-001.mp4", AP4_FileByteStream::STREAM_MODE_READ, original)));
    AP4_File original_file(*original, true);
    AP4_Movie* source_movie = original_file.GetMovie();
    REQUIRE(source_movie != NULL);
    AP4_Track* source_track = source_movie->GetTrack(AP4_Track::TYPE_VIDEO);
    REQUIRE(source_track != NULL);
    AP4_Sample source_sample;
    AP4_DataBuffer payload;
    REQUIRE(AP4_SUCCEEDED(source_track->ReadSample(0,source_sample,payload)));
    REQUIRE(source_sample.IsSync() && payload.GetDataSize()>0);

    AP4_Movie movie(1000,40);
    AP4_SyntheticSampleTable* empty = new AP4_SyntheticSampleTable();
    REQUIRE(AP4_SUCCEEDED(empty->AddSampleDescription(source_track->GetSampleDescription(0),false)));
    AP4_Track* track = new AP4_Track(AP4_Track::TYPE_VIDEO,empty,1,1000,40,1000,40,"und",source_track->GetWidth(),source_track->GetHeight());
    REQUIRE(AP4_SUCCEEDED(movie.AddTrack(track)));
    AP4_ContainerAtom* mvex = new AP4_ContainerAtom(AP4_ATOM_TYPE_MVEX);
    mvex->AddChild(new AP4_TrexAtom(1,1,10,payload.GetDataSize(),0));
    movie.GetMoovAtom()->AddChild(mvex);

    AP4_ContainerAtom moof(AP4_ATOM_TYPE_MOOF);
    moof.AddChild(new AP4_MfhdAtom(1));
    AP4_ContainerAtom* traf = new AP4_ContainerAtom(AP4_ATOM_TYPE_TRAF);
    moof.AddChild(traf);
    traf->AddChild(new AP4_TfhdAtom(AP4_TFHD_FLAG_DEFAULT_BASE_IS_MOOF|AP4_TFHD_FLAG_DEFAULT_SAMPLE_DURATION_PRESENT|AP4_TFHD_FLAG_DEFAULT_SAMPLE_SIZE_PRESENT|AP4_TFHD_FLAG_DEFAULT_SAMPLE_FLAGS_PRESENT,1,0,0,10,payload.GetDataSize(),0));
    traf->AddChild(new AP4_TfdtAtom(0,0));
    AP4_TrunAtom* runs[2];
    for (int i=0;i<2;i++) {
        runs[i]=new AP4_TrunAtom(AP4_TRUN_FLAG_DATA_OFFSET_PRESENT,0,0);
        AP4_Array<AP4_TrunAtom::Entry> entries;
        entries.Append(AP4_TrunAtom::Entry());
        entries.Append(AP4_TrunAtom::Entry());
        REQUIRE(AP4_SUCCEEDED(runs[i]->SetEntries(entries)));
        traf->AddChild(runs[i]);
    }
    AP4_UI32 body=(AP4_UI32)moof.GetSize()+8;
    runs[0]->SetDataOffset(body);
    runs[1]->SetDataOffset(body+2*payload.GetDataSize());
    AP4_MemoryByteStream* fragments = new AP4_MemoryByteStream();
    REQUIRE(AP4_SUCCEEDED(moof.Write(*fragments)));
    REQUIRE(AP4_SUCCEEDED(fragments->WriteUI32(8+4*payload.GetDataSize())));
    REQUIRE(AP4_SUCCEEDED(fragments->WriteUI32(AP4_ATOM_TYPE_MDAT)));
    for (int i=0;i<4;i++) REQUIRE(AP4_SUCCEEDED(fragments->Write(payload.GetData(),payload.GetDataSize())));
    AP4_ContainerAtom mfra(AP4_ATOM_TYPE_MFRA);
    AP4_TfraAtom* tfra = new AP4_TfraAtom(1);
    REQUIRE(AP4_SUCCEEDED(tfra->AddEntry(30,0,1,2,2)));
    mfra.AddChild(tfra);
    mfra.AddChild(new AP4_MfroAtom((AP4_UI32)mfra.GetSize()+16));
    REQUIRE(AP4_SUCCEEDED(mfra.Write(*fragments)));
    REQUIRE(AP4_SUCCEEDED(fragments->Seek(0)));
    {
        AP4_LinearReader reader(movie,fragments);
        REQUIRE(AP4_SUCCEEDED(reader.EnableTrack(1)));
        for (int i=0;i<4;i++) {
            AP4_Sample s;
            AP4_DataBuffer data;
            REQUIRE(AP4_SUCCEEDED(reader.ReadNextSample(1,s,data)));
            REQUIRE(s.GetDts()==(AP4_UI64)i*10 && data.GetDataSize()==payload.GetDataSize());
            REQUIRE(std::memcmp(data.GetData(),payload.GetData(),payload.GetDataSize())==0);
        }
        std::puts("BENTO_NORMAL samples=4 payload_match=1 dts=0,10,20,30");
        AP4_UI32 actual=0;
        REQUIRE(AP4_SUCCEEDED(reader.SeekTo(30,&actual)));
        AP4_Sample selected;
        AP4_DataBuffer selected_data;
        REQUIRE(AP4_SUCCEEDED(reader.ReadNextSample(1,selected,selected_data)));
        REQUIRE(selected_data.GetDataSize()==payload.GetDataSize());
        std::printf("BENTO_SEEK tfra_trun=2 tfra_sample=2 observed_dts=%llu expected_dts=30 mismatch=%d actual_ms=%u\n",(unsigned long long)selected.GetDts(),selected.GetDts()!=30,actual);
    }
    fragments->Release();
    original->Release();
    std::puts("BENTO_BASELINE_COMPLETE");
    return 0;
}
