#include "caption/caption.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void send(caption_frame_t *f, uint16_t w) { caption_frame_decode(f,w,1.0); }
int main(void) {
    caption_frame_t f; caption_frame_init(&f);
    send(&f,eia608_control_command(eia608_control_resume_direct_captioning,0));
    send(&f,eia608_row_column_pramble(10,0,0,0));
    send(&f,eia608_from_utf8_2("Z","Z"));
    send(&f,eia608_control_command(eia608_control_roll_up_2,0));
    send(&f,eia608_row_style_pramble(4,0,eia608_style_green,1));
    send(&f,eia608_from_utf8_2("A","B"));
    send(&f,eia608_control_command(eia608_control_carriage_return,0));
    assert(!strcmp(caption_frame_read_char(&f,3,0,0,0),"A"));
    assert(!strcmp(caption_frame_read_char(&f,10,0,0,0),"Z"));
    assert(!strcmp(caption_frame_read_char(&f,10,1,0,0),"Z"));
    printf("baseline: AB rolled up to row3; ZZ outside the window stays on row10\n");
}
