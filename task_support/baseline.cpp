#include "geometrycentral/surface/flip_geodesics.h"
#include "geometrycentral/surface/surface_mesh_factories.h"
#include <cmath>
#include <iostream>
using namespace geometrycentral;using namespace geometrycentral::surface;
int main(){
 const size_t n=6;std::vector<Vector3> pos;std::vector<std::vector<size_t>> faces;
 for(size_t y=0;y<n;y++)for(size_t x=0;x<n;x++)pos.push_back({x<=2?double(x):2+.5*(x-2),double(y),x<=2?0.:std::sqrt(3.)*.5*(x-2)});
 for(size_t y=0;y+1<n;y++)for(size_t x=0;x+1<n;x++){size_t a=y*n+x;faces.push_back({a,a+1,a+n+1});faces.push_back({a,a+n+1,a+n});}
 auto [mp,gp]=makeManifoldSurfaceMeshAndGeometry(faces,pos);auto& mesh=*mp;auto& geom=*gp;
 std::vector<size_t> indices={6,7,13,14,20,21,27,28,29};std::vector<Halfedge> hs;
 for(size_t i=1;i<indices.size();i++){Halfedge found;bool ok=false;for(Halfedge h:mesh.vertex(indices[i-1]).outgoingHalfedges())if(h.tipVertex()==mesh.vertex(indices[i])){found=h;ok=true;break;}if(!ok)return 1;hs.push_back(found);}
 FlipEdgeNetwork net(mesh,geom,{hs});net.posGeom=&geom;net.supportRewinding=true;
 const double before=net.length();net.iterativeShorten();const double after=net.length();
 auto poly=net.getPathPolyline3D();if(!std::isfinite(after)||!(after<before)||net.nFlips==0||poly.size()!=1||poly[0].size()<2)return 2;
 std::cout<<"GEODESIC_CURRENT_OK before="<<before<<" after="<<after<<" flips="<<net.nFlips<<" points="<<poly[0].size()<<"\n";
}
