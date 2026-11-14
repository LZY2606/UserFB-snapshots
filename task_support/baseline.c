#include "caption/vtt.h"
#include "caption/caption.h"
#include <stdio.h>
#include <string.h>
static void line_text(char* s){for(;*s;++s)if(*s=='\r'||*s=='\n')*s='|';}
int main(void){
 caption_frame_t f;char text[CAPTION_FRAME_TEXT_BYTES];
 caption_frame_from_text(&f,"Hello");caption_frame_to_text(&f,text);
 if(strcmp(text,"Hello"))return 1;
 printf("BASELINE_NORMAL text=%s\n",text);
 const char* input="WEBVTT\n\n00:00:10.000 --> 00:00:14.000\n<i>A&amp;B</i><00:00:12.000><u>CD</u>\n\n";
 vtt_t* v=vtt_parse(input,strlen(input));
 if(!v||!v->cue_head)return 2;
 vtt_cue_to_caption_frame(v->cue_head,&f);caption_frame_to_text(&f,text);
 line_text(text);printf("BASELINE_FRAME text=%s\n",text);
 if(strstr(text,"00:00:12.000"))puts("BASELINE_RAW_TIMECODE_VISIBLE");
 else if(!strcmp(text,"A&BCD"))puts("BASELINE_TIMED_TEXT_INTERPRETED");
 else{vtt_free(v);return 3;}
 vtt_free(v);puts("BASELINE_OK");return 0;
}
