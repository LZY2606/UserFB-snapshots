#include "CImg.h"
#include <cmath>
#include <iostream>
using namespace cimg_library;
int main() {
 CImg<double> dst(8,7,1,3,10), src(4,4,1,3,80);
 CImg<unsigned char> mask(4,4,1,1,0); mask(1,1)=255; mask(2,1)=255;
 dst.draw_image(2,1,0,0,src,mask,1,255);
 if(dst(3,2,0,0)!=80 || dst(2,1,0,0)!=10) return 1;
 CImg<double> ramp(5,5,1,1,0);
 cimg_forXY(ramp,x,y) ramp(x,y)=x*x+2*y*y;
 auto g=ramp.get_gradient("xy",0); auto lap=ramp.get_laplacian();
 if(std::abs(lap(2,2)-6)>1e-10 || g.size()!=2) return 2;
 CImg<double> b(1,2,1,1,0), A(2,2,1,1,0); A(0,0)=2; A(1,1)=4; b(0,0)=6;b(0,1)=8;
 auto x=b.get_solve(A); if(std::abs(x(0,0)-3)>1e-10||std::abs(x(0,1)-2)>1e-10)return 3;
 std::cout<<"CIMG_MASK_DRAW_OK value="<<dst(3,2)<<" lap="<<lap(2,2)<<"\n";
}
