#include "cpurast.hpp"
#include "color_shader.hpp"
#include <cmath>
#include <cstdio>
struct MemoryCanvas:cr::canvas {
  std::vector<cr::color> pixels;
  void draw(const std::vector<cr::color>& p,unsigned w,unsigned h) override {if(p.size()!=w*h)std::abort();pixels=p;}
};
struct CountColor:color_fs { mutable unsigned calls=0; cr::color run(const std::vector<float>& in)const override {++calls;return color_fs::run(in);} };
static std::vector<float> vertex(float x,float y,float z,float r,float g){return {x,y,z,r,g,0};}
int main(){
  MemoryCanvas canvas;cr::context c(canvas,63,47);
  auto fs=std::make_shared<CountColor>();c.bind_vertex_shader(std::make_shared<color_vs>());c.bind_fragment_shader(fs);
  c.enable_depth_test();c.clear_framebuf(true,true);
  c.draw_triangle(vertex(-.75f,-.75f,.6f,1,0),vertex(.75f,-.75f,.6f,1,0),vertex(0,.75f,.6f,1,0));
  c.draw_triangle(vertex(-.75f,-.75f,-.4f,0,1),vertex(.75f,-.75f,-.4f,0,1),vertex(0,.75f,-.4f,0,1));
  c.update_canvas();unsigned red=0,green=0;
  for(auto p:canvas.pixels){if(!std::isfinite(p.r)||!std::isfinite(p.g)||!std::isfinite(p.b))return 1;red+=p.r>.5f;green+=p.g>.5f;}
  if(red<100||green||fs->calls!=red)return 2;const unsigned first_calls=fs->calls;
  c.clear_framebuf(true,true);
  c.draw_triangle(vertex(-.75f,-.75f,-.4f,0,1),vertex(.75f,-.75f,-.4f,0,1),vertex(0,.75f,-.4f,0,1));c.update_canvas();
  unsigned later=0;for(auto p:canvas.pixels){if(p.r>.5f)return 3;later+=p.g>.5f;}
  if(later!=red)return 4;
  std::printf("BASELINE_OK red=%u green=0 callbacks=%u second_frame=green\n",red,first_calls);
  std::puts("PRODUCTION_PATH context_renderer_scanline_earlydepth_shader_framebuf_canvas");return 0;
}
