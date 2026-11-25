//============================================================================
// EmberGL
//
// CPU rolling-exposure motion rasterization support.
//
// A motion cluster stores, per vertex, the homogeneous clip-space position at
// both exposure endpoints. At scanline row y the vertex position is evaluated
// at time t=y/(height-1) by interpolating the *homogeneous* coordinates. The
// per-row triangle is clipped and rasterized without ever re-submitting the
// scene. The header is self contained on top of the existing rasterizer and
// plugs into dispatch_pso()/commit() through an optional customization hook
// (see motion_raster_context / motion_rasterize()).
//============================================================================
#ifndef EGL_MOTION_H
#define EGL_MOTION_H
#ifdef __cplusplus
//============================================================================

#include "egl_rasterizer.h"
#include <cmath>
EGL_NAMESPACE_BEGIN

// new
struct motion_vertex;
struct motion_cluster_data;
struct motion_stats;
template<class PSC> struct motion_raster_context;
template<class PSC> struct motion_pixel_out_access;
template<class Base, class VAttr> class motion_cluster_psc;

// minimum positive homogeneous w accepted as "in front of the eye"
EGL_INLINE float motion_near_w() {return 1.0e-6f;}
//----------------------------------------------------------------------------


//============================================================================
// motion_vertex
//============================================================================
// Vertex with the homogeneous position at exposure start (pos0) and exposure
// end (pos1). UV and color do not move during the exposure.
struct motion_vertex
{
  EGL_INLINE motion_vertex() {}
  EGL_INLINE motion_vertex(const vec4f &pos0_, const vec4f &pos1_, const vec2f &uv_, const color_rgbaf &color_):
    pos0(pos0_), pos1(pos1_), uv(uv_), color(color_) {}

  vec4f pos0, pos1;
  vec2f uv;
  color_rgbaf color;
};
//----------------------------------------------------------------------------


//============================================================================
// motion_cluster_data
//============================================================================
// Indexed triangle cluster. Triangle indices stay fixed for the whole
// exposure.
struct motion_cluster_data
{
  motion_cluster_data(): vertices(0), num_vertices(0), tri_indices(0), num_triangles(0) {}
  motion_cluster_data(const motion_vertex *vertices_, uint8_t num_vertices_, const uint8_t *tri_indices_, uint8_t num_triangles_):
    vertices(vertices_), num_vertices(num_vertices_), tri_indices(tri_indices_), num_triangles(num_triangles_) {}

  const motion_vertex *vertices;
  uint8_t num_vertices;
  const uint8_t *tri_indices;
  uint8_t num_triangles;
};
//----------------------------------------------------------------------------


//============================================================================
// motion_stats
//============================================================================
// Counters describing the work the motion pipeline actually performed.
struct motion_stats
{
  EGL_INLINE motion_stats() {reset();}
  EGL_INLINE void reset()
  {
    num_transforms=0;
    num_row_updates=0;
    num_bins_fullscreen=0;
    num_bins_tight=0;
    num_fan_pieces=0;
    num_shaded_pixels=0;
  }

  size_t num_transforms;       // full cluster vertex transforms (once per commit)
  size_t num_row_updates;      // per-row homogeneous re-evaluations of the cluster
  size_t num_bins_fullscreen;  // clusters binned conservatively to every tile
  size_t num_bins_tight;       // clusters binned to a tight screen range
  size_t num_fan_pieces;       // triangle fan pieces produced by row clipping
  size_t num_shaded_pixels;    // pixels that passed coverage and depth tests
};
//----------------------------------------------------------------------------


//============================================================================
// motion_raster_context
//============================================================================
// Everything the per-row motion rasterizer needs from the tile pass.
template<class PSC>
struct motion_raster_context
{
  typedef PSC psc_t;
  typedef rasterizer_pixel_out<PSC> pixel_out_t;
  const render_target_cfg *rt_cfg;
  rasterizer_pixel_out<PSC> *psout;
  void *depth_buf;
  uint16_t tile_x, tile_y, tile_width, tile_height;
  uint16_t thread_idx;
  vec2u16 *tile_min;
  vec2u16 *tile_end;
};
//----------------------------------------------------------------------------


