#include <fstream>
#include <cmath>
int main(int n,char**v){if(n!=2)return 2;std::ifstream f(v[1],std::ios::binary);float x;unsigned count=0;double energy=0;while(f.read((char*)&x,4)){if(!std::isfinite(x))return 3;energy+=x*x;++count;}return count>=8&&energy>1e-12&&f.gcount()==0?0:4;}
