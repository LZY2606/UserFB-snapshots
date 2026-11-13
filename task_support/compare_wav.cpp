#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
int main(int argc,char** argv) {
    if(argc!=3) return 2;
    std::ifstream a(argv[1],std::ios::binary), b(argv[2],std::ios::binary);
    if(!a || !b) return 3;
    std::vector<char> x((std::istreambuf_iterator<char>(a)),{}), y((std::istreambuf_iterator<char>(b)),{});
    if(x.size()<100000 || x!=y) return 4;
    bool nonzero=false;
    for(size_t i=44;i<x.size();++i) nonzero |= x[i]!=0;
    if(!nonzero) return 5;
    std::cout << "UPSTREAM_NSF_FILE_MEMORY_OK bytes=" << x.size() << '\n';
}