//============================================================================
// motion_pixel_out_access
//============================================================================
// Grants the motion rasterizer (a friend of rasterizer_pixel_out) access to
// the pixel output accumulator.
template<class PSC>
struct motion_pixel_out_access
{
  static EGL_INLINE void set_rts(rasterizer_pixel_out<PSC> &psout_, void *const*rts_)
  {
    for(unsigned i=0; i<rasterizer_max_rts; ++i)
      psout_.m_rts[i]=rts_[i];
  }
  static EGL_INLINE void *const*get_rts(rasterizer_pixel_out<PSC> &psout_)
  {
    return psout_.m_rts;
  }
  static EGL_INLINE void set_px_offs(rasterizer_pixel_out<PSC> &psout_, uint32_t px_offs_)
  {
    psout_.m_px_offs=px_offs_;
  }
};
//----------------------------------------------------------------------------


//============================================================================
// internal motion rasterization state
//============================================================================
namespace motion_detail
{

enum
{
  motion_max_vertices=32,
  motion_max_triangles=12,
  motion_max_clip_verts=10   // a triangle clipped against 7 planes has <=10 verts
};

// cluster vertex with both endpoint positions (transform output, reused for
// every scanline of the commit)
struct cached_vertex
{
  vec4f pos0, pos1;
  vec2f uv;
  color_rgbaf color;
};

// a vertex of a clipped per-row polygon. bc[3] are the barycentric weights of
// this point with respect to the original triangle, invw=1/w of the current
// homogeneous position.
struct clip_vertex
{
  vec4f pos;
  float invw;
  float depth;    // z/w of the current position
  float bc[3];
};

struct clipped_triangle
{
  uint8_t num_verts;
  clip_vertex verts[motion_max_clip_verts];
};

// homogeneous clip plane codes
enum e_clip_code
{
  clip_w      =1<<0,  // w < eps (behind / on the eye plane)
  clip_near_z =1<<1,  // z < 0
  clip_far_z  =1<<2,  // z > w
  clip_left_x =1<<3,  // x < -w
  clip_right_x=1<<4,  // x > w
  clip_bottom_y=1<<5, // y < -w
  clip_top_y  =1<<6   // y > w
};

EGL_INLINE uint8_t clip_code(const vec4f &p_, float near_w_)
{
  uint8_t code=0;
  if(p_.w<near_w_) code|=clip_w;
  if(p_.z<0.0f) code|=clip_near_z;
  if(p_.z>p_.w) code|=clip_far_z;
  if(p_.x<-p_.w) code|=clip_left_x;
  if(p_.x>p_.w) code|=clip_right_x;
  if(p_.y<-p_.w) code|=clip_bottom_y;
  if(p_.y>p_.w) code|=clip_top_y;
  return code;
}

// signed distance of p_ to the plane given by the clip code (positive outside)
EGL_INLINE float clip_distance(const vec4f &p_, uint8_t plane_, float near_w_)
{
  switch(plane_)
  {
    case clip_w:       return near_w_-p_.w;
    case clip_near_z:  return -p_.z;
    case clip_far_z:   return p_.z-p_.w;
    case clip_left_x:  return -p_.x-p_.w;
    case clip_right_x: return p_.x-p_.w;
    case clip_bottom_y:return -p_.y-p_.w;
    case clip_top_y:   return p_.y-p_.w;
  }
  return 0.0f;
}

EGL_INLINE clip_vertex lerp_clip_vertex(const clip_vertex &a_, const clip_vertex &b_, float t_)
{
  clip_vertex r;
  r.pos=a_.pos+(b_.pos-a_.pos)*t_;
  float w=r.pos.w;
  r.invw=1.0f/w;
  r.depth=r.pos.z/w;
  for(unsigned i=0; i<3; ++i)
    r.bc[i]=a_.bc[i]+(b_.bc[i]-a_.bc[i])*t_;
  return r;
}

// Sutherland-Hodgman clipping of a homogeneous triangle against the view
// volume w>=eps, 0<=z<=w, -w<=x,y<=w. The output polygon always keeps the
// perspective barycentric mapping back to the original triangle.
EGL_INLINE uint8_t clip_triangle(const clip_vertex in_[3], clip_vertex out_[motion_max_clip_verts], float near_w_)
{
  clip_vertex buf[2][motion_max_clip_verts];
  uint8_t num_in=3, num_out=0;
  for(unsigned i=0; i<3; ++i)
    buf[0][i]=in_[i];
  clip_vertex *src=buf[0], *dst=buf[1];
  static const uint8_t planes[7]={clip_w, clip_near_z, clip_far_z, clip_left_x, clip_right_x, clip_bottom_y, clip_top_y};
  for(unsigned pi=0; pi<7; ++pi)
  {
    uint8_t plane=planes[pi];
    num_out=0;
    if(!num_in)
      break;
    const clip_vertex *prev=&src[num_in-1];
    float prev_d=clip_distance(prev->pos, plane, near_w_);
    bool prev_inside=prev_d<=0.0f;
    for(unsigned i=0; i<num_in; ++i)
    {
      const clip_vertex *cur=&src[i];
      float cur_d=clip_distance(cur->pos, plane, near_w_);
      bool cur_inside=cur_d<=0.0f;
      if(cur_inside)
      {
        if(!prev_inside)
        {
          float t=prev_d/(prev_d-cur_d);
          if(num_out<motion_max_clip_verts) dst[num_out++]=lerp_clip_vertex(*prev, *cur, t);
        }
        if(num_out<motion_max_clip_verts) dst[num_out++]=*cur;
      }
      else if(prev_inside)
      {
        float t=prev_d/(prev_d-cur_d);
        if(num_out<motion_max_clip_verts) dst[num_out++]=lerp_clip_vertex(*prev, *cur, t);
      }
      prev=cur;
      prev_d=cur_d;
      prev_inside=cur_inside;
    }
    clip_vertex *tmp=src; src=dst; dst=tmp;
    num_in=num_out;
  }
  for(unsigned i=0; i<num_in; ++i)
    out_[i]=src[i];
  return num_in;
}

EGL_INLINE bool finite_vec4(const vec4f &p_)
{
  return std::isfinite(p_.x) && std::isfinite(p_.y) && std::isfinite(p_.z) && std::isfinite(p_.w);
}

} // namespace motion_detail
//----------------------------------------------------------------------------


