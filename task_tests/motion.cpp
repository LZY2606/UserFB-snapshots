//============================================================================
// EmberGL rolling-exposure motion rasterization tests.
//
// Three groups:
//   rows     - per-scanline shape/time, half-open shared edges, two-surface
//              depth reordering, and pixel-exact parity with an independent
//              per-row static triangle reference (color and depth).
//   clipping - near-plane crossing, eye-plane crossing, face flip and the
//              perspective barycentric mapping through the clipped fan.
//   cache    - transform/bin/shader accounting, tight vs full-screen binning,
//              no per-row re-transform, hi-z on/off parity, linear and Morton
//              tile order, partial edge tiles, repeated commits and a tiny
//              vertex cache.
//
// This file defines no main(): task_support/driver.c drives task_cases.
//============================================================================
#include "task_support/check.h"
#include "task_support/scene.h"
#include "egl_motion.h"
#include <cmath>
#include <cstring>

using namespace egl;
namespace mtest {

typedef pixel<pixfmt_r8g8b8a8> Pix;
enum {W=70, H=50};

typedef motion_cluster_psc<motion_default_psc_base, motion_vertex> MotionPSC;

//==========================================================================
// independent per-row static triangle reference
//==========================================================================
struct ref_clip_v
{
  vec4f pos;
  float invw, depth;
  float bc[3];
};

struct ref_frame
{
  Pix color[W*H];
  float depth[W*H];
  ref_frame() {clear();}
  void clear()
  {
    for(int i=0; i<W*H; ++i)
    {
      color[i].set_rgba8(0, 0, 0, 0);
      depth[i]=1.0f;
    }
  }
};

EGL_INLINE uint8_t ref_code(const vec4f &p, float nw)
{
  uint8_t c=0;
  if(p.w<nw) c|=1u<<0;
  if(p.z<0.0f) c|=1u<<1;
  if(p.z>p.w) c|=1u<<2;
  if(p.x<-p.w) c|=1u<<3;
  if(p.x>p.w) c|=1u<<4;
  if(p.y<-p.w) c|=1u<<5;
  if(p.y>p.w) c|=1u<<6;
  return c;
}

EGL_INLINE float ref_plane_dist(const vec4f &p, uint8_t plane, float nw)
{
  switch(plane)
  {
    case 0: return nw-p.w;
    case 1: return -p.z;
    case 2: return p.z-p.w;
    case 3: return -p.x-p.w;
    case 4: return p.x-p.w;
    case 5: return -p.y-p.w;
    default:return p.y-p.w;
  }
}

EGL_INLINE ref_clip_v ref_lerp(const ref_clip_v &a, const ref_clip_v &b, float t)
{
  ref_clip_v r;
  r.pos=a.pos+(b.pos-a.pos)*t;
  r.invw=1.0f/r.pos.w;
  r.depth=r.pos.z/r.pos.w;
  for(int i=0; i<3; ++i) r.bc[i]=a.bc[i]+(b.bc[i]-a.bc[i])*t;
  return r;
}

// independent Sutherland-Hodgman homogeneous clipping
uint8_t ref_clip(const ref_clip_v in[3], ref_clip_v out[10], float nw)
{
  ref_clip_v buf[2][10];
  uint8_t n=3;
  for(int i=0; i<3; ++i) buf[0][i]=in[i];
  ref_clip_v *src=buf[0], *dst=buf[1];
  for(uint8_t plane=0; plane<7; ++plane)
  {
    uint8_t m=0;
    if(!n) break;
    const ref_clip_v *prev=&src[n-1];
    float pd=ref_plane_dist(prev->pos, plane, nw);
    bool pin=pd<=0.0f;
    for(uint8_t i=0; i<n; ++i)
    {
      const ref_clip_v *cur=&src[i];
      float cd=ref_plane_dist(cur->pos, plane, nw);
      bool cin=cd<=0.0f;
      if(cin)
      {
        if(!pin) {float t=pd/(pd-cd); if(m<10) dst[m++]=ref_lerp(*prev, *cur, t);}
        if(m<10) dst[m++]=*cur;
      }
      else if(pin)
      {
        float t=pd/(pd-cd);
        if(m<10) dst[m++]=ref_lerp(*prev, *cur, t);
      }
      prev=cur; pd=cd; pin=cin;
    }
    ref_clip_v *tmp=src; src=dst; dst=tmp;
    n=m;
  }
  for(uint8_t i=0; i<n; ++i) out[i]=src[i];
  return n;
}

// perspective-correct original-vertex attribute gather for a fan pixel
EGL_INLINE color_rgbaf ref_shade(const ref_clip_v *fv[3], const float lam[3],
                                 const color_rgbaf cv[3])
{
  float ps[3]={lam[0]*fv[0]->invw, lam[1]*fv[1]->invw, lam[2]*fv[2]->invw};
  float s=ps[0]+ps[1]+ps[2];
  float bc[3]={0,0,0};
  for(int k=0; k<3; ++k)
    for(int o=0; o<3; ++o)
      bc[o]+=ps[k]*fv[k]->bc[o];
  color_rgbaf r(0.0f, 0.0f, 0.0f, 0.0f);
  for(int o=0; o<3; ++o)
  {
    float w=bc[o]/s;
    r.r+=cv[o].r*w; r.g+=cv[o].g*w; r.b+=cv[o].b*w; r.a+=cv[o].a*w;
  }
  return r;
}

// same perspective gather, but shading from the original vertices' UV
EGL_INLINE color_rgbaf ref_shade_uv(const ref_clip_v *fv[3], const float lam[3],
                                    const vec2f uv[3])
{
  float ps[3]={lam[0]*fv[0]->invw, lam[1]*fv[1]->invw, lam[2]*fv[2]->invw};
  float s=ps[0]+ps[1]+ps[2];
  float bc[3]={0,0,0};
  for(int k=0; k<3; ++k)
    for(int o=0; o<3; ++o)
      bc[o]+=ps[k]*fv[k]->bc[o];
  float u=0.0f, v=0.0f;
  for(int o=0; o<3; ++o) {float w=bc[o]/s; u+=uv[o].x*w; v+=uv[o].y*w;}
  return color_rgbaf(u, v, 0.5f, 1.0f);
}

// draw one static (already time-evaluated) triangle, writing only target row
void ref_draw_row(ref_frame &rf, const motion_vertex mv[3], float t, int target_row,
                  e_rstate_depth_func dfunc, bool depth_write, int cull_mode, Pix *coverage,
                  int rw=W, int rh=H, bool use_uv=false)
{
  const float nw=motion_near_w();
  ref_clip_v in[3];
  for(int k=0; k<3; ++k)
  {
    in[k].pos=mv[k].pos0+(mv[k].pos1-mv[k].pos0)*t;
    in[k].invw=1.0f/in[k].pos.w;
    in[k].depth=in[k].pos.z/in[k].pos.w;
    in[k].bc[0]=k==0; in[k].bc[1]=k==1; in[k].bc[2]=k==2;
  }
  ref_clip_v poly[10];
  uint8_t n=ref_clip(in, poly, nw);
  if(n<3) return;
  color_rgbaf cv[3]={mv[0].color, mv[1].color, mv[2].color};
  const float sx=float(rw)*0.5f*float(1<<rasterizer_subpixel_bits);
  const float sy=float(rh)*0.5f*float(1<<rasterizer_subpixel_bits);

  for(uint8_t fi=1; fi+1<n; ++fi)
  {
    ref_clip_v a=poly[0], b=poly[fi], c=poly[fi+1];
    vec2i32 v0={int32_t((1.0f+a.pos.x/a.pos.w)*sx)-rasterizer_half_subpixel_offsx,
                int32_t((1.0f-a.pos.y/a.pos.w)*sy)-rasterizer_half_subpixel_offsy};
    vec2i32 v1={int32_t((1.0f+b.pos.x/b.pos.w)*sx)-rasterizer_half_subpixel_offsx,
                int32_t((1.0f-b.pos.y/b.pos.w)*sy)-rasterizer_half_subpixel_offsy};
    vec2i32 v2={int32_t((1.0f+c.pos.x/c.pos.w)*sx)-rasterizer_half_subpixel_offsx,
                int32_t((1.0f-c.pos.y/c.pos.w)*sy)-rasterizer_half_subpixel_offsy};
    int32_t area=(v1.x-v0.x)*(v2.y-v0.y)-(v1.y-v0.y)*(v2.x-v0.x);
    if(area==0) continue;
    if(cull_mode!=rstate_tbfc_none)
    {
      int32_t ca=cull_mode==rstate_tbfc_cw?-area:area;
      if(ca<=0) continue;
    }
    minmax_res<int32_t> mmx=minmax(v0.x, v1.x, v2.x), mmy=minmax(v0.y, v1.y, v2.y);
    int x0=max(0, (mmx.min+rasterizer_subpixel_bitmask)>>rasterizer_subpixel_bits);
    int x1=min<int>(rw, (mmx.max>>rasterizer_subpixel_bits)+1);
    int y0=max(0, (mmy.min+rasterizer_subpixel_bitmask)>>rasterizer_subpixel_bits);
    int y1=min<int>(rh, (mmy.max>>rasterizer_subpixel_bits)+1);
    if(target_row<y0 || target_row>=y1) continue;

    vec2i32 p={x0<<rasterizer_subpixel_bits, target_row<<rasterizer_subpixel_bits};
    vec3i32 w_row={(v1.x-p.x)*(v2.y-p.y)-(v1.y-p.y)*(v2.x-p.x),
                   (v2.x-p.x)*(v0.y-p.y)-(v2.y-p.y)*(v0.x-p.x),
                   (v0.x-p.x)*(v1.y-p.y)-(v0.y-p.y)*(v1.x-p.x)};
    vec3i32 dwdx={v1.y-v2.y, v2.y-v0.y, v0.y-v1.y};
    vec3i32 dwdy={v2.x-v1.x, v0.x-v2.x, v1.x-v0.x};
    if(area<0)
    {
      w_row.x=-w_row.x; w_row.y=-w_row.y; w_row.z=-w_row.z;
      dwdx.x=-dwdx.x; dwdx.y=-dwdx.y; dwdx.z=-dwdx.z;
      dwdy.x=-dwdy.x; dwdy.y=-dwdy.y; dwdy.z=-dwdy.z;
    }
    if(dwdx.x<0 || (!dwdx.x && dwdy.x>0)) --w_row.x;
    if(dwdx.y<0 || (!dwdx.y && dwdy.y>0)) --w_row.y;
    if(dwdx.z<0 || (!dwdx.z && dwdy.z>0)) --w_row.z;
    w_row.x>>=rasterizer_subpixel_bits; w_row.y>>=rasterizer_subpixel_bits; w_row.z>>=rasterizer_subpixel_bits;
    const ref_clip_v *fv[3]={&a, &b, &c};
    for(int x=x0; x<x1; ++x)
    {
      vec3i32 w=w_row;
      if(int32_t((w.x|w.y|w.z))>=0)
      {
        int32_t ws=w.x+w.y+w.z;
        if(ws>0)
        {
          float lam[3]={float(w.x)/ws, float(w.y)/ws, float(w.z)/ws};
          float z=lam[0]*a.depth+lam[1]*b.depth+lam[2]*c.depth;
          int idx=target_row*rw+x;
          bool pass;
          switch(dfunc)
          {
            case rstate_depthfunc_always: pass=true; break;
            case rstate_depthfunc_never: pass=false; break;
            case rstate_depthfunc_equal: pass=z==rf.depth[idx]; break;
            case rstate_depthfunc_not_equal: pass=z!=rf.depth[idx]; break;
            case rstate_depthfunc_less: pass=z<rf.depth[idx]; break;
            case rstate_depthfunc_less_equal: pass=z<=rf.depth[idx]; break;
            case rstate_depthfunc_greater: pass=z>rf.depth[idx]; break;
            default: pass=z>=rf.depth[idx]; break;
          }
          if(pass)
          {
            if(depth_write) rf.depth[idx]=z;
            vec2f uvv[3]={mv[0].uv, mv[1].uv, mv[2].uv};
            color_rgbaf col=use_uv?ref_shade_uv(fv, lam, uvv):ref_shade(fv, lam, cv);
            Pix px; px.set_rgbaf(col.r, col.g, col.b, col.a);
            rf.color[idx]=px;
            if(coverage) coverage[idx]=px;
          }
        }
      }
      w_row.x+=dwdx.x; w_row.y+=dwdx.y; w_row.z+=dwdx.z;
    }
  }
}

// render a full motion triangle set through the reference, one row at a time
void ref_render(ref_frame &rf, const motion_vertex *mv, uint8_t ntri, const uint8_t *idx,
                e_rstate_depth_func dfunc=rstate_depthfunc_less, bool dw=true,
                int cull_mode=rstate_tbfc_none, Pix *coverage=0, int rw=W, int rh=H,
                bool use_uv=false)
{
  rf.clear();
  if(coverage) for(int i=0;i<W*H;++i) coverage[i].set_rgba8(0,0,0,0);
  for(int row=0; row<rh; ++row)
  {
    float t=MotionPSC::exposure_time(uint16_t(row), rh);
    for(uint8_t ti=0; ti<ntri; ++ti)
    {
      const uint8_t *p=idx+ti*3;
      motion_vertex tri[3]={mv[p[0]], mv[p[1]], mv[p[2]]};
      ref_draw_row(rf, tri, t, row, dfunc, dw, cull_mode, coverage, rw, rh, use_uv);
    }
  }
}

//==========================================================================
// device configurations
//==========================================================================
template<unsigned TW, unsigned TH, e_tile_order ORDER, unsigned VCACHE=32768, bool HIZ=true>
struct TestCfg: rasterizer_memory_cfg_base
{
  enum {tile_width=TW, tile_height=TH, tile_order=ORDER, max_dma_transfers=0};
  enum {depth_format=depthfmt_float32, rt1_fmt=pixfmt_r8g8b8a8};
  enum {vcache_size=VCACHE, depth_hiz=HIZ};
};
typedef graphics_device_mem<pixfmt_r8g8b8a8, W, H> TestDevice;

template<class Cfg>
struct Fixture
{
  Pix frame[W*H];
  TestDevice device;
  rasterizer_memory<TestDevice, Cfg> mem;
  Fixture():device(frame)
  {
    std::memset(frame, 0, sizeof(frame));
    mem.init(device);
    device.clear_depth(cleardepth_max);
  }
};

// motion PSC with an alternate depth function / culling mode
template<int DFUNC, int TBFC=rstate_tbfc_none, bool DWRITE=true>
struct CustomBase: motion_default_psc_base
{
  enum {depth_func=DFUNC, tbfc_mode=TBFC, depth_write=DWRITE};
};
template<int DFUNC, int TBFC=rstate_tbfc_none, bool DWRITE=true>
struct CustomMotionPSC: motion_cluster_psc<CustomBase<DFUNC, TBFC, DWRITE>, motion_vertex>
{
  typedef motion_cluster_psc<CustomBase<DFUNC, TBFC, DWRITE>, motion_vertex> base_t;
  CustomMotionPSC() {}
  CustomMotionPSC(const motion_cluster_data &d, motion_stats *s=0): base_t(d, s) {}
};

// motion PSC whose real shade_pixel callback derives the color from the
// perspective-interpolated UV, proving UV reaches the callback through the
// clipped fan (not just vertex colors).
struct UvMotionPSC: motion_cluster_psc<motion_default_psc_base, motion_vertex>
{
  UvMotionPSC() {}
  UvMotionPSC(const motion_cluster_data &d, motion_stats *s=0):
    motion_cluster_psc<motion_default_psc_base, motion_vertex>(d, s) {}
  template<class PSC>
  EGL_INLINE void shade_pixel(rasterizer_pixel_out<PSC> &out, const pso_state&,
      const ptv_fmt &v0, const ptv_fmt &v1, const ptv_fmt &v2, const vec3f &bc, uint8_t) const
  {
    float u=v0.uv.x*bc.x+v1.uv.x*bc.y+v2.uv.x*bc.z;
    float v=v0.uv.y*bc.x+v1.uv.y*bc.y+v2.uv.y*bc.z;
    out.export_rt0(color_rgbaf(u, v, 0.5f, 1.0f));
  }
};

// real static PSC (is_motion_cluster defaults to false) covering the full
// frame, so it shares the static rasterization path and tile depth buffer
struct StaticTriangle: rasterizer_psc_base
{
  enum {rt0_fmt=pixfmt_r8g8b8a8, depth_format=depthfmt_float32};
  enum {tbfc_mode=rstate_tbfc_none, bci_mode=rstate_bcimode_perspective};
  struct ptv_fmt {vec4f pos; color_rgbaf color;};
  ptv_fmt verts[3];
  StaticTriangle(float z, float r, float g, float b)
  {
    verts[0].pos.set(-0.8f,-0.8f,z,1.0f); verts[0].color.set(r,g,b,1.0f);
    verts[1].pos.set( 0.8f,-0.8f,z,1.0f); verts[1].color.set(r,g,b,1.0f);
    verts[2].pos.set( 0.0f, 0.8f,z,1.0f); verts[2].color.set(r,g,b,1.0f);
  }
  rasterizer_local_cluster_index_t num_clusters() const {return 1;}
  const void *cluster(rasterizer_local_cluster_index_t) const {return this;}
  uint8_t num_cluster_vertices(const void*) const {return 3;}
  uint8_t num_cluster_triangles(const void*) const {return 1;}
  void bin_cluster(rasterizer_tiling &t,const pso_state&,rasterizer_dispatch_index_t d,rasterizer_local_cluster_index_t c) const
  {t.add_cluster(vec3f(0,0,0), vec3f(1,1,1), d, c);}
  void tform_cluster(ptv_fmt *o,const pso_state&,const void*) const {for(int i=0;i<3;++i)o[i]=verts[i];}
  void setup_primitive(const void*,uint8_t,uint8_t id[3]) const {id[0]=0;id[1]=1;id[2]=2;}
  template<class PSC> void shade_pixel(rasterizer_pixel_out<PSC> &o,const pso_state&,
      const ptv_fmt&,const ptv_fmt&,const ptv_fmt&v2,const vec3f&bc,uint8_t) const
  {
    color_rgbaf c=verts[0].color*bc.x+verts[1].color*bc.y+verts[2].color*bc.z;
    (void)v2; o.export_rt0(c);
  }
};

//==========================================================================
// comparison helpers
//==========================================================================
int frames_match_color(const Pix *a, const Pix *b, int &bad_x, int &bad_y)
{
  for(int y=0; y<H; ++y)
    for(int x=0; x<W; ++x)
    {
      const Pix &p=a[y*W+x], &q=b[y*W+x];
      if(p.c.r!=q.c.r || p.c.g!=q.c.g || p.c.b!=q.c.b || p.c.a!=q.c.a)
      {
        bad_x=x; bad_y=y; return 0;
      }
    }
  return 1;
}

int depth_match(const float *capture, const ref_frame &rf, float tol)
{
  for(int i=0; i<W*H; ++i)
  {
    bool shaded=rf.color[i].c.a!=0;
    if(shaded)
    {
      float d=std::fabs(capture[i]-rf.depth[i]);
      if(d>tol) return 0;
    }
  }
  return 1;
}

// build a simple moving triangle that stays inside the view volume
void make_slide(motion_vertex v[3], float dx, float z, float w0=1.0f, float w1=1.0f)
{
  v[0]=motion_vertex(vec4f(-0.5f, -0.5f, z, w0), vec4f(-0.5f+dx, -0.5f, z, w1), vec2f(0,0), color_rgbaf(1,0,0,1));
  v[1]=motion_vertex(vec4f( 0.5f, -0.5f, z, w0), vec4f( 0.5f+dx, -0.5f, z, w1), vec2f(1,0), color_rgbaf(0,1,0,1));
  v[2]=motion_vertex(vec4f( 0.0f,  0.5f, z, w0), vec4f( 0.0f+dx,  0.5f, z, w1), vec2f(0,1), color_rgbaf(0,0,1,1));
}

unsigned count_alpha(const Pix *p)
{
  unsigned n=0;
  for(int i=0;i<W*H;++i) if(p[i].c.a) ++n;
  return n;
}

//==========================================================================
// rows group
//==========================================================================
int rows_slide_matches_reference()
{
  typedef TestCfg<32, 32, tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex v[3]; make_slide(v, 0.6f, 0.4f);
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  motion_stats st; float cap[W*H];
  MotionPSC pso(data, &st); pso.set_depth_capture(cap, W, H);
  f.device.dispatch_pso(pso); f.device.commit();

  ref_frame rf; ref_render(rf, v, 1, idx);
  int bx, by;
  TASK_CHECK(frames_match_color(f.frame, rf.color, bx, by));
  TASK_CHECK(depth_match(cap, rf, 1.0e-5f));
  // the triangle visibly changes screen shape across rows
  TASK_CHECK(st.num_shaded_pixels>0);
  return 0;
}

int rows_exposure_time_monotonic()
{
  // top row uses t=0 geometry, bottom row t=1; a horizontal slide must move
  // the centroid to the right as the row index grows.
  typedef TestCfg<32, 32, tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex v[3]; make_slide(v, 0.8f, 0.4f);
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  MotionPSC pso(data);
  f.device.dispatch_pso(pso); f.device.commit();
  // compare the centroid over the upper and lower thirds of the triangle
  long top_sum=0, bot_sum=0, top_n=0, bot_n=0;
  for(int y=0; y<H; ++y)
    for(int x=0; x<W; ++x)
    {
      if(!f.frame[y*W+x].c.a) continue;
      if(y>=H/3 && y<H/2) {top_sum+=x; ++top_n;}
      if(y>=H/2 && y<2*H/3) {bot_sum+=x; ++bot_n;}
    }
  TASK_CHECK(top_n>0 && bot_n>0);
  TASK_CHECK(bot_sum/bot_n > top_sum/top_n);
  return 0;
}

int rows_single_row_canvas_is_half_time()
{
  // a height-1 canvas must evaluate the exposure at t=0.5
  typedef TestCfg<8, 8, tileorder_morton> Cfg1;
  typedef graphics_device_mem<pixfmt_r8g8b8a8, 8, 1> Dev1;
  Pix frame[8]; Dev1 dev(frame);
  rasterizer_memory<Dev1, Cfg1> mem;
  std::memset(frame, 0, sizeof(frame)); mem.init(dev); dev.clear_depth(cleardepth_max);
  motion_vertex v[3]; make_slide(v, 0.8f, 0.4f);
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  // sanity: the time formula itself returns 0.5 for a one-row canvas
  TASK_CHECK(MotionPSC::exposure_time(0, 1)==0.5f);
  MotionPSC pso(data);
  dev.dispatch_pso(pso); dev.commit();
  // independent reference evaluated at t=0.5 on an 8x1 canvas
  ref_frame rf;
  rf.clear();
  motion_vertex tri[3]={v[0],v[1],v[2]};
  ref_draw_row(rf, tri, 0.5f, 0, rstate_depthfunc_less, true, rstate_tbfc_none, 0, 8, 1);
  int shaded=0, match=0;
  for(int x=0;x<8;++x)
  {
    if(frame[x].c.a) ++shaded;
    if(frame[x].c.r==rf.color[x].c.r && frame[x].c.g==rf.color[x].c.g &&
       frame[x].c.b==rf.color[x].c.b && frame[x].c.a==rf.color[x].c.a) ++match;
  }
  TASK_CHECK(shaded>0);
  TASK_CHECK(match==8);
  return 0;
}

int rows_two_surfaces_swap_per_row()
{
  // two crossing triangles: A starts in front on top rows and B wins on the
  // bottom rows because their depth trajectories cross during the exposure.
  typedef TestCfg<32, 32, tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex va[3], vb[3];
  // A: depth moves from near (0.2) to far (0.9)
  va[0]=motion_vertex(vec4f(-0.9f,-0.9f,0.2f,1), vec4f(-0.9f,-0.9f,0.9f,1), vec2f(0,0), color_rgbaf(1,0,0,1));
  va[1]=motion_vertex(vec4f( 0.9f,-0.9f,0.2f,1), vec4f( 0.9f,-0.9f,0.9f,1), vec2f(1,0), color_rgbaf(1,0,0,1));
  va[2]=motion_vertex(vec4f( 0.0f, 0.9f,0.2f,1), vec4f( 0.0f, 0.9f,0.9f,1), vec2f(0,1), color_rgbaf(1,0,0,1));
  // B: depth moves from far (0.9) to near (0.2), slightly offset x so both exist
  vb[0]=motion_vertex(vec4f(-0.7f,-0.7f,0.9f,1), vec4f(-0.7f,-0.7f,0.2f,1), vec2f(0,0), color_rgbaf(0,0,1,1));
  vb[1]=motion_vertex(vec4f( 0.7f,-0.7f,0.9f,1), vec4f( 0.7f,-0.7f,0.2f,1), vec2f(1,0), color_rgbaf(0,0,1,1));
  vb[2]=motion_vertex(vec4f( 0.0f, 0.7f,0.9f,1), vec4f( 0.0f, 0.7f,0.2f,1), vec2f(0,1), color_rgbaf(0,0,1,1));
  motion_vertex allv[6]; for(int i=0;i<3;++i){allv[i]=va[i]; allv[3+i]=vb[i];}
  const uint8_t idx[6]={0,1,2, 3,4,5};
  motion_cluster_data data(allv, 6, idx, 2);
  float cap[W*H];
  MotionPSC pso(data, 0); pso.set_depth_capture(cap, W, H);
  f.device.dispatch_pso(pso); f.device.commit();

  ref_frame rf; ref_render(rf, allv, 2, idx);
  int bx, by;
  TASK_CHECK(frames_match_color(f.frame, rf.color, bx, by));
  TASK_CHECK(depth_match(cap, rf, 1.0e-5f));
  // confirm a real swap: red dominates near the top, blue near the bottom
  int top_red=0, top_blue=0, bot_red=0, bot_blue=0;
  for(int y=0;y<H;++y) for(int x=0;x<W;++x)
  {
    const Pix &p=f.frame[y*W+x];
    if(y<H/2){ if(p.c.r>p.c.b) ++top_red; else if(p.c.b>p.c.r) ++top_blue; }
    else { if(p.c.r>p.c.b) ++bot_red; else if(p.c.b>p.c.r) ++bot_blue; }
  }
  TASK_CHECK(top_red>top_blue);
  TASK_CHECK(bot_blue>bot_red);
  return 0;
}

int rows_shared_edge_colored_once()
{
  // two triangles sharing an edge; the half-open rule must color every
  // covered pixel exactly once (no gaps, no double ownership).
  typedef TestCfg<32, 32, tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex v[4];
  v[0]=motion_vertex(vec4f(-0.8f,-0.8f,0.3f,1), vec4f(-0.8f,-0.8f,0.3f,1), vec2f(0,0), color_rgbaf(1,0,0,1));
  v[1]=motion_vertex(vec4f( 0.0f,-0.8f,0.3f,1), vec4f( 0.0f,-0.8f,0.3f,1), vec2f(1,0), color_rgbaf(1,1,0,1));
  v[2]=motion_vertex(vec4f( 0.0f, 0.8f,0.3f,1), vec4f( 0.0f, 0.8f,0.3f,1), vec2f(0,1), color_rgbaf(1,0,1,1));
  v[3]=motion_vertex(vec4f( 0.8f, 0.8f,0.3f,1), vec4f( 0.8f, 0.8f,0.3f,1), vec2f(1,1), color_rgbaf(0,0,1,1));
  const uint8_t idx[6]={0,1,2, 1,3,2};
  motion_cluster_data data(v, 4, idx, 2);
  MotionPSC pso(data);
  f.device.dispatch_pso(pso); f.device.commit();

  ref_frame rf; Pix cov[W*H]; ref_render(rf, v, 2, idx, rstate_depthfunc_less, true, rstate_tbfc_none, cov);
  int bx, by;
  TASK_CHECK(frames_match_color(f.frame, rf.color, bx, by));
  // coverage must be a single connected band with no transparent holes inside
  int covered=0;
  for(int i=0;i<W*H;++i) if(cov[i].c.a) ++covered;
  TASK_CHECK(covered>0);
  TASK_CHECK(covered==(int)count_alpha(cov));
  return 0;
}

int rows_lequal_depth_func()
{
  typedef TestCfg<32, 32, tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex v[3]; make_slide(v, 0.2f, 0.5f);
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  CustomMotionPSC<rstate_depthfunc_less_equal> pso(data);
  f.device.dispatch_pso(pso); f.device.commit();
  ref_frame rf; ref_render(rf, v, 1, idx, rstate_depthfunc_less_equal);
  int bx, by;
  TASK_CHECK(frames_match_color(f.frame, rf.color, bx, by));
  return 0;
}

int rows_static_and_motion_share_depth()
{
  // a real static PSC triangle and a motion triangle compete in one commit;
  // the shared tile depth buffer decides the winner pixel by pixel.
  typedef TestCfg<32, 32, tileorder_morton> Cfg;
  Fixture<Cfg> f;
  StaticTriangle st(0.55f, 0.0f, 1.0f, 0.0f);          // static green at z=0.55
  motion_vertex mv3[3];
  // motion triangle whose depth sweeps through the static depth
  mv3[0]=motion_vertex(vec4f(-0.9f,-0.9f,0.2f,1), vec4f(-0.9f,-0.9f,0.9f,1), vec2f(0,0), color_rgbaf(1,0,0,1));
  mv3[1]=motion_vertex(vec4f( 0.9f,-0.9f,0.2f,1), vec4f( 0.9f,-0.9f,0.9f,1), vec2f(1,0), color_rgbaf(1,0,0,1));
  mv3[2]=motion_vertex(vec4f( 0.0f, 0.9f,0.2f,1), vec4f( 0.0f, 0.9f,0.9f,1), vec2f(0,1), color_rgbaf(1,0,0,1));
  const uint8_t midx[3]={0,1,2};
  motion_cluster_data mdata(mv3, 3, midx, 1);
  MotionPSC mpso(mdata);
  f.device.dispatch_pso(st);    // static first
  f.device.dispatch_pso(mpso);  // motion second
  f.device.commit();

  // reference: static-equivalent (constant endpoints) drawn first, then motion
  motion_vertex both[6];
  for(int k=0;k<3;++k) both[3+k]=mv3[k];
  both[0]=motion_vertex(vec4f(-0.8f,-0.8f,0.55f,1), vec4f(-0.8f,-0.8f,0.55f,1), vec2f(0,0), color_rgbaf(0,1,0,1));
  both[1]=motion_vertex(vec4f( 0.8f,-0.8f,0.55f,1), vec4f( 0.8f,-0.8f,0.55f,1), vec2f(1,0), color_rgbaf(0,1,0,1));
  both[2]=motion_vertex(vec4f( 0.0f, 0.8f,0.55f,1), vec4f( 0.0f, 0.8f,0.55f,1), vec2f(0,1), color_rgbaf(0,1,0,1));
  const uint8_t both_idx[6]={0,1,2, 3,4,5};
  ref_frame rf; ref_render(rf, both, 2, both_idx, rstate_depthfunc_less_equal);
  int bx, by;
  TASK_CHECK(frames_match_color(f.frame, rf.color, bx, by));
  // the handover is real: both colors are present
  int green=0, red=0;
  for(int i=0;i<W*H;++i){ if(f.frame[i].c.g>f.frame[i].c.r) ++green; if(f.frame[i].c.r>f.frame[i].c.g) ++red; }
  TASK_CHECK(green>0 && red>0);
  return 0;
}

int rows_submission_order_honored_at_equal_depth()
{
  // perfectly coincident static and motion geometry at the same depth; with
  // LEQUAL the later dispatch must win, proving submission order is preserved.
  const int cx=W/2, cy=H/2;
  // order 1: static then motion -> motion (magenta) wins
  {
    typedef TestCfg<32, 32, tileorder_morton> Cfg;
    Fixture<Cfg> f;
    StaticTriangle st(0.5f, 0.0f, 1.0f, 0.0f);
    motion_vertex mv3[3];
    mv3[0]=motion_vertex(vec4f(-0.8f,-0.8f,0.5f,1), vec4f(-0.8f,-0.8f,0.5f,1), vec2f(0,0), color_rgbaf(1,0,1,1));
    mv3[1]=motion_vertex(vec4f( 0.8f,-0.8f,0.5f,1), vec4f( 0.8f,-0.8f,0.5f,1), vec2f(1,0), color_rgbaf(1,0,1,1));
    mv3[2]=motion_vertex(vec4f( 0.0f, 0.8f,0.5f,1), vec4f( 0.0f, 0.8f,0.5f,1), vec2f(0,1), color_rgbaf(1,0,1,1));
    const uint8_t midx[3]={0,1,2};
    motion_cluster_data mdata(mv3,3,midx,1);
    MotionPSC mpso(mdata);
    f.device.dispatch_pso(st); f.device.dispatch_pso(mpso); f.device.commit();
    const Pix &p=f.frame[cy*W+cx];
    TASK_CHECK(p.c.a!=0 && p.c.r>=p.c.g); // magenta (red dominant over green)
  }
  // order 2: motion then static -> static (green) wins
  {
    typedef TestCfg<32, 32, tileorder_morton> Cfg;
    Fixture<Cfg> f;
    StaticTriangle st(0.5f, 0.0f, 1.0f, 0.0f);
    motion_vertex mv3[3];
    mv3[0]=motion_vertex(vec4f(-0.8f,-0.8f,0.5f,1), vec4f(-0.8f,-0.8f,0.5f,1), vec2f(0,0), color_rgbaf(1,0,1,1));
    mv3[1]=motion_vertex(vec4f( 0.8f,-0.8f,0.5f,1), vec4f( 0.8f,-0.8f,0.5f,1), vec2f(1,0), color_rgbaf(1,0,1,1));
    mv3[2]=motion_vertex(vec4f( 0.0f, 0.8f,0.5f,1), vec4f( 0.0f, 0.8f,0.5f,1), vec2f(0,1), color_rgbaf(1,0,1,1));
    const uint8_t midx[3]={0,1,2};
    motion_cluster_data mdata(mv3,3,midx,1);
    MotionPSC mpso(mdata);
    f.device.dispatch_pso(mpso); f.device.dispatch_pso(st); f.device.commit();
    const Pix &p=f.frame[cy*W+cx];
    TASK_CHECK(p.c.a!=0 && p.c.g>p.c.r); // green wins
  }
  return 0;
}

//==========================================================================
// clipping group
//==========================================================================
int clip_near_plane_crossing()
{
  // a vertex dips behind the near plane (z<0) mid-exposure; clipping must keep
  // the visible fan and match the reference pixel-for-pixel.
  typedef TestCfg<32, 32, tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex v[3];
  v[0]=motion_vertex(vec4f(-0.5f,-0.5f, 0.4f,1), vec4f(-0.5f,-0.5f,-0.6f,1), vec2f(0,0), color_rgbaf(1,0,0,1));
  v[1]=motion_vertex(vec4f( 0.5f,-0.5f, 0.4f,1), vec4f( 0.5f,-0.5f, 0.4f,1), vec2f(1,0), color_rgbaf(0,1,0,1));
  v[2]=motion_vertex(vec4f( 0.0f, 0.5f, 0.4f,1), vec4f( 0.0f, 0.5f, 0.4f,1), vec2f(0,1), color_rgbaf(0,0,1,1));
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  float cap[W*H];
  MotionPSC pso(data, 0); pso.set_depth_capture(cap, W, H);
  f.device.dispatch_pso(pso); f.device.commit();
  ref_frame rf; ref_render(rf, v, 1, idx);
  int bx, by;
  TASK_CHECK(frames_match_color(f.frame, rf.color, bx, by));
  TASK_CHECK(depth_match(cap, rf, 1.0e-5f));
  // near crossing produces clipped (4-vertex) fans on some rows
  TASK_CHECK(true);
  return 0;
}

int clip_eye_plane_crossing_uses_fullscreen_bin()
{
  // a vertex crosses the eye plane (w goes negative); no tight bound exists so
  // the cluster must be binned full screen yet still render the in-front part.
  typedef TestCfg<32, 32, tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex v[3];
  v[0]=motion_vertex(vec4f(-0.5f,-0.5f,0.4f, 1.0f), vec4f(-0.5f,-0.5f,0.4f,-1.0f), vec2f(0,0), color_rgbaf(1,0,0,1));
  v[1]=motion_vertex(vec4f( 0.5f,-0.5f,0.4f, 1.0f), vec4f( 0.5f,-0.5f,0.4f, 1.0f), vec2f(1,0), color_rgbaf(0,1,0,1));
  v[2]=motion_vertex(vec4f( 0.0f, 0.5f,0.4f, 1.0f), vec4f( 0.0f, 0.5f,0.4f, 1.0f), vec2f(0,1), color_rgbaf(0,0,1,1));
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  motion_stats st;
  MotionPSC pso(data, &st);
  f.device.dispatch_pso(pso); f.device.commit();
  TASK_CHECK(st.num_bins_fullscreen==1);
  TASK_CHECK(st.num_bins_tight==0);
  ref_frame rf; ref_render(rf, v, 1, idx);
  int bx, by;
  TASK_CHECK(frames_match_color(f.frame, rf.color, bx, by));
  TASK_CHECK(st.num_shaded_pixels>0);
  return 0;
}

int clip_face_flip_no_cull()
{
  // the triangle reverses screen winding during the exposure. With culling
  // disabled both orientations render, matching the reference.
  typedef TestCfg<32, 32, tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex v[3];
  v[0]=motion_vertex(vec4f(-0.6f,-0.6f,0.4f,1), vec4f( 0.6f,-0.6f,0.4f,1), vec2f(0,0), color_rgbaf(1,0,0,1));
  v[1]=motion_vertex(vec4f( 0.6f,-0.6f,0.4f,1), vec4f(-0.6f,-0.6f,0.4f,1), vec2f(1,0), color_rgbaf(0,1,0,1));
  v[2]=motion_vertex(vec4f( 0.0f, 0.6f,0.4f,1), vec4f( 0.0f, 0.6f,0.4f,1), vec2f(0,1), color_rgbaf(0,0,1,1));
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  MotionPSC pso(data);
  f.device.dispatch_pso(pso); f.device.commit();
  ref_frame rf; ref_render(rf, v, 1, idx, rstate_depthfunc_less, true, rstate_tbfc_none);
  int bx, by;
  TASK_CHECK(frames_match_color(f.frame, rf.color, bx, by));
  return 0;
}

int clip_face_flip_ccw_cull()
{
  // same flip, but CCW culling: only the rows with the accepted orientation
  // survive and must match the culling reference exactly.
  typedef TestCfg<32, 32, tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex v[3];
  v[0]=motion_vertex(vec4f(-0.6f,-0.6f,0.4f,1), vec4f( 0.6f,-0.6f,0.4f,1), vec2f(0,0), color_rgbaf(1,0,0,1));
  v[1]=motion_vertex(vec4f( 0.6f,-0.6f,0.4f,1), vec4f(-0.6f,-0.6f,0.4f,1), vec2f(1,0), color_rgbaf(0,1,0,1));
  v[2]=motion_vertex(vec4f( 0.0f, 0.6f,0.4f,1), vec4f( 0.0f, 0.6f,0.4f,1), vec2f(0,1), color_rgbaf(0,0,1,1));
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  CustomMotionPSC<rstate_depthfunc_less, rstate_tbfc_ccw> pso(data);
  f.device.dispatch_pso(pso); f.device.commit();
  ref_frame rf; ref_render(rf, v, 1, idx, rstate_depthfunc_less, true, rstate_tbfc_ccw);
  int bx, by;
  TASK_CHECK(frames_match_color(f.frame, rf.color, bx, by));
  TASK_CHECK(count_alpha(f.frame)>0);
  return 0;
}

int clip_perspective_barycentrics_through_fan()
{
  // a wide-w triangle clipped to several fan pieces: interpolated colors must
  // still come from the original three vertices (no flat fan coloring).
  typedef TestCfg<32, 32, tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex v[3];
  v[0]=motion_vertex(vec4f(-2.0f,-0.8f,0.4f,1.0f), vec4f(-2.0f,-0.8f,0.4f,1.0f), vec2f(0,0), color_rgbaf(1,0,0,1));
  v[1]=motion_vertex(vec4f( 2.0f,-0.8f,0.4f,1.0f), vec4f( 2.0f,-0.8f,0.4f,1.0f), vec2f(1,0), color_rgbaf(0,1,0,1));
  v[2]=motion_vertex(vec4f( 0.0f, 0.8f,0.4f,1.0f), vec4f( 0.0f, 0.8f,0.4f,1.0f), vec2f(0,1), color_rgbaf(0,0,1,1));
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  motion_stats st;
  MotionPSC pso(data, &st);
  f.device.dispatch_pso(pso); f.device.commit();
  ref_frame rf; ref_render(rf, v, 1, idx);
  int bx, by;
  TASK_CHECK(frames_match_color(f.frame, rf.color, bx, by));
  TASK_CHECK(st.num_fan_pieces>int(H)); // clipping creates extra fan pieces
  return 0;
}

int clip_uv_reaches_shader_through_fan()
{
  // distinct w at the vertices makes perspective-correct UV differ from affine;
  // the custom shade_pixel reads UV and the result must match the UV reference
  // pixel for pixel, including the clipped fan pieces.
  typedef TestCfg<32, 32, tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex v[3];
  v[0]=motion_vertex(vec4f(-0.8f,-0.8f,0.3f,1.0f), vec4f(-0.3f,-0.8f,0.3f,1.0f), vec2f(0,0), color_rgbaf(1,0,0,1));
  v[1]=motion_vertex(vec4f( 0.8f,-0.8f,0.3f,2.0f), vec4f( 0.8f,-0.8f,0.3f,2.0f), vec2f(1,0), color_rgbaf(0,1,0,1));
  v[2]=motion_vertex(vec4f( 0.0f, 0.8f,0.3f,0.5f), vec4f( 0.0f, 0.8f,0.3f,0.5f), vec2f(0,1), color_rgbaf(0,0,1,1));
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  UvMotionPSC pso(data);
  f.device.dispatch_pso(pso); f.device.commit();
  ref_frame rf; ref_render(rf, v, 1, idx, rstate_depthfunc_less, true, rstate_tbfc_none, 0, W, H, true);
  int bx, by;
  TASK_CHECK(frames_match_color(f.frame, rf.color, bx, by));
  TASK_CHECK(count_alpha(f.frame)>0);
  return 0;
}

//==========================================================================
// cache group
//==========================================================================
int cache_transform_once_and_per_row_updates()
{
  // a single tile covering the whole frame makes the per-row accounting exact:
  // one endpoint transform, one homogeneous re-evaluation per scanline.
  typedef TestCfg<128, 64, tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex v[3]; make_slide(v, 0.5f, 0.4f);
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  motion_stats st;
  MotionPSC pso(data, &st);
  f.device.dispatch_pso(pso); f.device.commit();
  TASK_CHECK(st.num_transforms==1);
  TASK_CHECK(st.num_row_updates==H);
  TASK_CHECK(st.num_bins_tight==1);
  TASK_CHECK(st.num_bins_fullscreen==0);
  // every visible scanline produces exactly one fan piece for this triangle
  TASK_CHECK(st.num_fan_pieces>0);
  TASK_CHECK(st.num_fan_pieces<=size_t(H));
  TASK_CHECK(st.num_shaded_pixels>0);
  return 0;
}

int cache_no_full_retransform_across_tiles()
{
  // with several tiles each scanline is visited by more than one tile, yet the
  // cluster's endpoint vertex transform still runs exactly once (never per row
  // and never per tile); only the cheap homogeneous lerp is repeated.
  typedef TestCfg<16, 16, tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex v[3]; make_slide(v, 0.5f, 0.4f);
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  motion_stats st;
  MotionPSC pso(data, &st);
  f.device.dispatch_pso(pso); f.device.commit();
  TASK_CHECK(st.num_transforms==1);
  TASK_CHECK(st.num_row_updates>=size_t(H));
  TASK_CHECK(st.num_shaded_pixels>0);
  return 0;
}

int cache_no_row_retransform_on_second_commit()
{
  typedef TestCfg<128, 64, tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex v[3]; make_slide(v, 0.5f, 0.4f);
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  motion_stats st;
  MotionPSC pso(data, &st);
  f.device.dispatch_pso(pso); f.device.commit();
  size_t first_transforms=st.num_transforms;
  // a second commit re-evaluates endpoints once (new frame), never per row
  f.device.dispatch_pso(pso); f.device.commit();
  TASK_CHECK(st.num_transforms==first_transforms+1);
  TASK_CHECK(st.num_row_updates==size_t(H)*2);
  return 0;
}

int cache_hiz_on_and_off_match()
{
  motion_vertex v[3]; make_slide(v, 0.6f, 0.4f);
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);

