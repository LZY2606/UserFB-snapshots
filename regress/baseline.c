#include <plutovg.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
int main(void) {
    plutovg_surface_t *s=plutovg_surface_create(64,48);
    plutovg_canvas_t *c=plutovg_canvas_create(s);
    plutovg_canvas_set_rgb(c,1,0,0);
    plutovg_canvas_set_line_width(c,4);
    plutovg_canvas_scale(c,3,1);
    plutovg_canvas_move_to(c,4,20);
    plutovg_canvas_line_to(c,16,20);
    plutovg_rect_t r;
    plutovg_canvas_stroke_extents(c,&r);
    assert(r.x==12 && r.w==36 && r.y<18 && r.h>4);
    plutovg_canvas_stroke_preserve(c);
    int stride=plutovg_surface_get_stride(s);
    unsigned char *data=plutovg_surface_get_data(s);
    assert(((uint32_t*)(data+20*stride))[20]>>24);
    printf("baseline legacy stroke extents %.0f %.0f %.0f %.0f; center covered\n",r.x,r.y,r.w,r.h);
    plutovg_canvas_destroy(c); plutovg_surface_destroy(s);
}
