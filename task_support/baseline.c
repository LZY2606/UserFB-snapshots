#include "vterm.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
static int oscs;
static int osc(int command,VTermStringFragment f,void *p){(void)f;(void)p;if(command==8)++oscs;return 1;}
int main(void){
 VTerm *v=vterm_new(3,20);assert(v);vterm_set_utf8(v,1);
 VTermScreen *s=vterm_obtain_screen(v);vterm_screen_reset(s,1);
 VTermStateFallbacks fb={.osc=osc};vterm_state_set_unrecognised_fallbacks(vterm_obtain_state(v),&fb,NULL);
 const char *str="\033]8;id=a;https://example.invalid/a\033\\AB\033]8;;\033\\C";
 for(size_t i=0;i<strlen(str);i++)assert(vterm_input_write(v,str+i,1)==1);
 VTermScreenCell cell;assert(vterm_screen_get_cell(s,(VTermPos){0,0},&cell)&&cell.chars[0]=='A');
 assert(vterm_screen_get_cell(s,(VTermPos){0,2},&cell)&&cell.chars[0]=='C');
 if(oscs>0)puts("BASELINE_OSC8_UNHANDLED");else puts("BASELINE_OSC8_HANDLED");puts("BASELINE_TEXT_ABC_OK");vterm_free(v);return 0;
}