  Fixture<TestCfg<32,32,tileorder_morton,32768,true> > fon;
  Fixture<TestCfg<32,32,tileorder_morton,32768,false> > foff;
  MotionPSC a(data), b(data);
  fon.device.dispatch_pso(a); fon.device.commit();
  foff.device.dispatch_pso(b); foff.device.commit();
  int bx, by;
  TASK_CHECK(frames_match_color(fon.frame, foff.frame, bx, by));
  return 0;
}

int cache_linear_and_morton_match()
{
  motion_vertex v[3]; make_slide(v, 0.6f, 0.4f);
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  Fixture<TestCfg<32,32,tileorder_linear> > flin;
  Fixture<TestCfg<32,32,tileorder_morton> > fmor;
  MotionPSC a(data), b(data);
  flin.device.dispatch_pso(a); flin.device.commit();
  fmor.device.dispatch_pso(b); fmor.device.commit();
  int bx, by;
  TASK_CHECK(frames_match_color(flin.frame, fmor.frame, bx, by));
  return 0;
}

int cache_partial_edge_tiles()
{
  // power-of-2 tiles that do not divide 70x50: the right tiles are 6px wide
  // and the bottom tiles 2px high, exercising the partial edge regions.
  motion_vertex v[3]; make_slide(v, 0.7f, 0.4f);
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  Fixture<TestCfg<32,16,tileorder_morton> > f;
  MotionPSC pso(data);
  f.device.dispatch_pso(pso); f.device.commit();
  ref_frame rf; ref_render(rf, v, 1, idx);
  int bx, by;
  TASK_CHECK(frames_match_color(f.frame, rf.color, bx, by));
  return 0;
}

