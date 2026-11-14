#include "meshoptimizer.h"
#include <vector>
#include <iostream>
#include <cmath>
int main(){
 const unsigned n=9;std::vector<float> flat,bent;std::vector<unsigned> ix;
 for(unsigned y=0;y<n;++y)for(unsigned x=0;x<n;++x){
  flat.push_back(float(x));flat.push_back(float(y));flat.push_back(0);
  bent.push_back(float(x));bent.push_back(float(y));bent.push_back((x%2&&y%2)?2.f:0.f);
 }
 for(unsigned y=0;y+1<n;++y)for(unsigned x=0;x+1<n;++x){unsigned a=y*n+x,b=a+1,c=a+n,d=c+1;unsigned t[]={a,b,c,b,d,c};ix.insert(ix.end(),t,t+6);}
 std::vector<unsigned> a(ix.size()),b(ix.size());float ea=0,eb=0;
 size_t na=meshopt_simplify(a.data(),ix.data(),ix.size(),flat.data(),n*n,12,96,.005f,meshopt_SimplifyLockBorder,&ea);
 size_t nb=meshopt_simplify(b.data(),ix.data(),ix.size(),bent.data(),n*n,12,96,.005f,meshopt_SimplifyLockBorder,&eb);
 if(!(na<ix.size()&&nb>na&&na%3==0&&nb%3==0&&std::isfinite(ea)&&std::isfinite(eb)))return 1;
 std::cout<<"BASELINE_OK input="<<ix.size()<<" flat="<<na<<" bent="<<nb<<" errors="<<ea<<","<<eb<<"\n";
}