//============================================================================
// motion_default_psc_base
//============================================================================
// Default render state for the motion PSC. Tests can substitute another base
// to change the pixel/depth format, depth function or culling mode.
struct motion_default_psc_base: rasterizer_psc_base
{
  enum {rt0_fmt=pixfmt_r8g8b8a8};
  enum {depth_format=depthfmt_float32};
  enum {tbfc_mode=rstate_tbfc_none};
  enum {bci_mode=rstate_bcimode_perspective};
  enum {vertex_cache=false};
};
//----------------------------------------------------------------------------


//============================================================================
// motion_cluster_psc
//============================================================================
template<class Base, class VAttr>
class motion_cluster_psc: public Base
{
public:
  typedef VAttr motion_vertex_t;

  // motion clusters own their per-scanline rasterization
  enum {is_motion_cluster=true};

  // post transform vertex format handed to shade_pixel
  struct ptv_fmt
  {
    vec4f pos;
    vec2f uv;
    color_rgbaf color;
  };
  // pso_state is inherited from Base (rasterizer_psc_base by default)
  typedef typename Base::pso_state pso_state;
  //--------------------------------------------------------------------------

  EGL_INLINE motion_cluster_psc(): m_data(0), m_stats(0) {reset_cache_();}
  explicit EGL_INLINE motion_cluster_psc(const motion_cluster_data &data_, motion_stats *stats_=0): m_data(&data_), m_stats(stats_) {reset_cache_();}

  EGL_INLINE void set_motion_data(const motion_cluster_data &data_, motion_stats *stats_=0)
  {
    m_data=&data_;
    m_stats=stats_;
    reset_cache_();
  }

  // Optional W*H float buffer that records the winning NDC depth (z/w) of
  // every pixel the motion cluster shades, for verification.
  EGL_INLINE void set_depth_capture(float *capture_, uint16_t width_, uint16_t height_)
  {
    m_depth_capture=capture_;
    m_capture_width=width_;
    m_capture_height=height_;
  }

  // exposure time of scanline row_ of a height_ pixel canvas
  static EGL_INLINE float exposure_time(uint16_t row_, uint16_t height_)
  {
    return height_<=1?0.5f:float(row_)/float(height_-1);
  }

