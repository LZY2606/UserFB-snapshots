// File-format smoke check only; target tests must check shadow semantics.
#include "tgaimage.h"
#include <iostream>
int main(int argc,char**argv){if(argc!=2)return 2;TGAImage m,f;std::string d=argv[1];
 if(!m.read_tga_file(d+"/shadow-mask.tga")||!f.read_tga_file(d+"/shadow-frame.tga"))return 1;
 if(m.width()<1||m.height()<1||m.width()!=f.width()||m.height()!=f.height())return 1;
 std::cout<<"EXAMPLE_IMAGES_OK "<<f.width()<<"x"<<f.height()<<"\n";}
