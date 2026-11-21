#include "caption/cea708.h"
#include <stdio.h>
#include <string.h>
int main(void) {
 caption_frame_t plain={0},frame={0}; cea708_t cc,wire; uint8_t data[512]; char text[4096];
 caption_frame_from_text(&plain,"Hello\nWorld");
 caption_frame_to_text(&plain,text);
 if(!strstr(text,"Hello")||!strstr(text,"World")) return 1;
 puts("TEXT_PROJECTION_OK");
 caption_frame_init(&frame); cea708_init(&cc,3.0);
 cea708_add_cc_data(&cc,1,cc_type_ntsc_cc_field_1,eia608_control_command(eia608_control_resume_caption_loading,0));
 cea708_add_cc_data(&cc,1,cc_type_ntsc_cc_field_1,eia608_from_utf8_2("A","A"));
 cea708_add_cc_data(&cc,1,cc_type_ntsc_cc_field_1,eia608_control_command(eia608_control_resume_caption_loading,1));
 cea708_add_cc_data(&cc,1,cc_type_ntsc_cc_field_1,eia608_from_utf8_2("B","B"));
 cea708_add_cc_data(&cc,1,cc_type_ntsc_cc_field_1,eia608_control_command(eia608_control_end_of_caption,1));
 int size=cea708_render(&cc,data,sizeof(data)); if(size<=0)return 2;
 cea708_init(&wire,3.0); if(LIBCAPTION_ERROR==cea708_parse_h264(data,size,&wire))return 3;
 if(LIBCAPTION_ERROR==cea708_to_caption_frame(&frame,&wire))return 4;
 caption_frame_to_text(&frame,text); if(!strstr(text,"AABB"))return 5;
 printf("LEGACY_SINGLE_FRAME_MERGES_CHANNELS=%s",text);
 puts("GA94_ROUNDTRIP_OK"); return 0;
}
