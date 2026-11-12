#include <stdio.h>
#include <stdlib.h>
#include "stream.h"
#include "frame.h"
#include "synth.h"
#include "fixture.h"
static int decode(int mixed,int *pcm){
 unsigned char data[6*144+8];unsigned n=make_stream(data,mixed),frames=0,used=0;
 struct mad_stream s;struct mad_frame f;struct mad_synth sy;
 mad_stream_init(&s);mad_frame_init(&f);mad_synth_init(&sy);mad_stream_buffer(&s,data,n);
 while(frames<6){if(mad_frame_decode(&f,&s)){fprintf(stderr,"DECODE mixed=%d frame=%u error=%x\n",mixed,frames,s.error);return -1;}
  mad_synth_frame(&sy,&f);if(sy.pcm.length!=576||sy.pcm.samplerate!=8000)return -1;
  for(unsigned i=0;i<sy.pcm.length;i++)pcm[used++]=sy.pcm.samples[0][i];frames++;
 }
 mad_frame_finish(&f);mad_stream_finish(&s);return used;
}
int main(void){
 int a[3456],b[3456];int na=decode(0,a),nb=decode(1,b);if(na!=3456||nb!=3456)return 2;
 unsigned diff=0;for(int i=0;i<na;i++)diff+=a[i]!=b[i];
 printf("MAD_8K_BASE frames=12 samples=6912 differing=%u\n",diff);return 0;
}
