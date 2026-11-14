#include <assert.h>
#define TSF_IMPLEMENTATION
#include "tsf.h"
#include "font_fixture.h"
#include <stdio.h>
#include <math.h>
int main(void){
 tsf*f=make_font(1,0);assert(f&&tsf_get_presetcount(f)==2);tsf_set_output(f,TSF_STEREO_INTERLEAVED,22050,0);assert(tsf_note_on(f,0,60,1));
 float pcm[1024];tsf_render_float(f,pcm,512,0);double energy=0;for(int i=0;i<1024;i++){assert(isfinite(pcm[i]));energy+=pcm[i]*pcm[i];}assert(energy>0.001);
 tsf_note_off(f,0,60);for(int i=0;i<40;i++)tsf_render_float(f,pcm,512,0);assert(tsf_active_voice_count(f)==0);tsf_close(f);printf("TSF_SEED_AUDIO_OK presets=2 energy=%.8f\n",energy);
}