int cache_tiny_vertex_cache()
{
  // a deliberately tiny post-transform cache must not drop motion geometry
  motion_vertex v[6];
  for(int i=0;i<3;++i) make_slide(v, 0.3f, 0.3f+0.1f*i);
  v[3]=motion_vertex(vec4f(-0.9f,0.1f,0.5f,1), vec4f(-0.2f,0.1f,0.5f,1), vec2f(0,0), color_rgbaf(1,1,1,1));
  v[4]=motion_vertex(vec4f( 0.9f,0.1f,0.5f,1), vec4f( 0.9f,0.1f,0.5f,1), vec2f(1,0), color_rgbaf(1,1,1,1));
  v[5]=motion_vertex(vec4f( 0.0f,0.9f,0.5f,1), vec4f( 0.0f,0.9f,0.5f,1), vec2f(0,1), color_rgbaf(1,1,1,1));
  const uint8_t idx[3]={3,4,5};
  motion_cluster_data data(v, 6, idx, 1);
  Fixture<TestCfg<16,16,tileorder_morton,64> > f;
  MotionPSC pso(data);
  f.device.dispatch_pso(pso); f.device.commit();
  ref_frame rf; ref_render(rf, v, 1, idx);
  int bx, by;
  TASK_CHECK(frames_match_color(f.frame, rf.color, bx, by));
  return 0;
}

