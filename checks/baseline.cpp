// Procedural input and current-behavior witness, not a target implementation.
#include "pmp/algorithms/remeshing.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace pmp;
static void require(bool b, const char* msg) { if (!b) throw std::runtime_error(msg); }
static SurfaceMesh grid(bool textured) {
    SurfaceMesh m;
    Vertex v[3][5];
    for (int y=0;y<3;++y) for(int x=0;x<5;++x)
        v[y][x]=m.add_vertex(Point(0.5f*x-1.0f,0.5f*y,0));
    for (int y=0;y<2;++y) for(int x=0;x<4;++x) {
        m.add_triangle(v[y][x],v[y][x+1],v[y+1][x+1]);
        m.add_triangle(v[y][x],v[y+1][x+1],v[y+1][x]);
    }
    if (textured) {
        auto uv=m.halfedge_property<TexCoord>("h:tex");
        for(auto f:m.faces()) {
            float cx=0; for(auto q:m.vertices(f)) cx+=m.position(q)[0];
            for(auto h:m.halfedges(f)) {
                auto q=m.position(m.to_vertex(h));
                uv[h]= cx<0 ? TexCoord(10+q[0],2+q[1]) : TexCoord(20+2*q[0],4+3*q[1]);
            }
        }
    }
    return m;
}
int main() {
    auto plain=grid(false), textured=grid(true);
    const auto before=plain.n_vertices();
    uniform_remeshing(plain,0.2f,2,true);
    uniform_remeshing(textured,0.2f,2,true);
    require(plain.n_vertices()>before,"no geometric refinement");
    require(plain.n_vertices()==textured.n_vertices(),"unexpected textured geometry difference");
    auto uv=textured.get_halfedge_property<TexCoord>("h:tex");
    size_t bad=0,corners=0;
    for(auto f:textured.faces()) {
        float cx=0; for(auto v:textured.vertices(f)) cx+=textured.position(v)[0];
        for(auto h:textured.halfedges(f)) {
            auto q=textured.position(textured.to_vertex(h));
            auto want= cx<0 ? TexCoord(10+q[0],2+q[1]) : TexCoord(20+2*q[0],4+3*q[1]);
            if(norm(uv[h]-want)>1e-4f) ++bad;
            ++corners;
            require(std::isfinite(q[0])&&std::isfinite(q[1])&&std::abs(q[2])<1e-5f,"invalid geometry");
        }
    }
    require(bad>0,"witness no longer exposes untransferred UV");
    std::cout << "PMP_GEOMETRY_BASELINE_OK vertices=" << before << "->" << plain.n_vertices() << " faces=" << plain.n_faces() << "\n";
    std::cout << "PMP_TEXTURE_TRANSFER_MISSING bad_corners=" << bad << "/" << corners << "\n";
}
