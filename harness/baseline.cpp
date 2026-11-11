#include "QuickHull.hpp"
#include <cassert>
#include <iostream>
#include <map>
using namespace quickhull;
int main() {
  std::vector<Vector3<double>> p;
  for(int x:{-1,1}) for(int y:{-1,1}) for(int z:{-1,1}) p.emplace_back(x,y,z);
  QuickHull<double> q;
  auto h=q.getConvexHull(p,true,false);
  const auto& ids=h.getIndexBuffer(); const auto& v=h.getVertexBuffer();
  assert(ids.size()==36 && v.size()==8);
  std::map<std::pair<size_t,size_t>,int> edges;
  for(size_t i=0;i<ids.size();i+=3) for(int k=0;k<3;++k) {
    auto a=ids[i+k],b=ids[i+(k+1)%3];
    ++edges[std::minmax(a,b)];
  }
  for(const auto& e:edges) assert(e.second==2);
  assert(q.getDiagnostics().m_failedHorizonEdges==0);
  std::cout<<"QUICKHULL_CUBE_OK vertices=8 triangles=12 edges=18\n";
}