  //--------------------------------------------------------------------------
  // PSC interface
  //--------------------------------------------------------------------------
  EGL_INLINE rasterizer_local_cluster_index_t num_clusters() const {return 1;}
  EGL_INLINE const void *cluster(rasterizer_local_cluster_index_t) const {return this;}
  EGL_INLINE uint8_t num_cluster_vertices(const void*) const {return m_data?m_data->num_vertices:0;}
  EGL_INLINE uint8_t num_cluster_triangles(const void*) const {return m_data?m_data->num_triangles:0;}

  void bin_cluster(rasterizer_tiling &tiling_, const pso_state&, rasterizer_dispatch_index_t dispatch_idx_, rasterizer_local_cluster_index_t cluster_idx_) const
  {
    // Conservative screen range of the whole motion path. When every endpoint
    // stays in front of the eye the projected vertex traces are monotonic
    // fractional-linear curves, so the six endpoint screen positions bound
    // every intermediate row tightly. Otherwise fall back to full screen.
    bool tight=m_data && m_data->num_vertices && m_data->num_triangles;
    float u0=1.0f, v0=1.0f, u1=0.0f, v1=0.0f;
    float near_w=motion_near_w();
    if(tight)
    {
      for(uint8_t vi=0; vi<m_data->num_vertices; ++vi)
      {
        const vec4f &p0=m_data->vertices[vi].pos0;
        const vec4f &p1=m_data->vertices[vi].pos1;
        if(!motion_detail::finite_vec4(p0) || !motion_detail::finite_vec4(p1) || p0.w<=near_w || p1.w<=near_w)
        {
          tight=false;
          break;
        }
        float us[2]={(p0.x/p0.w+1.0f)*0.5f, (p1.x/p1.w+1.0f)*0.5f};
        float vs[2]={(1.0f-p0.y/p0.w)*0.5f, (1.0f-p1.y/p1.w)*0.5f};
        for(unsigned k=0; k<2; ++k)
        {
          float uu=clamp(us[k], 0.0f, 1.0f);
          float vv=clamp(vs[k], 0.0f, 1.0f);
          u0=min(u0, uu); u1=max(u1, uu);
          v0=min(v0, vv); v1=max(v1, vv);
        }
      }
    }
    if(!tight)
    {
      u0=v0=0.0f;
      u1=v1=1.0f;
      if(m_stats) ++m_stats->num_bins_fullscreen;
    }
    else if(m_stats) ++m_stats->num_bins_tight;
    // depth range is kept conservative so hi-z can never reject a row
    tiling_.add_cluster(vec3f(u0, v0, 0.0f), vec3f(u1, v1, 1.0f), dispatch_idx_, cluster_idx_);
  }

  void tform_cluster(ptv_fmt *ptvs_, const pso_state&, const void*) const
  {
    // The endpoint positions are transformed once; all later scanlines only
    // interpolate these cached homogeneous positions.
    if(!m_cache.transformed)
    {
      EGL_ASSERT_MSG(m_data->num_vertices<=motion_detail::motion_max_vertices, "Too many motion cluster vertices.\r\n");
      EGL_ASSERT_MSG(m_data->num_triangles<=motion_detail::motion_max_triangles, "Too many motion cluster triangles.\r\n");
      m_cache.num_vertices=m_data->num_vertices;
      for(uint8_t vi=0; vi<m_data->num_vertices; ++vi)
      {
        const motion_vertex &mv=m_data->vertices[vi];
        motion_detail::cached_vertex &cv=m_cache.verts[vi];
        cv.pos0=mv.pos0;
        cv.pos1=mv.pos1;
        cv.uv=mv.uv;
        cv.color=mv.color;
      }
      m_cache.transformed=true;
      if(m_stats) ++m_stats->num_transforms;
    }
    for(uint8_t vi=0; vi<m_cache.num_vertices; ++vi)
    {
      ptvs_[vi].pos=m_cache.verts[vi].pos0;
      ptvs_[vi].uv=m_cache.verts[vi].uv;
      ptvs_[vi].color=m_cache.verts[vi].color;
    }
  }

