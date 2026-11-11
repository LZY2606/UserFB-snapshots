#include "SHLib.hpp"
#include <cmath>
#include <cstdio>
int main() {
    Vec4f constant[9], xLobe[9];
    for (int i=0;i<9;++i) constant[i]=xLobe[i]=Vec4f(vl_0);
    constant[0]=Vec4f(1/SHL::kSH_Y[0],1/SHL::kSH_Y[0],1/SHL::kSH_Y[0],0);
    xLobe[3]=Vec4f(1/SHL::kSH_Y[3],0,0,0);
    SHL::ApplyDiffuseBRDF(3,constant);
    SHL::ApplyDiffuseBRDF(3,xLobe);
    Vec4f a=SHL::SampleSH(Vec3f(1,0,0),3,constant);
    Vec4f b=SHL::SampleSH(Vec3f(1,0,0),3,xLobe);
    std::printf("constant diffuse %.9f %.9f %.9f; x lobe diffuse %.9f\n",a[0],a[1],a[2],b[0]);
    Vec4f rotated[9];
    SHL::CRotateSH(Mat3f(0,-1,0,1,0,0,0,0,1),3,xLobe,rotated);
    Vec4f c=SHL::SampleSH(Vec3f(0,1,0),3,rotated);
    std::printf("column Rz(+90) x lobe at +Y %.9f\n",c[0]);
    return std::fabs(a[0]-1)>1e-6 || std::fabs(b[0]-2.0/3)>1e-6 || std::fabs(c[0]-2.0/3)>1e-6;
}
