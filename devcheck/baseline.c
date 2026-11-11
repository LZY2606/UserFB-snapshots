#include "plutovg.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
static unsigned alpha(plutovg_surface_t*s,int x,int y){uint32_t*p=(uint32_t*)(plutovg_surface_get_data(s)+y*plutovg_surface_get_stride(s));return p[x]>>24;}
static plutovg_canvas_t* fresh(plutovg_surface_t**s){*s=plutovg_surface_create(32,32);plutovg_canvas_t*c=plutovg_canvas_create(*s);plutovg_canvas_set_rgb(c,1,1,1);return c;}
static void done(plutovg_canvas_t*c,plutovg_surface_t*s){plutovg_canvas_destroy(c);plutovg_surface_destroy(s);}
int main(void){
 plutovg_surface_t*s;plutovg_canvas_t*c=fresh(&s);
 plutovg_canvas_rect(c,2,2,24,24);plutovg_canvas_rect(c,8,8,10,10);plutovg_canvas_set_fill_rule(c,PLUTOVG_FILL_RULE_EVEN_ODD);plutovg_canvas_fill_preserve(c);
 assert(alpha(s,12,12)==0&&alpha(s,4,4)==255);plutovg_canvas_set_fill_rule(c,PLUTOVG_FILL_RULE_NON_ZERO);plutovg_canvas_fill(c);assert(alpha(s,12,12)==255);done(c,s);
 c=fresh(&s);plutovg_canvas_move_to(c,2,2);plutovg_canvas_cubic_to(c,2,14,14,14,14,2);plutovg_canvas_close_path(c);plutovg_canvas_fill(c);
 assert(alpha(s,8,7)==255&&alpha(s,8,13)==0);done(c,s);
 c=fresh(&s);plutovg_canvas_clip_rect(c,0,0,16,32);plutovg_canvas_clip_rect(c,8,0,16,32);plutovg_canvas_paint(c);
 assert(alpha(s,4,12)==0&&alpha(s,12,12)==255&&alpha(s,20,12)==0);done(c,s);
 puts("PLUTOVG_RULE_CURVE_CLIP_BASELINE_OK");return 0;
}