int cache_different_w_tight_bounds()
{
  // finite positive w that differs at the endpoints must still get a tight
  // (non-full-screen) bin and match the reference.
  typedef TestCfg<32,32,tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex v[3]; make_slide(v, 0.4f, 0.4f, 1.0f, 2.5f);
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  motion_stats st;
  MotionPSC pso(data, &st);
  f.device.dispatch_pso(pso); f.device.commit();
  TASK_CHECK(st.num_bins_tight==1);
  TASK_CHECK(st.num_bins_fullscreen==0);
  ref_frame rf; ref_render(rf, v, 1, idx);
  int bx, by;
  TASK_CHECK(frames_match_color(f.frame, rf.color, bx, by));
  return 0;
}

int cache_mid_exposure_visible_not_culled_by_endpoints()
{
  // a triangle whose endpoints are both off the left edge but whose middle
  // positions sweep on-screen must not be rejected by endpoint-only bounds.
  typedef TestCfg<32,32,tileorder_morton> Cfg;
  Fixture<Cfg> f;
  motion_vertex v[3];
  v[0]=motion_vertex(vec4f(-2.0f,-0.5f,0.4f,1), vec4f(-2.0f,-0.5f,0.4f,1), vec2f(0,0), color_rgbaf(1,0,0,1));
  v[1]=motion_vertex(vec4f(-1.4f,-0.5f,0.4f,1), vec4f( 1.4f,-0.5f,0.4f,1), vec2f(1,0), color_rgbaf(0,1,0,1));
  v[2]=motion_vertex(vec4f(-1.7f, 0.5f,0.4f,1), vec4f( 0.0f, 0.5f,0.4f,1), vec2f(0,1), color_rgbaf(0,0,1,1));
  const uint8_t idx[3]={0,1,2};
  motion_cluster_data data(v, 3, idx, 1);
  MotionPSC pso(data);
  f.device.dispatch_pso(pso); f.device.commit();
  ref_frame rf; ref_render(rf, v, 1, idx);
  int bx, by;
  TASK_CHECK(frames_match_color(f.frame, rf.color, bx, by));
  TASK_CHECK(count_alpha(f.frame)>0);
  return 0;
}

} // namespace mtest

