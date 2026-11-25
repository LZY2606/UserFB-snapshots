#include "kt_bvh.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <limits>
#include <algorithm>
#include <cstdlib>
static void require(bool v){if(!v){std::fprintf(stderr,"triangle baseline assertion failed\n");std::exit(1);}}
struct V{double x,y,z;};
static V sub(V a,V b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
static V cross(V a,V b){return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
static double dot(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static bool triangle(const std::vector<float>&p,unsigned id,V o,V d,double&best){
 auto point=[&](unsigned k){return V{p[9*id+3*k],p[9*id+3*k+1],p[9*id+3*k+2]};};
 V a=point(0),e1=sub(point(1),a),e2=sub(point(2),a),h=cross(d,e2);double det=dot(e1,h);
 if(std::abs(det)<1e-12)return false;V s=sub(o,a);double u=dot(s,h)/det;if(u<0||u>1)return false;
 V c=cross(s,e1);double v=dot(d,c)/det;if(v<0||u+v>1)return false;
 double t=dot(e2,c)/det;if(t<0||t>=best)return false;best=t;return true;
}
static bool box(const float*lo,const float*hi,V o,V d,double best){
 double l=0,h=best;const double origin[]={o.x,o.y,o.z},dir[]={d.x,d.y,d.z};
 for(int a=0;a<3;++a){if(dir[a]==0){if(origin[a]<lo[a]||origin[a]>hi[a])return false;}else{
 double x=(lo[a]-origin[a])/dir[a],y=(hi[a]-origin[a])/dir[a];if(x>y)std::swap(x,y);l=std::max(l,x);h=std::min(h,y);}}
 return l<=h;
}
int main(){
 std::vector<float> p;for(int i=0;i<24;++i){float x=3.f*i,z=2.f+i%3;float a[]={x-.4f,-.4f,z,x+.4f,-.4f,z,x,.4f,z};p.insert(p.end(),a,a+9);}
 kt_bvh::TriMesh mesh{};mesh.set_vertices(p.data(),3*sizeof(float),72);mesh.set_unindexed();
 for(auto type:{kt_bvh::BVHBuildType::MedianSplit})for(auto width:{kt_bvh::BVHWidth::BVH2}){
 kt_bvh::BVHBuildDesc desc;desc.type=type;desc.width=width;desc.min_leaf_prims=1;desc.max_leaf_prims=2;
 auto* tree=kt_bvh::bvh_build_intermediate(&mesh,1,desc);require(tree!=nullptr);auto r=kt_bvh::bvh_build_result(tree);
 require(r.total_nodes>1&&r.prim_id_array_size==24);std::vector<kt_bvh::PrimitiveID> ids(r.prim_id_array,r.prim_id_array+24);std::vector<int> seen(24);
 for(auto id:ids){require(id.mesh_idx==0&&id.mesh_prim_idx<24);++seen[id.mesh_prim_idx];}for(auto n:seen)require(n==1);
 V o{21,0,0},d{0,0,2};double best=100;unsigned hit=~0u,tests=0;
 auto leaf=[&](unsigned off,unsigned count){for(unsigned i=0;i<count;++i){++tests;if(triangle(p,ids[off+i].mesh_prim_idx,o,d,best))hit=ids[off+i].mesh_prim_idx;}};
 if(width==kt_bvh::BVHWidth::BVH2){std::vector<kt_bvh::BVH2Node> n(r.total_nodes);require(kt_bvh::bvh2_intermediate_to_flat(tree,n.data(),n.size()));std::vector<unsigned>s{0};while(!s.empty()){auto i=s.back();s.pop_back();auto&a=n[i];if(!box(a.aabb_min,a.aabb_max,o,d,best))continue;if(a.is_leaf())leaf(a.right_child_or_prim_offset,a.num_prims_in_leaf);else{s.push_back(i+1);s.push_back(a.right_child_or_prim_offset);}}}
 else{std::vector<kt_bvh::BVH4Node>n(r.total_interior_nodes);require(kt_bvh::bvh4_intermediate_to_flat(tree,n.data(),n.size()));std::vector<unsigned>s{0};while(!s.empty()){auto i=s.back();s.pop_back();for(unsigned c=0;c<4;++c){auto&a=n[i];if(a.children[c]==UINT32_MAX)continue;float lo[3],hi[3];for(int k=0;k<3;++k){lo[k]=a.aabb_min_soa[k][c];hi[k]=a.aabb_max_soa[k][c];}if(!box(lo,hi,o,d,best))continue;if(a.is_child_leaf(c))leaf(a.children[c],a.num_prims_in_leaf[c]);else s.push_back(a.children[c]);}}}
 require(hit==7&&std::abs(best-1.5)<1e-8&&tests<24);std::printf("TRIANGLE_BASELINE width=%u strategy=%s source=7 t=%.8f primitive_tests=%u\n",desc.get_branching_factor(),type==kt_bvh::BVHBuildType::MedianSplit?"median":"sah",best,tests);kt_bvh::bvh_free_intermediate(tree);
 }
 puts("PRODUCTION_BUILD_TRACE_OK");
}