  EGL_INLINE void setup_primitive(const void*, uint8_t prim_idx_, uint8_t vidx_[3]) const
  {
    const uint8_t *pidx=m_data->tri_indices+prim_idx_*3;
    vidx_[0]=pidx[0];
    vidx_[1]=pidx[1];
    vidx_[2]=pidx[2];
  }

  // Default pixel shader: perspective-correct UV/color of the original three
  // vertices. Custom PSCs can override this.
  template<class PSC>
  EGL_INLINE void shade_pixel(rasterizer_pixel_out<PSC> &psout_, const pso_state&, const ptv_fmt &v0_, const ptv_fmt &v1_, const ptv_fmt &v2_, const vec3f &bc_, uint8_t) const
  {
    color_rgbaf c;
    c.r=v0_.color.r*bc_.x+v1_.color.r*bc_.y+v2_.color.r*bc_.z;
    c.g=v0_.color.g*bc_.x+v1_.color.g*bc_.y+v2_.color.g*bc_.z;
    c.b=v0_.color.b*bc_.x+v1_.color.b*bc_.y+v2_.color.b*bc_.z;
    c.a=v0_.color.a*bc_.x+v1_.color.a*bc_.y+v2_.color.a*bc_.z;
    psout_.export_rt0(c);
  }

  //--------------------------------------------------------------------------
  // motion rasterization entry point (called from the rasterizer hook)
  //--------------------------------------------------------------------------
  template<class Ctx>
  bool motion_rasterize(Ctx &ctx_, const pso_state&) const
  {
    if(!m_data || !m_data->num_triangles)
      return true;
    const render_target_cfg &rt=*ctx_.rt_cfg;
    const uint16_t rt_w=rt.rt_width, rt_h=rt.rt_height;
    const float near_w=motion_near_w();
    m_depth_buf=ctx_.depth_buf;

    // prepare per-row geometry for every scanline touched by this tile
    for(uint16_t ly=0; ly<ctx_.tile_height; ++ly)
    {
      uint16_t row=ctx_.tile_y+ly;
      prepare_row_(row, rt_h, near_w);
      rasterize_row_(ctx_, row, rt_w, rt_h, near_w);
    }
    return true;
  }

private:
  //--------------------------------------------------------------------------
  struct row_cache
  {
    bool transformed;
    uint16_t row;
    bool row_valid;
    uint8_t num_vertices;
    motion_detail::cached_vertex verts[motion_detail::motion_max_vertices];
    motion_detail::clipped_triangle tris[motion_detail::motion_max_triangles];
  };

  EGL_INLINE void reset_cache_()
  {
    m_cache.transformed=false;
    m_cache.row=uint16_t(-1);
    m_cache.row_valid=false;
    m_cache.num_vertices=0;
    m_depth_buf=0;
    m_depth_capture=0;
    m_capture_width=0;
    m_capture_height=0;
  }

  EGL_INLINE float row_time_(uint16_t row_, uint16_t rt_h_) const
  {
    return exposure_time(row_, rt_h_);
  }

  // Re-evaluate the whole cluster for the scanline. Only homogeneous endpoint
  // lerps and row clipping happen here; the endpoint transform is reused.
  void prepare_row_(uint16_t row_, uint16_t rt_h_, float near_w_) const
  {
    if(m_cache.row_valid && m_cache.row==row_)
      return;
    float t=row_time_(row_, rt_h_);
    motion_detail::clip_vertex tri_in[3];
    for(uint8_t ti=0; ti<m_data->num_triangles; ++ti)
    {
      const uint8_t *pidx=m_data->tri_indices+ti*3;
      for(unsigned k=0; k<3; ++k)
      {
        uint8_t vi=pidx[k];
        const motion_detail::cached_vertex &cv=m_cache.verts[vi];
        motion_detail::clip_vertex &cv_out=tri_in[k];
        cv_out.pos=cv.pos0+(cv.pos1-cv.pos0)*t;
        cv_out.invw=cv_out.pos.w!=0.0f?1.0f/cv_out.pos.w:0.0f;
        cv_out.depth=cv_out.pos.w!=0.0f?cv_out.pos.z/cv_out.pos.w:0.0f;
        cv_out.bc[0]=k==0?1.0f:0.0f;
        cv_out.bc[1]=k==1?1.0f:0.0f;
        cv_out.bc[2]=k==2?1.0f:0.0f;
      }
      motion_detail::clipped_triangle &ct=m_cache.tris[ti];
      ct.num_verts=motion_detail::clip_triangle(tri_in, ct.verts, near_w_);
    }
    m_cache.row=row_;
    m_cache.row_valid=true;
    if(m_stats) ++m_stats->num_row_updates;
  }

