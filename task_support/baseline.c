#include <webvtt/parser.h>
#include <stdio.h>
#include <string.h>
static webvtt_cue *saved; static unsigned cues, errors, stamps, styles; static char text[256];
static void oncue(void *unused,webvtt_cue *cue){(void)unused;saved=cue;++cues;webvtt_ref_cue(cue);}
static int onerror(void *unused,webvtt_uint line,webvtt_uint col,webvtt_error err){(void)unused;(void)line;(void)col;(void)err;++errors;return 0;}
static int walk(webvtt_node *n){
 if(!n)return 0;
 if(n->kind==WEBVTT_TEXT){const char *p=webvtt_string_text(&n->data.text);if(strlen(text)+strlen(p)>=sizeof(text))return 0;strcat(text,p);}
 else if(n->kind==WEBVTT_TIME_STAMP){if(n->data.timestamp!=12000)return 0;++stamps;}
 else if(WEBVTT_IS_VALID_INTERNAL_NODE(n->kind)){
  if(n->kind!=WEBVTT_HEAD_NODE)++styles;
  for(unsigned i=0;i<n->data.internal_data->length;++i){webvtt_node *c=n->data.internal_data->children[i];if(c->parent!=n||!walk(c))return 0;}
 }
 return 1;
}
int main(void){
 const char *vtt="WEBVTT\n\nid-7\n00:00:10.000 --> 00:00:14.000 align:start\n<v Alice><c.red>A&amp;<i>中😀</i></c><00:00:12.000>B</v>\n\n";
 webvtt_parser parser=0;if(webvtt_create_parser(oncue,onerror,0,&parser)!=WEBVTT_SUCCESS)return 1;
 for(size_t i=0;i<strlen(vtt);++i){if(WEBVTT_FAILED(webvtt_parse_chunk(parser,vtt+i,1)))return 2;}
 if(WEBVTT_FAILED(webvtt_finish_parsing(parser)))return 3;
 webvtt_delete_parser(parser);
 if(cues!=1||errors||!saved||saved->from!=10000||saved->until!=14000)return 4;
 if(!walk(saved->node_head)||strcmp(text,"A&中😀B")||stamps!=1||styles!=3)return 5;
 if(!strstr(webvtt_string_text(&saved->body),"&amp;"))return 6;
 printf("STREAM_CUE_OK id=%s from=10000 until=14000\n",webvtt_string_text(&saved->id));
 printf("VISIBLE_TEXT=%s\n",text);puts("TREE_OK styles=3 timestamps=1 parent_links=valid");
 puts("BODY_RETAINS_MARKUP_OK");webvtt_release_cue(&saved);return 0;
}
