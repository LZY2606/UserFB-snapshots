#include <cassert>
#include <iostream>
#include <vector>
#include "agg_pixfmt_rgba.h"
#include "agg_renderer_base.h"
#include "agg_renderer_scanline.h"
#include "agg_rasterizer_scanline_aa.h"
#include "agg_scanline_u.h"
int main() {
    using pixfmt=agg::pixfmt_custom_blend_rgba<agg::comp_op_adaptor_rgba_pre<agg::rgba8,agg::order_rgba>,agg::rendering_buffer>;
    std::vector<unsigned char> bytes(8*8*4,0);
    agg::rendering_buffer rb(bytes.data(),8,8,8*4);
    pixfmt pix(rb,agg::comp_op_src_over);
    agg::renderer_base<pixfmt> ren(pix);
    ren.clear(agg::rgba8(0,0,64,128));
    agg::int8u cover[3]={0,128,255};
    pix.blend_solid_hspan(1,1,3,agg::rgba8(128,0,0,128),cover);
    for(int x=1;x<4;x++) {
        auto c=pix.pixel(x,1);
        std::cout<<"cover="<<int(cover[x-1])<<" rgba="<<int(c.r)<<","<<int(c.g)<<","<<int(c.b)<<","<<int(c.a)<<"\n";
        assert(c.r<=c.a && c.b<=c.a);
    }
    assert(pix.pixel(1,1).a==128 && pix.pixel(3,1).a>128);
    agg::rasterizer_scanline_aa<> ras; agg::scanline_u8 sl;
    ras.move_to_d(.5,.5);ras.line_to_d(7.5,1.5);ras.line_to_d(2.5,7.5);ras.close_polygon();
    agg::render_scanlines_aa_solid(ras,sl,ren,agg::rgba8(0,80,0,100));
    unsigned changed=0;for(int y=0;y<8;y++)for(int x=0;x<8;x++)changed+=pix.pixel(x,y).g>0;
    assert(changed>10);std::cout<<"triangle covered pixels="<<changed<<"\n";
}
