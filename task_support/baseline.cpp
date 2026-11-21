#include <Mathematics/NURBSSurface.h>
#include <Mathematics/Vector3.h>
#include <Mathematics/CurveExtractorSquares.h>
#include <cmath>
#include <iostream>
#include <vector>
int main() {
    gte::BasisFunctionInput<double> b;
    b.numControls=4; b.degree=2; b.uniform=false; b.periodic=false;
    b.numUniqueKnots=3; b.uniqueKnots={{-2,3},{0,1},{2,3}};
    const double g[4]={-2,-1,1,2}, q[4]={4,0,0,4};
    gte::Vector3<double> c[16]; double w[16];
    for(int j=0;j<4;++j) for(int i=0;i<4;++i) {
        int k=i+4*j; w[k]=1+q[i]+q[j];
        c[k]={2*g[i]/w[k],2*g[j]/w[k],(q[i]+q[j]-1)/w[k]};
    }
    gte::NURBSSurface<3,double> s(b,b,c,w);
    double worst=0, normalResidual=0, second=0;
    for(int k=0;k<32;++k) {
        double a=6.2831853071795864769*k/32, u=std::cos(a),v=std::sin(a);
        gte::Vector3<double> jet[6]; s.Evaluate(u,v,2,jet);
        worst=std::max(worst,std::abs(gte::Length(jet[0])-1));
        auto n=gte::Cross(jet[1],jet[2]);
        normalResidual=std::max(normalResidual,std::abs(n[2])/gte::Length(n));
        second=std::max(second,gte::Length(jet[3]));
    }
    int pixels[4]={1,1,1,1};
    gte::CurveExtractorSquares<int,double> extractor(2,2,pixels);
    std::vector<gte::CurveExtractor<int,double>::Vertex> verts;
    std::vector<gte::CurveExtractor<int,double>::Edge> edges;
    extractor.Extract(0,verts,edges);
    if(worst>1e-12 || normalResidual>1e-12 || second<=0 || !edges.empty()) return 1;
    std::cout<<"NURBS_JET_OK samples=32 sphere_error="<<worst<<" normal_z="<<normalResidual<<" second="<<second<<"\n";
    std::cout<<"SAMPLED_GRID edges="<<edges.size()<<" (constant corner input; no surface contour queried)\n";
}
