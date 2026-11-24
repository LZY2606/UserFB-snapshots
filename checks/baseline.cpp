// License: HarfBuzz Old MIT; see COPYING.
// Existing-path smoke witness; this is not the independent CFF2 geometry oracle.
#include "hb.h"
#include "hb-ot.h"
#include "hb-subset.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cmath>
#include <algorithm>
struct Counts { unsigned moves=0, lines=0, curves=0, closes=0; std::vector<float> points; };
static void fail(const char* s) { std::fprintf(stderr,"BASELINE_FAIL %s\n",s); std::exit(1); }
static void move(hb_draw_funcs_t*,void* p,hb_draw_state_t*,float x,float y,void*) { Counts& c=*(Counts*)p; ++c.moves; c.points.push_back(x); c.points.push_back(y); }
static void line(hb_draw_funcs_t*,void* p,hb_draw_state_t*,float x,float y,void*) { Counts& c=*(Counts*)p; ++c.lines; c.points.push_back(x); c.points.push_back(y); }
static void cubic(hb_draw_funcs_t*,void* p,hb_draw_state_t*,float x1,float y1,float x2,float y2,float x,float y,void*) { Counts& c=*(Counts*)p; ++c.curves; for(float v:{x1,y1,x2,y2,x,y}) c.points.push_back(v); }
static void close(hb_draw_funcs_t*,void* p,hb_draw_state_t*,void*) { ++((Counts*)p)->closes; }
static Counts draw(hb_face_t* face) {
  hb_font_t* font=hb_font_create(face);
  hb_variation_t vars[]={{HB_TAG('w','g','h','t'),500},{HB_TAG('C','N','T','R'),50}};
  hb_font_set_variations(font,vars,2);
  hb_draw_funcs_t* f=hb_draw_funcs_create();
  hb_draw_funcs_set_move_to_func(f,move,nullptr,nullptr);
  hb_draw_funcs_set_line_to_func(f,line,nullptr,nullptr);
  hb_draw_funcs_set_cubic_to_func(f,cubic,nullptr,nullptr);
  hb_draw_funcs_set_close_path_func(f,close,nullptr,nullptr);
  Counts c;
  for (unsigned cp='a';cp<='c';++cp) {
    hb_codepoint_t gid;
    if (!hb_font_get_nominal_glyph(font,cp,&gid)) fail("cmap");
    hb_font_draw_glyph(font,gid,f,&c);
    hb_glyph_extents_t e;
    if (!hb_font_get_glyph_extents(font,gid,&e) || e.width<=0 || e.height>=0) fail("extents");
  }
  if (!c.moves || !c.curves || c.moves!=c.closes) fail("outline callbacks");
  hb_draw_funcs_destroy(f); hb_font_destroy(font); return c;
}
int main() {
  hb_blob_t* blob=hb_blob_create_from_file_or_fail("test/api/fonts/AdobeVFPrototype.abc.otf");
  if (!blob) fail("font bytes");
  hb_face_t* face=hb_face_create(blob,0); hb_blob_destroy(blob);
  if (hb_ot_var_get_axis_count(face)!=2) fail("two source axes");
  Counts c=draw(face);
  std::printf("CFF2_SOURCE_DRAW_OK moves=%u curves=%u\n",c.moves,c.curves);
  for (unsigned full=0;full<2;++full) {
    hb_subset_input_t* in=hb_subset_input_create_or_fail();
    hb_set_add_range(hb_subset_input_unicode_set(in),'a','c');
    if (!hb_subset_input_pin_axis_location(in,face,HB_TAG('w','g','h','t'),500)) fail("pin weight");
    if (full && !hb_subset_input_pin_axis_location(in,face,HB_TAG('C','N','T','R'),50)) fail("pin contrast");
    hb_face_t* result=hb_subset_or_fail(face,in); hb_subset_input_destroy(in);
    if (!result || hb_ot_var_get_axis_count(result)!=(full?0:1)) fail("subset axes");
    Counts d=draw(result);
    if (d.points.size()!=c.points.size()) fail("trace size");
    double max_error=0;
    for (unsigned j=0;j<c.points.size();++j) max_error=std::max(max_error,std::abs(double(c.points[j])-d.points[j]));
    if (max_error>3.0) { std::fprintf(stderr,"BASELINE_SEMANTIC_MISMATCH max_error=%.4f limit=3\n",max_error); return 1; }
    std::printf("CFF2_EXISTING_TRACE_OK mode=%u max_error=%.4f\n",full,max_error);
    std::printf("CFF2_%s_INSTANCE_OK axes=%u moves=%u curves=%u\n",full?"FULL":"PARTIAL",hb_ot_var_get_axis_count(result),d.moves,d.curves);
    hb_face_destroy(result);
  }
  hb_face_destroy(face);
  std::puts("CFF2_BASELINE_OK");
}
