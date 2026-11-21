#include <cmath>
#include <iostream>
#include <stdexcept>
#include "manifold/manifold.h"
using namespace manifold;
int main() {
    auto a=Manifold::Cube({1,1,1}).SetProperties(2,[](double* out,vec3 p,const double*) {out[0]=p.x+2*p.y;out[1]=1-p.z;});
    auto b=a.Translate({.5,0,0});
    auto halves=a.Split(b);
    auto uni=a+b;
    if(halves.first.Status()!=Manifold::Error::NoError||halves.second.Status()!=Manifold::Error::NoError||uni.Status()!=Manifold::Error::NoError) throw std::runtime_error("status");
    if(std::abs(halves.first.Volume()-.5)>1e-8||std::abs(halves.second.Volume()-.5)>1e-8||std::abs(uni.Volume()-1.5)>1e-8) throw std::runtime_error("volume");
    auto m=halves.first.GetMeshGL64();
    if(m.numProp!=5||m.NumTri()==0||m.runOriginalID.empty()||(!m.faceID.empty()&&m.faceID.size()!=m.NumTri()))throw std::runtime_error("properties/relation");
    for(auto idx:m.triVerts)if(idx>=m.NumVert())throw std::runtime_error("index");
    std::cout<<"MANIFOLD_BASELINE intersection="<<halves.first.Volume()<<" difference="<<halves.second.Volume()<<" union="<<uni.Volume()<<" properties=2 source_runs="<<m.NumRun()<<" triangles="<<m.NumTri()<<"\n";
}
