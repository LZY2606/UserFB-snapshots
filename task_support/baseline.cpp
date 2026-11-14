#include "libs/yocto/yocto_shape.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
using namespace yocto;
static void require(bool b,const char* msg) { if(!b) throw std::runtime_error(msg); }
struct topology {int edges,boundary_edges,face_components;};
static topology inspect(const vector<vec3i>& t) {
  std::map<std::pair<int,int>,vector<int>> edges;
  for(int f=0;f<(int)t.size();f++)for(int c=0;c<3;c++)
    edges[std::minmax(t[f][c],t[f][(c+1)%3])].push_back(f);
  vector<vector<int>> adj(t.size());int boundary=0;
  for(auto& e:edges) {require(e.second.size()<=2,"fixture edge manifold");
    if(e.second.size()==1)boundary++;
    else {auto a=e.second[0],b=e.second[1];adj[a].push_back(b);adj[b].push_back(a);}}
  vector<int> seen(t.size());int components=0;
  for(int i=0;i<(int)t.size();i++)if(!seen[i]) {
    components++;std::queue<int> q;q.push(i);seen[i]=1;
    while(!q.empty()){int f=q.front();q.pop();for(int g:adj[f])if(!seen[g]){seen[g]=1;q.push(g);}}}
  return {(int)edges.size(),boundary,components};
}
static int edge_direction(const vec3i& t,int a,int b) {
  for(int c=0;c<3;c++){if(t[c]==a&&t[(c+1)%3]==b)return 1;
    if(t[c]==b&&t[(c+1)%3]==a)return -1;}
  return 0;
}
static pair<vector<vec3i>,vector<vec3f>> band(int n,bool twisted) {
  vector<vec3f> p;vector<vec3i> t;
  for(int i=0;i<n;i++){float a=2*pif*i/n;
    for(int s:{-1,1}){float w=.25f*s;
      p.push_back({(2+w*std::cos(twisted?a/2:0))*std::cos(a),
        (2+w*std::cos(twisted?a/2:0))*std::sin(a),twisted?w*std::sin(a/2):w});}}
  for(int i=0;i<n;i++){int a=2*i,b=a+1,c=2*((i+1)%n),d=c+1;
    if(twisted&&i==n-1)std::swap(c,d);
    t.push_back({a,c,d});t.push_back({a,d,b});}
  return {t,p};
}
static int strip_cycle_parity(const vector<vec3i>& t) {
  // Known dual cycle through each strip cell: 1,0,3,2,... .
  int parity=0;
  for(int k=0;k<(int)t.size();k++){
    int f=k^1,g=((k+1)%(int)t.size())^1; bool found=false;
    for(int c=0;c<3;c++){int a=t[f][c],b=t[f][(c+1)%3];
      int dir=edge_direction(t[g],a,b);
      if(dir){parity^=(dir==1);found=true;break;}}
    require(found,"fixture dual cycle");
  }
  return parity;
}
int main() {
  shape_data disk;disk.positions={{0,0,0},{1,0,0},{1,1,0},{0,1,0}};
  disk.triangles={{0,1,2},{0,2,3}};
  const auto adj=face_adjacencies(disk.triangles);
  require(adj.size()==2&&adj[0].z==1&&adj[1].x==0,"shared edge adjacency");
  require(get_edges(disk.triangles).size()==5,"edge identity");
  require(get_boundary(make_edge_map(disk.triangles)).size()==4,"boundary edges");
  auto loops=ordered_boundaries(disk.triangles,adj,4);
  require(loops.size()==1&&loops[0].size()==4,"oriented disk boundary loop");
  auto normals=compute_normals(disk);auto flipped=disk;
  flipped.triangles=flip_triangles(disk.triangles);auto reverse=compute_normals(flipped);
  for(int v=0;v<4;v++)require(length(normals[v]-vec3f{0,0,1})<1e-6f&&length(normals[v]+reverse[v])<1e-6f,"flip/normal flow");
  require(disk.triangles[0]==vec3i{0,1,2},"source unchanged");
  std::cout<<"BASELINE disk adjacency boundary and opposite normals PASS\n";
  vector<vec4i> q={{0,1,2,2},{0,2,3,3}},uvq={{0,1,2,2},{3,4,5,5}},sq;
  vector<vec3f> sp,sn;vector<vec2f> st;
  vector<vec2f> uv={{0,0},{1,0},{1,1},{10,10},{11,11},{10,11}};
  split_facevarying(sq,sp,sn,st,q,{},uvq,disk.positions,{},uv);
  require(sp.size()==6&&st.size()==6&&sq[0].x!=sq[1].x,"face varying split");
  require(st[sq[0].x]==uv[0]&&st[sq[1].x]==uv[3],"corner seam preservation");
  require(sp[sq[0].x]==sp[sq[1].x],"attribute split keeps geometry");
  std::cout<<"BASELINE facevarying seam values PASS\n";
  const int n=7;auto annulus=band(n,false),mobius=band(n,true);
  for(auto* item:{&annulus,&mobius}){
    auto s=inspect(item->first);auto emap=make_edge_map(item->first);
    require(s.edges==4*n&&s.boundary_edges==2*n&&s.face_components==1,"band topology");
    require(num_edges(emap)==s.edges&&get_boundary(emap).size()==(size_t)s.boundary_edges,"upstream edge map bands");
    auto a=face_adjacencies(item->first);
    for(int f=0;f<2*n;f++)for(int c=0;c<3;c++){
      int g=a[f][c];if(g<0)continue;
      require(edge_direction(item->first[g],item->first[f][c],item->first[f][(c+1)%3])!=0,"band adjacency provenance");}
  }
  require(strip_cycle_parity(annulus.first)==0&&strip_cycle_parity(mobius.first)==1,"orientation obstruction witness");
  require(strip_cycle_parity(flip_triangles(mobius.first))==1,"global flip cannot repair obstruction");
  auto naive=mobius.first;auto other=flip_triangles(mobius.first);
  for(auto& t:other){t.x+=2*n;t.y+=2*n;t.z+=2*n;naive.push_back(t);}
  auto wrong=inspect(naive);
  require(wrong.face_components==2&&wrong.boundary_edges==4*n,"naive two copies stay disconnected");
  std::cout<<"BASELINE Mobius odd cycle and naive duplicate counterexample PASS\n";
}
