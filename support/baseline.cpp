#include "Renderer.h"
#include <vector>
#include <cstdio>
#include <cmath>
using namespace swr;
struct V{float x,y,z,w,c;};
struct PS:PixelShaderBase<PS>{static const bool InterpolateZ=true;static const bool InterpolateW=true;static const int PVarCount=1;static const int AVarCount=0;static int count;static double sum;static void drawPixel(const PixelData&p){if(p.x<0||p.x>=32||p.y<0||p.y>=32||!std::isfinite(p.pvar[0]))std::abort();count++;sum+=p.pvar[0];}};
int PS::count=0;double PS::sum=0;
struct VS:VertexShaderBase<VS>{static const int AttribCount=1;static const int AVarCount=0;static const int PVarCount=1;static void processVertex(VertexShaderInput in,VertexShaderOutput*out){auto*v=(const V*)in[0];out->x=v->x;out->y=v->y;out->z=v->z;out->w=v->w;out->pvar[0]=v->c;}};
int main(){V v[]={{-0.8f,-0.8f,0,1,0.2f},{0.8f,-0.8f,0,1,0.6f},{0,0.8f,0,1,1}};int indices[]={0,1,2};Rasterizer r;VertexProcessor p(&r);r.setScissorRect(0,0,32,32);r.setRasterMode(RasterMode::Span);r.setPixelShader<PS>();p.setViewport(0,0,32,32);p.setCullMode(CullMode::None);p.setVertexShader<VS>();p.setVertexAttribPointer(0,sizeof(V),v);p.drawElements(DrawMode::Triangle,3,indices);if(PS::count<=0||PS::sum<=0)return 1;printf("RASTER_SHADER fragments=%d color_sum=%.9f\n",PS::count,PS::sum);}
