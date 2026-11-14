#include "Renderer.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
using namespace swr;
static int visits[32*32];
static float minimumZ[32*32],maximumZ[32*32];
static int outside,bad;
struct V { float x,y,z; };
struct VS : VertexShaderBase<VS> {
    static const int AttribCount=1, PVarCount=3;
    static void processVertex(VertexShaderInput in, VertexShaderOutput* out) {
        const V& v=*static_cast<const V*>(in[0]);
        out->x=v.x;out->y=v.y;out->z=(-11.f/9.f)*v.z-20.f/9.f;out->w=-v.z;
        out->pvar[0]=v.x;out->pvar[1]=v.y;out->pvar[2]=v.z;
    }
};
struct PS : PixelShaderBase<PS> {
    static const int InterpolateZ=true,InterpolateW=true,PVarCount=3;
    static void drawPixel(const PixelData& p) {
        if(p.x<0||p.y<0||p.x>=32||p.y>=32){++outside;return;}
        int k=p.y*32+p.x;++visits[k];
        minimumZ[k]=std::min(minimumZ[k],p.pvar[2]);maximumZ[k]=std::max(maximumZ[k],p.pvar[2]);
        double unproject=-20.0/(20.0-18.0*p.z);
        if(!std::isfinite(p.pvar[2])||std::fabs(unproject-p.pvar[2])>2e-4)++bad;
        double nx=(p.x+.5)/16.-1,ny=1-(p.y+.5)/16.;
        if(std::fabs(p.pvar[0]+nx*p.pvar[2])>2e-4||std::fabs(p.pvar[1]+ny*p.pvar[2])>2e-4)++bad;
    }
};
static void require(bool v,const char* msg){if(!v){std::fprintf(stderr,"BASELINE_FAIL %s\n",msg);std::exit(1);}}
int main(){
    const RasterMode modes[]={RasterMode::Span,RasterMode::Block,RasterMode::Adaptive};
    // Tetrahedron crosses near plane z=-1; all four faces go through real clipping.
    V v[]={{-1.3f,-1.f,-.5f},{1.3f,-1.f,-2.f},{0,1.4f,-2.f},{0,0,-5.f}};
    int faces[]={0,1,2,0,3,1,1,3,2,2,3,0};
    for(int mode=0;mode<3;++mode){
        std::fill(visits,visits+1024,0);std::fill(minimumZ,minimumZ+1024,1e9f);std::fill(maximumZ,maximumZ+1024,-1e9f);outside=bad=0;
        Rasterizer r;r.setRasterMode(modes[mode]);r.setScissorRect(0,0,32,32);r.setPixelShader<PS>();
        VertexProcessor vp(&r);vp.setViewport(0,0,32,32);vp.setCullMode(CullMode::None);vp.setVertexShader<VS>();vp.setVertexAttribPointer(0,sizeof(V),v);vp.drawElements(DrawMode::Triangle,12,faces);
        int n=0;for(int k:visits)n+=k;
        require(n>40,"no production fragments");require(!outside&&!bad,"perspective reconstruction");
        require(visits[16*32+16]==2,"two boundary faces at interior pixel");
        require(minimumZ[528]<maximumZ[528]-.3f,"separate boundary depths");
        std::printf("BOUNDARY mode=%d fragments=%d center=%d z=[%.6f,%.6f]\n",mode,n,visits[528],minimumZ[528],maximumZ[528]);
    }
    std::puts("BASELINE_OK real_clipped_boundaries perspective=3_modes");
}