  template<class Ctx>
  void rasterize_row_(Ctx &ctx_, uint16_t row_, uint16_t rt_w_, uint16_t rt_h_, float near_w_) const
  {
    (void)near_w_;
    // the concrete (possibly derived) PSC type carried by the context, so the
    // correct shade_pixel override is selected at compile time
    typedef typename Ctx::psc_t concrete_psc_t;
    typedef motion_pixel_out_access<concrete_psc_t> access_t;
    const render_target_cfg &rt=*ctx_.rt_cfg;
    // build a pixel output of the concrete PSC's type sharing the tile storage
    rasterizer_pixel_out<concrete_psc_t> local_psout;
    access_t::set_rts(local_psout,
      motion_pixel_out_access<typename Ctx::psc_t>::get_rts(*ctx_.psout));
    rasterizer_pixel_out<concrete_psc_t> &psout=local_psout;
    const uint16_t pitch=rt.tile_pitch;
    const int x_hi=int(ctx_.tile_x)+ctx_.tile_width;

    // sub-pixel projection scale. For even canvas dimensions this equals the
    // static rasterizer's integer rt_coord_scale exactly, while remaining
    // valid for degenerate single-row/single-column canvases.
    const float scale_x=float(rt_w_)*0.5f*float(1<<rasterizer_subpixel_bits);
    const float scale_y=float(rt_h_)*0.5f*float(1<<rasterizer_subpixel_bits);

    for(uint8_t ti=0; ti<m_data->num_triangles; ++ti)
    {
      const motion_detail::clipped_triangle &ct=m_cache.tris[ti];
      if(ct.num_verts<3)
        continue;

      // fan the clipped polygon around its first vertex; every fan piece maps
      // through the perspective barycentrics back to the original triangle.
      for(uint8_t fi=1; fi+1<ct.num_verts; ++fi)
      {
        const motion_detail::clip_vertex &a=ct.verts[0];
        const motion_detail::clip_vertex &b=ct.verts[fi];
        const motion_detail::clip_vertex &c=ct.verts[fi+1];
        if(m_stats) ++m_stats->num_fan_pieces;

        // project the fan piece to sub-pixel integer coordinates exactly like
        // the static rasterizer does
        vec2i32 v0={int32_t((1.0f+a.pos.x/a.pos.w)*scale_x)-rasterizer_half_subpixel_offsx,
                    int32_t((1.0f-a.pos.y/a.pos.w)*scale_y)-rasterizer_half_subpixel_offsy};
        vec2i32 v1={int32_t((1.0f+b.pos.x/b.pos.w)*scale_x)-rasterizer_half_subpixel_offsx,
                    int32_t((1.0f-b.pos.y/b.pos.w)*scale_y)-rasterizer_half_subpixel_offsy};
        vec2i32 v2={int32_t((1.0f+c.pos.x/c.pos.w)*scale_x)-rasterizer_half_subpixel_offsx,
                    int32_t((1.0f-c.pos.y/c.pos.w)*scale_y)-rasterizer_half_subpixel_offsy};

        // back-face culling uses the current row orientation
        int32_t signed_area=(v1.x-v0.x)*(v2.y-v0.y)-(v1.y-v0.y)*(v2.x-v0.x);
        enum {tbfc=Base::tbfc_mode};
        if(tbfc!=rstate_tbfc_none)
        {
          int32_t cw_area=signed_area;
          if(tbfc==rstate_tbfc_cw)
            cw_area=-cw_area;
          if(cw_area<=0)
            continue;
        }
        if(signed_area==0)
          continue;

        // pixel bounding rectangle clamped to this tile
        minmax_res<int32_t> mmx=minmax(v0.x, v1.x, v2.x);
        minmax_res<int32_t> mmy=minmax(v0.y, v1.y, v2.y);
        vec2u16 brect_min={(uint16_t)max<int32_t>(ctx_.tile_x, (mmx.min+rasterizer_subpixel_bitmask)>>rasterizer_subpixel_bits),
                           (uint16_t)max<int32_t>(ctx_.tile_y, (mmy.min+rasterizer_subpixel_bitmask)>>rasterizer_subpixel_bits)};
        vec2u16 brect_end={(uint16_t)min<int32_t>(x_hi, (mmx.max>>rasterizer_subpixel_bits)+1),
                           (uint16_t)min<int32_t>(int(ctx_.tile_y)+ctx_.tile_height, (mmy.max>>rasterizer_subpixel_bits)+1)};
        if(int16_t(brect_end.x-brect_min.x)<=0 || int16_t(brect_end.y-brect_min.y)<=0)
          continue;
        if(row_<brect_min.y || row_>=brect_end.y)
          continue;

        // edge functions with the same half-open (top/left) rule as the
        // static rasterizer, so shared edges are shaded exactly once
        vec2i32 p={int32_t(brect_min.x)<<rasterizer_subpixel_bits, int32_t(brect_min.y)<<rasterizer_subpixel_bits};
        vec3i32 w_row={(v1.x-p.x)*(v2.y-p.y)-(v1.y-p.y)*(v2.x-p.x),
                       (v2.x-p.x)*(v0.y-p.y)-(v2.y-p.y)*(v0.x-p.x),
                       (v0.x-p.x)*(v1.y-p.y)-(v0.y-p.y)*(v1.x-p.x)};
        vec3i32 dwdx={v1.y-v2.y, v2.y-v0.y, v0.y-v1.y};
        vec3i32 dwdy={v2.x-v1.x, v0.x-v2.x, v1.x-v0.x};
        if(signed_area<0)
        {
          w_row.x=-w_row.x; w_row.y=-w_row.y; w_row.z=-w_row.z;
          dwdx.x=-dwdx.x; dwdx.y=-dwdx.y; dwdx.z=-dwdx.z;
          dwdy.x=-dwdy.x; dwdy.y=-dwdy.y; dwdy.z=-dwdy.z;
        }
        if(dwdx.x<0 || (!dwdx.x && dwdy.x>0)) --w_row.x;
        if(dwdx.y<0 || (!dwdx.y && dwdy.y>0)) --w_row.y;
        if(dwdx.z<0 || (!dwdx.z && dwdy.z>0)) --w_row.z;
        w_row.x>>=rasterizer_subpixel_bits;
        w_row.y>>=rasterizer_subpixel_bits;
        w_row.z>>=rasterizer_subpixel_bits;

        // advance the edge functions to the single scanline of this row
        uint16_t ly=row_-brect_min.y;
        w_row.x+=dwdy.x*ly;
        w_row.y+=dwdy.y*ly;
        w_row.z+=dwdy.z*ly;

        const motion_detail::clip_vertex *fv[3]={&a, &b, &c};
        const uint8_t *pidx=m_data->tri_indices+ti*3;
        for(uint16_t px=brect_min.x; px<brect_end.x; ++px)
        {
          vec3i32 w=w_row;
          if(int32_t((w.x|w.y|w.z))>=0)
          {
            int32_t wsum=w.x+w.y+w.z;
            if(wsum>0)
            {
              float inv_sum=1.0f/float(wsum);
              float lam[3]={float(w.x)*inv_sum, float(w.y)*inv_sum, float(w.z)*inv_sum};

              // depth is affine screen-space interpolation of NDC z (z/w)
              float z=lam[0]*a.depth+lam[1]*b.depth+lam[2]*c.depth;
              uint32_t local_offs=uint32_t((px-ctx_.tile_x)+(row_-ctx_.tile_y)*pitch);
              if(motion_depth_pass_(z, local_offs))
              {
                // map the fan barycentrics back to the original triangle via
                // the per-vertex 1/w and original-triangle weights
                float persp[3]={lam[0]*a.invw, lam[1]*b.invw, lam[2]*c.invw};
                float persp_sum=persp[0]+persp[1]+persp[2];
                if(persp_sum>0.0f)
                {
                  float orig_bc[3]={0.0f, 0.0f, 0.0f};
                  for(unsigned k=0; k<3; ++k)
                    for(unsigned o=0; o<3; ++o)
                      orig_bc[o]+=persp[k]*fv[k]->bc[o];
                  float inv=1.0f/persp_sum;
                  vec3f bc(orig_bc[0]*inv, orig_bc[1]*inv, orig_bc[2]*inv);

                  // original-triangle vertices at this row time for the
                  // real shade_pixel callback
                  ptv_fmt pv[3];
                  float t=row_time_(row_, rt_h_);
                  for(unsigned k=0; k<3; ++k)
                  {
                    const motion_detail::cached_vertex &cv=m_cache.verts[pidx[k]];
                    pv[k].pos=cv.pos0+(cv.pos1-cv.pos0)*t;
                    pv[k].uv=cv.uv;
                    pv[k].color=cv.color;
                  }
                  access_t::set_px_offs(psout, local_offs);
                  // dispatch through the concrete PSC so derived shade_pixel
                  // overrides are selected (the method is non-virtual)
                  static_cast<const concrete_psc_t*>(this)->
                    shade_pixel(psout, pso_state(), pv[0], pv[1], pv[2], bc, ti);
                  if(m_stats) ++m_stats->num_shaded_pixels;
                  if(m_depth_capture)
                    m_depth_capture[px+row_*m_capture_width]=z;
                }
              }
            }
          }
          w_row.x+=dwdx.x;
          w_row.y+=dwdx.y;
          w_row.z+=dwdx.z;
        }

        // mark the touched tile region so the tile is submitted and cleared
        int lx0=max(0, int(brect_min.x)-ctx_.tile_x);
        int lx1=min<int>(ctx_.tile_width, int(brect_end.x)-ctx_.tile_x);
        if(lx1>lx0)
        {
          uint16_t ly0=uint16_t(row_-ctx_.tile_y);
          ctx_.tile_min->x=uint16_t(min<int>(ctx_.tile_min->x, lx0));
          ctx_.tile_min->y=uint16_t(min<int>(ctx_.tile_min->y, ly0));
          ctx_.tile_end->x=uint16_t(max<int>(ctx_.tile_end->x, lx1));
          ctx_.tile_end->y=uint16_t(max<int>(ctx_.tile_end->y, ly0+1));
        }
      }
    }
  }

