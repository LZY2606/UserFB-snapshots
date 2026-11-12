#include "FileLoop.h"
#include <fstream>
#include <cassert>
#include <cmath>
#include <cstdio>
static void word(std::ofstream&f,unsigned x,int n){while(n--){f.put(x&255);x>>=8;}}
int main(int argc,char**argv){
 assert(argc==2);std::string p=std::string(argv[1])+"/tiny.wav";std::ofstream f(p,std::ios::binary);
 f.write("RIFF",4);word(f,68,4);f.write("WAVEfmt ",8);word(f,16,4);word(f,1,2);word(f,2,2);word(f,8000,4);word(f,32000,4);word(f,4,2);word(f,16,2);f.write("data",4);word(f,32,4);
 for(int i=0;i<8;i++){word(f,100*(i+1),2);word(f,1000+50*i,2);}f.close();
 stk::Stk::setSampleRate(8000);stk::FileLoop a(p,false,false,1000,4,false),b(p,false,false,2,4,false);
 a.setRate(1);b.setRate(1);double sum=0;
 for(int i=0;i<64;i++){double x=a.tick(),y=b.tick();assert(std::abs(x-y)<1e-10);assert(std::abs(a.lastOut(1)-b.lastOut(1))<1e-10);sum+=x;}
 assert(sum>0);std::printf("STK_FILELOOP_OK frames=64 sum=%.1f\n",sum);
}
