#include "caption/caption.h"
#include "caption/mpeg.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 caption_frame_t a,b; sei_t s;
 caption_frame_init(&a); a.write=&a.front;
 assert(caption_frame_write_char(&a,14,0,eia608_style_white,0,"O"));
 assert(caption_frame_write_char(&a,14,1,eia608_style_white,0,"K"));
 caption_frame_init(&b); assert(sei_from_caption_frame(&s,&a)==LIBCAPTION_OK);
 assert(sei_to_caption_frame(&s,&b)==LIBCAPTION_READY);
 assert(!strcmp(caption_frame_read_char(&b,14,0,0,0),"O"));
 assert(!strcmp(caption_frame_read_char(&b,14,1,0,0),"K")); sei_free(&s);
 puts("PLAIN_POPON_PASS");
 caption_frame_init(&a); a.write=&a.front;
 caption_frame_write_char(&a,14,0,eia608_style_white,0,"A");
 caption_frame_write_char(&a,14,1,eia608_style_red,1,"B");
 caption_frame_write_char(&a,14,4,eia608_style_green,0,"C");
 caption_frame_init(&b); sei_from_caption_frame(&s,&a); sei_to_caption_frame(&s,&b);
 eia608_style_t st; int u; caption_frame_read_char(&b,14,0,&st,&u);
 printf("CURRENT_STYLE_A=%d underline=%d C='%s'\n",st,u,caption_frame_read_char(&b,14,4,0,0));

 sei_free(&s);
 puts("ENCODER_DIAGNOSTIC_DONE");
}
