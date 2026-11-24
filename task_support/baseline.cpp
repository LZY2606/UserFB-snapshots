#include "meshoptimizer.h"
#include <vector>
#include <cmath>
#include <cstdio>
#include <cassert>
struct V { float x,y,z; };
int main() {
    const int n=16; std::vector<V> p; std::vector<unsigned> ix;
    for(int y=0;y<=n;y++) for(int x=0;x<=n;x++) {
        V v={float(x)/n,float(y)/n,0.03f*float(std::sin(x*0.5)*std::sin(y*0.6))};p.push_back(v);
    }
    for(int y=0;y<n;y++) for(int x=0;x<n;x++) {
        unsigned a=y*(n+1)+x,b=a+1,c=a+n+1,d=c+1;
        ix.push_back(a);ix.push_back(b);ix.push_back(c);
        ix.push_back(c);ix.push_back(b);ix.push_back(d);
    }
    std::vector<unsigned> out(ix.size()); float e=-1;
    size_t count=meshopt_simplify(out.data(),ix.data(),ix.size(),&p[0].x,p.size(),sizeof(V),384,1.f,0,&e);
    assert(count>0 && count<ix.size() && count%3==0 && std::isfinite(e));
    for(size_t i=0;i<count;i++) assert(out[i]<p.size());
    std::vector<V> q=p; std::vector<unsigned> update=ix; float eu=-1;
    size_t moved=meshopt_simplifyWithUpdate(update.data(),update.size(),&q[0].x,q.size(),sizeof(V),0,0,0,0,0,384,1.f,0,&eu);
    assert(moved>0 && moved<ix.size() && moved%3==0 && std::isfinite(eu));
    size_t changed=0;for(size_t i=0;i<p.size();i++) changed+=(p[i].x!=q[i].x || p[i].y!=q[i].y || p[i].z!=q[i].z);
    assert(changed>0);
    std::printf("MESH_BASELINE input_triangles=%zu reduced_triangles=%zu update_triangles=%zu changed_vertices=%zu error=%.9g\n",ix.size()/3,count/3,moved/3,changed,e);
}
