#include "common/ffsparser.h"
#include "common/ffs.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <fstream>
using Bytes=std::vector<unsigned char>;
static void put(Bytes& b,size_t p,unsigned long long v,size_t n){for(size_t i=0;i<n;++i)b.at(p+i)=static_cast<unsigned char>(v>>(8*i));}
static void guid(Bytes& b,size_t p,const UByteArray& g){std::memcpy(b.data()+p,g.constData(),16);}
static Bytes volume(size_t n,bool vtf){
  Bytes b(n,0xff); for(size_t i=0;i<72;++i)b[i]=0;
  guid(b,16,EFI_FIRMWARE_FILE_SYSTEM2_GUID);put(b,32,n,8);put(b,40,0x4856465f,4);
  put(b,44,0x800,4);put(b,48,72,2);b[55]=2;put(b,56,1,4);put(b,60,n,4);
  unsigned sum=0;for(size_t i=0;i<72;i+=2)sum+=b[i]+(b[i+1]<<8);put(b,50,(-sum)&65535,2);
  for(size_t i=72;i<n;++i)b[i]=0;
  if(vtf)guid(b,72,EFI_FFS_VOLUME_TOP_FILE_GUID);else{b[72]=0x75;b[73]=0x61;}
  b[89]=0xaa;b[90]=EFI_FV_FILETYPE_RAW;put(b,92,n-72,3);b[95]=0xf8;
  sum=0;for(size_t i=72;i<96;++i)if(i!=89&&i!=95)sum+=b[i];b[88]=static_cast<unsigned char>(-sum);
  return b;
}
static Bytes capsule(){
  Bytes a=volume(512,true),b=volume(768,true);const size_t h=32,f=24;
  Bytes out(h+f+32+a.size()+40+b.size(),0);guid(out,0,EFI_FMP_CAPSULE_GUID);
  put(out,16,h,4);put(out,24,out.size(),4);put(out,h,1,4);put(out,h+6,2,2);
  size_t p=h+f;put(out,h+8,f,8);put(out,h+16,f+32+a.size(),8);
  put(out,p,1,4);out[p+20]=1;put(out,p+24,a.size(),4);
  std::memcpy(out.data()+p+32,a.data(),a.size());p+=32+a.size();
  put(out,p,2,4);out[p+20]=2;put(out,p+24,b.size(),4);put(out,p+32,0x1234,8);
  std::memcpy(out.data()+p+40,b.data(),b.size());return out;
}
static unsigned count(TreeModel& m,const UModelIndex& p,unsigned type){unsigned n=0;for(int i=0;i<m.rowCount(p);++i){auto x=m.index(i,0,p);if(m.type(x)==type)++n;n+=count(m,x,type);}return n;}
static void showVolumes(TreeModel& m,const UModelIndex& p){for(int i=0;i<m.rowCount(p);++i){auto x=m.index(i,0,p);if(m.type(x)==Types::Volume){std::printf("VOLUME base=%x bytes=%x\n%s\n",m.base(x),m.fullSize(x),m.info(x).toLocal8Bit());}showVolumes(m,x);}}
static bool inspect(const Bytes& b,const char* label){TreeModel m;FfsParser p(&m);auto s=p.parse(UByteArray(reinterpret_cast<const char*>(b.data()),b.size()));unsigned v=count(m,UModelIndex(),Types::Volume),im=count(m,UModelIndex(),Types::Image),f=count(m,UModelIndex(),Types::File);std::printf("%s status=%zu images=%u volumes=%u files=%u address_diff=%llx\n",label,size_t(s),im,v,f,(unsigned long long)p.getAddressDiff());showVolumes(m,UModelIndex());return s==U_SUCCESS&&v>0&&f>0;}
int main(int argc,char**argv){auto a=volume(512,true);auto b=capsule();if(argc==2){std::ofstream(argv[1],std::ios::binary).write(reinterpret_cast<const char*>(b.data()),b.size());}if(!inspect(a,"BARE_FV")||!inspect(b,"FMP_CURRENT"))return 1;std::puts("BASELINE_OK");}
