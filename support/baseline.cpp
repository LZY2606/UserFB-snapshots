#include "Ap4.h"
#include <cstdio>
#define CHECK(x) do{if(!(x)){std::fprintf(stderr,"failed %d\n",__LINE__);return 1;}}while(0)
int main(){
 const unsigned char text[]={0,5,'h','e','l','l','o'};
 auto* stream=new AP4_MemoryByteStream(text,sizeof(text));auto* table=new AP4_SyntheticSampleTable();
 CHECK(AP4_SUCCEEDED(table->AddSampleDescription(new AP4_SampleDescription(AP4_SampleDescription::TYPE_UNKNOWN,AP4_SAMPLE_FORMAT_TX3G,0))));
 CHECK(AP4_SUCCEEDED(table->AddSample(*stream,0,sizeof(text),2000,0,0,0,true)));
 AP4_Track track(AP4_Track::TYPE_SUBTITLES,table,1,1000,2000,1000,2000,"eng",0,0);
 AP4_Sample sample;AP4_DataBuffer data;CHECK(AP4_SUCCEEDED(track.ReadSample(0,sample,data)));CHECK(data.GetDataSize()==7);
 AP4_String codec;track.GetSampleDescription(0)->GetCodecString(codec);
 std::printf("baseline codec=%s raw-text=%.*s duration=%u bytes=%u\n",codec.GetChars(),5,data.GetData()+2,sample.GetDuration(),data.GetDataSize());
 stream->Release();return 0;
}
