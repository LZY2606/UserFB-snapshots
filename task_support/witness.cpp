#include "bw64/bw64.hpp"
#include <fstream>
#include <iostream>
#include <vector>
#include <cmath>
#include <stdexcept>
#include <type_traits>
template<class T> class has_reserve {
  template<class U> static auto probe(int) -> decltype(std::declval<U&>().reserveMetadata(uint32_t(64)),std::true_type());
  template<class> static std::false_type probe(...);
 public: static const bool value=decltype(probe<T>(0))::value;
};
static void need(bool b){if(!b) throw std::runtime_error("production witness mismatch");}
int main(){try {
 const std::string path=".task-build/production.wav";
 float samples[]={0.25f,-0.5f,0.75f};
 auto chna=std::make_shared<bw64::ChnaChunk>();chna->addAudioId(bw64::AudioId(1,"ATU_00000001","AT_00010001_01","AP_00010001"));
 auto w=bw64::writeFile(path,1,48000,24,chna);
 need(w->write(samples,3)==3);w->setAxmlChunk(std::make_shared<bw64::AxmlChunk>("abcde"));w->close();
 auto r=bw64::readFile(path);need(r->channels()==1 && r->numberOfFrames()==3 && r->chnaChunk()->numUids()==1 && r->axmlChunk()->data()=="abcde");float got[3]={};need(r->read(got,3)==3);for(int i=0;i<3;++i)need(std::fabs(got[i]-samples[i])<1e-6);r->close();
 std::ifstream f(path,std::ios::binary);uint32_t id=0,size=0;f.seekg(12);std::streamoff data=-1,axml=-1;uint32_t dataSize=0;while(f && f.peek()!=EOF){auto at=f.tellg();bw64::utils::readValue(f,id);bw64::utils::readValue(f,size);if(id==bw64::utils::fourCC("data")){data=at;dataSize=size;}if(id==bw64::utils::fourCC("axml"))axml=at;f.seekg(size+(size%2),std::ios::cur);}
 need(data>=0 && axml>data && dataSize==9);
 std::cout<<"production PCM frames=3 channels=1 depth=24 exact=true\n";
 std::cout<<"metadata chna_uids=1 axml=abcde data_bytes=9 axml_after_data=true\n";
 std::cout<<"reserve_api_present="<<std::boolalpha<<has_reserve<bw64::Bw64Writer>::value<<"\n";
 std::cout<<"production witness: ok\n";
 return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
