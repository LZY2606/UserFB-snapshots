#include "Cpp/FastNoiseLite.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <stdexcept>
void require(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
int main(){
 FastNoiseLite n(1337); n.SetNoiseType(FastNoiseLite::NoiseType_ValueCubic);n.SetFrequency(0.73f);
 uint64_t digest=1469598103934665603ULL;
 for(int j=-4;j<5;++j)for(int i=-5;i<6;++i){
   float v=n.GetNoise(i*0.371+0.11,j*0.217-0.19);require(std::isfinite(v)&&std::abs(v)<=1.01,"scalar range");
   uint32_t bits;std::memcpy(&bits,&v,4);digest^=bits;digest*=1099511628211ULL;
 }
 std::cout<<"VALUECUBIC_GRID_OK points=99 digest="<<std::hex<<digest<<std::dec<<"\n";
 double samples[4];for(int k=0;k<4;++k){FastNoiseLite octave(1337+k);octave.SetNoiseType(FastNoiseLite::NoiseType_ValueCubic);octave.SetFrequency(0.73f);samples[k]=octave.GetNoise(-0.37*std::pow(1.75,k),0.92*std::pow(1.75,k));}
 n.SetFractalType(FastNoiseLite::FractalType_FBm);n.SetFractalOctaves(4);n.SetFractalLacunarity(1.75f);n.SetFractalGain(-0.5f);n.SetFractalWeightedStrength(0);
 double ref=(samples[0]-.5*samples[1]+.25*samples[2]-.125*samples[3])/1.875;
 require(std::abs(n.GetNoise(-0.37,0.92)-ref)<1e-6,"signed FBm");
 std::cout<<std::setprecision(10)<<"SIGNED_FBM_OK value="<<n.GetNoise(-0.37,0.92)<<" reference="<<ref<<"\n";
 n.SetFrequency(0);require(n.GetNoise(-12.3,5.9)==n.GetNoise(412.0,-82.0),"constant field");
 std::cout<<"ZERO_FREQUENCY_OK\nFASTNOISELITE_BASELINE_OK\n";
}