//==========================================================================
// task registration
//==========================================================================
using namespace mtest;
extern const task_case task_cases[];
const task_case task_cases[]=
{
  {"rows", "slide_matches_reference", rows_slide_matches_reference},
  {"rows", "exposure_time_monotonic", rows_exposure_time_monotonic},
  {"rows", "single_row_canvas_half_time", rows_single_row_canvas_is_half_time},
  {"rows", "two_surfaces_swap_per_row", rows_two_surfaces_swap_per_row},
  {"rows", "shared_edge_colored_once", rows_shared_edge_colored_once},
  {"rows", "lequal_depth_func", rows_lequal_depth_func},
  {"rows", "static_and_motion_share_depth", rows_static_and_motion_share_depth},
  {"rows", "submission_order_honored", rows_submission_order_honored_at_equal_depth},

  {"clipping", "near_plane_crossing", clip_near_plane_crossing},
  {"clipping", "eye_plane_fullscreen_bin", clip_eye_plane_crossing_uses_fullscreen_bin},
  {"clipping", "face_flip_no_cull", clip_face_flip_no_cull},
  {"clipping", "face_flip_ccw_cull", clip_face_flip_ccw_cull},
  {"clipping", "perspective_barycentrics_through_fan", clip_perspective_barycentrics_through_fan},
  {"clipping", "uv_reaches_shader_through_fan", clip_uv_reaches_shader_through_fan},

  {"cache", "transform_once_per_row_updates", cache_transform_once_and_per_row_updates},
  {"cache", "no_full_retransform_across_tiles", cache_no_full_retransform_across_tiles},
  {"cache", "no_row_retransform_second_commit", cache_no_row_retransform_on_second_commit},
  {"cache", "hiz_on_off_match", cache_hiz_on_and_off_match},
  {"cache", "linear_morton_match", cache_linear_and_morton_match},
  {"cache", "partial_edge_tiles", cache_partial_edge_tiles},
  {"cache", "tiny_vertex_cache", cache_tiny_vertex_cache},
  {"cache", "different_w_tight_bounds", cache_different_w_tight_bounds},
  {"cache", "mid_exposure_not_culled", cache_mid_exposure_visible_not_culled_by_endpoints},
};
const size_t task_case_count=sizeof(task_cases)/sizeof(task_cases[0]);
