#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>
#include "agg_path_storage.h"
#include "agg_conv_curve.h"
#include "agg_conv_stroke.h"
#include "agg_rasterizer_scanline_aa.h"
#include "agg_scanline_u.h"
#include "agg_renderer_scanline.h"
#include "agg_pixfmt_gray.h"
#include "agg_renderer_base.h"

static void check(bool b, const char* what) { if(!b) throw std::runtime_error(what); }
struct Point { unsigned cmd; double x,y; };
template<class Source> static std::vector<Point> read(Source& s) {
    std::vector<Point> v; s.rewind(0); double x=0,y=0;
    for(unsigned count=0; count<100000; ++count) {
        unsigned cmd=s.vertex(&x,&y); if(agg::is_stop(cmd)) return v;
        if(agg::is_vertex(cmd)) check(std::isfinite(x)&&std::isfinite(y),"nonfinite vertex");
        v.push_back(Point{cmd,x,y});
    }
    throw std::runtime_error("unbounded vertex stream");
}
template<class Source> static unsigned long paint(Source& s) {
    std::vector<unsigned char> data(64*64,0);
    agg::rendering_buffer buf(data.data(),64,64,64);
    agg::pixfmt_gray8 pix(buf); agg::renderer_base<agg::pixfmt_gray8> ren(pix);
    agg::rasterizer_scanline_aa<> ras; agg::scanline_u8 sl;
    ras.add_path(s); agg::render_scanlines_aa_solid(ras,sl,ren,agg::gray8(255));
    unsigned long sum=0; for(auto x:data) sum+=x; return sum;
}
int main() {
  try {
    agg::path_storage p; p.move_to(8,16); p.line_to(48,16);
    agg::conv_stroke<agg::path_storage> stroke(p); stroke.width(4); stroke.line_cap(agg::butt_cap);
    auto line=read(stroke); check(line.size()==5,"line outline commands");
    check(paint(stroke)==40800,"constant butt stroke area");
    p.remove_all(); p.move_to(16,16);p.line_to(48,16);p.line_to(48,48);p.line_to(16,48);p.close_polygon();
    auto ring=read(stroke); unsigned closed=0;for(auto v:ring) closed+=agg::is_close(v.cmd)!=0;
    check(closed==2,"closed stroke needs two contours");check(paint(stroke)==130560,"closed stroke hole area");
    p.remove_all();p.move_to(8,32);p.curve4(8,4,56,4,56,32);p.line_to(56,32);p.line_to(48,48);
    agg::conv_curve<agg::path_storage> curve(p);curve.approximation_scale(4);
    agg::conv_stroke<decltype(curve)> curved(curve);curved.width(4);curved.line_join(agg::round_join);curved.line_cap(agg::round_cap);
    auto a=read(curved),b=read(curved);check(a.size()>20&&a.size()==b.size(),"curve stream/repeated point");
    for(size_t i=0;i<a.size();++i)check(a[i].cmd==b[i].cmd&&a[i].x==b[i].x&&a[i].y==b[i].y,"rewind changed stroke");
    auto sum=paint(curved);check(sum>40000&&sum<200000,"curve raster is empty or implausible");
    std::cout<<"AGG_OLD_STROKE_OK line_sum=40800 ring_sum=130560 curve_vertices="<<a.size()<<" curve_sum="<<sum<<"\n";
    std::cout<<"BASELINE_COMPLETE groups=3\n";
  } catch(const std::exception& e) {std::cerr<<"BASELINE_FAIL "<<e.what()<<"\n";return 1;}
}
