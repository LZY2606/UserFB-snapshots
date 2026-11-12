#include "MidiFile.h"
#include <sstream>
#include <cassert>
#include <cstdio>
#include <cmath>
static std::string file(unsigned division){const unsigned char tr[]={0,255,81,3,3,208,144,0,144,60,90,135,104,128,60,0,0,255,47,0};std::string s("MThd\0\0\0\6\0\0\0\1",12);s.push_back(char(division>>8));s.push_back(char(division));s.append("MTrk\0\0\0",7);s.push_back(char(sizeof(tr)));s.append((const char*)tr,sizeof(tr));return s;}
int main(){
 std::istringstream ps(file(500));smf::MidiFile p;assert(p.read(ps));p.doTimeAnalysis();assert(std::abs(p.getFileDurationInSeconds()-.5)<1e-10);puts("PPQN_TIME_OK ticks=1000 seconds=0.5");
 std::istringstream ss(file(0xe728));smf::MidiFile s;assert(s.read(ss));s.doTimeAnalysis();std::ostringstream out;assert(s.write(out));auto b=out.str();unsigned div=((unsigned char)b[12]<<8)|(unsigned char)b[13];printf("SMPTE_STARTPOINT seconds=%.6f division=0x%04x expected=1.000000/0xe728\n",s.getFileDurationInSeconds(),div);
 p.setTPQ(1000);printf("CACHE_STARTPOINT seconds=%.6f expected=0.250000\n",p.getFileDurationInSeconds());
}