  // per-pixel depth test/write against the tile depth buffer in the PSC's
  // configured depth format
  EGL_INLINE bool motion_depth_pass_(float z_, uint32_t local_offs_) const
  {
    if(Base::depth_format==int(depthfmt_none))
      return true;
    uint8_t *db=(uint8_t*)m_depth_buf;
    float ref;
    if(Base::depth_format==int(depthfmt_float32))
      ref=((const float*)db)[local_offs_];
    else if(Base::depth_format==int(depthfmt_uint16))
      ref=((const uint16_t*)db)[local_offs_]/65535.0f;
    else
      ref=((const uint8_t*)db)[local_offs_]/255.0f;
    bool pass;
    switch((e_rstate_depth_func)Base::depth_func)
    {
      case rstate_depthfunc_always: pass=true; break;
      case rstate_depthfunc_never: pass=false; break;
      case rstate_depthfunc_equal: pass=z_==ref; break;
      case rstate_depthfunc_not_equal: pass=z_!=ref; break;
      case rstate_depthfunc_less: pass=z_<ref; break;
      case rstate_depthfunc_less_equal: pass=z_<=ref; break;
      case rstate_depthfunc_greater: pass=z_>ref; break;
      case rstate_depthfunc_greater_equal: pass=z_>=ref; break;
      default: pass=true; break;
    }
    if(!pass)
      return false;
    if(Base::depth_write)
    {
      if(Base::depth_format==int(depthfmt_float32))
        ((float*)db)[local_offs_]=z_;
      else if(Base::depth_format==int(depthfmt_uint16))
        ((uint16_t*)db)[local_offs_]=uint16_t(z_*65535.0f);
      else
        ((uint8_t*)db)[local_offs_]=uint8_t(z_*255.0f);
    }
    return true;
  }

  const motion_cluster_data *m_data;
  motion_stats *m_stats;
  mutable row_cache m_cache;
  mutable void *m_depth_buf;
  mutable float *m_depth_capture;
  uint16_t m_capture_width, m_capture_height;
};
//----------------------------------------------------------------------------


//============================================================================
EGL_NAMESPACE_END
//============================================================================
#endif // __cplusplus
#endif // EGL_MOTION_H
