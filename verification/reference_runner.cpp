/* Test-only unmodified fixed-source compressor, not a product command. */
#include "reference/lodepng.cpp"
#include <fstream>
#include <cstdlib>
#include <iterator>
int main(int argc,char** argv){
 if(argc!=8)return 90;
 std::ifstream in(argv[1],std::ios::binary);if(!in)return 91;
 std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
 LodePNGCompressSettings s;lodepng_compress_settings_init(&s);
 s.btype=(unsigned)std::strtoul(argv[3],0,10);s.windowsize=(unsigned)std::strtoul(argv[4],0,10);s.minmatch=(unsigned)std::strtoul(argv[5],0,10);s.nicematch=(unsigned)std::strtoul(argv[6],0,10);s.lazymatching=(unsigned)std::strtoul(argv[7],0,10);
 unsigned char* out=0;size_t size=0;unsigned e=lodepng_zlib_compress(&out,&size,bytes.data(),bytes.size(),&s);if(e)return (int)e;
 std::ofstream file(argv[2],std::ios::binary);file.write((char*)out,size);free(out);return file.good()?0:92;
}
