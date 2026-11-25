#include "Ap4.h"
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
#define CHECK(x) do{if(!(x)){std::cerr<<"BASELINE_FAIL line="<<__LINE__<<"\n";return 1;}}while(0)
static int create(const char* path){
 const std::string payloads[]={"<tt>A</tt>","<tt>BB</tt>","<tt>CCC</tt>","<tt>D</tt>"};
 std::string bytes;for(const auto& x:payloads)bytes+=x;
 auto* stream=new AP4_MemoryByteStream(reinterpret_cast<const AP4_UI08*>(bytes.data()),static_cast<AP4_Size>(bytes.size()));
 auto* table=new AP4_SyntheticSampleTable();
 CHECK(AP4_SUCCEEDED(table->AddSampleDescription(new AP4_SubtitleSampleDescription(AP4_SAMPLE_FORMAT_STPP,"urn:ttml:A","schema-A",""),true)));
 CHECK(AP4_SUCCEEDED(table->AddSampleDescription(new AP4_SubtitleSampleDescription(AP4_SAMPLE_FORMAT_STPP,"urn:ttml:B","schema-B",""),true)));
 const unsigned descriptions[]={0,0,1,0};AP4_Position offset=0;
 for(unsigned i=0;i<4;++i){CHECK(AP4_SUCCEEDED(table->AddSample(*stream,offset,payloads[i].size(),1000,descriptions[i],1000*i,0,true)));offset+=payloads[i].size();}
 auto* movie=new AP4_Movie(1000,4000,0,0);
 CHECK(AP4_SUCCEEDED(movie->AddTrack(new AP4_Track(AP4_Track::TYPE_SUBTITLES,table,7,1000,4000,1000,4000,"eng",0,0,0,0))));
 AP4_File file(movie);AP4_UI32 brands[]={AP4_FILE_BRAND_ISOM};CHECK(AP4_SUCCEEDED(file.SetFileType(AP4_FILE_BRAND_ISOM,0,brands,1)));
 AP4_ByteStream* output=nullptr;CHECK(AP4_SUCCEEDED(AP4_FileByteStream::Create(path,AP4_FileByteStream::STREAM_MODE_WRITE,output)));
 AP4_Result result=AP4_FileWriter::Write(file,*output);output->Release();stream->Release();CHECK(AP4_SUCCEEDED(result));
 std::cout<<"BENTO_FIXTURE_WRITTEN samples=4 descriptions=2\n";return 0;
}
static int inspect(const char* path,bool fragments){
 AP4_ByteStream* input=nullptr;CHECK(AP4_SUCCEEDED(AP4_FileByteStream::Create(path,AP4_FileByteStream::STREAM_MODE_READ,input)));
 {AP4_File file(*input,true);CHECK(file.GetMovie());auto* track=file.GetMovie()->GetTrack(static_cast<AP4_UI32>(7));CHECK(track);CHECK(track->GetSampleDescriptionCount()==2);
 const unsigned expected_input[]={0,0,1,0};std::vector<unsigned> observed;
 if(fragments){
  CHECK(file.GetMovie()->HasFragments());AP4_LinearReader reader(*file.GetMovie(),input);CHECK(AP4_SUCCEEDED(reader.EnableTrack(7)));
  for(unsigned i=0;i<4;++i){AP4_Sample sample;AP4_DataBuffer data;CHECK(AP4_SUCCEEDED(reader.ReadNextSample(7,sample,data)));CHECK(sample.GetDts()==i*1000);CHECK(sample.GetDuration()==1000);CHECK(sample.GetDescriptionIndex()==0);observed.push_back(sample.GetDescriptionIndex());}
  AP4_Sample sample;AP4_DataBuffer data;CHECK(reader.ReadNextSample(7,sample,data)==AP4_ERROR_EOS);
 }else{
  CHECK(track->GetSampleCount()==4);for(unsigned i=0;i<4;++i){AP4_Sample sample;AP4_DataBuffer data;CHECK(AP4_SUCCEEDED(track->ReadSample(i,sample,data)));CHECK(sample.GetDescriptionIndex()==expected_input[i]);observed.push_back(sample.GetDescriptionIndex());}
 }
 std::cout<<(fragments?"BENTO_DESCRIPTION_BASELINE_OK output=":"BENTO_INPUT_OK input=");for(unsigned i=0;i<observed.size();++i)std::cout<<(i?",":"")<<observed[i];std::cout<<" samples="<<observed.size()<<"\n";
 }input->Release();return 0;
}
int main(int argc,char**argv){CHECK(argc==3);if(std::strcmp(argv[1],"create")==0)return create(argv[2]);if(std::strcmp(argv[1],"input")==0)return inspect(argv[2],false);CHECK(std::strcmp(argv[1],"fragment")==0);return inspect(argv[2],true);}
