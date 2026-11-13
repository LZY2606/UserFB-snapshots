#include <lc3.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
int main(void) {
 const int rates[]={8000,16000,24000,32000,48000};
 unsigned frames=0; double energy=0;
 for(int d=0;d<2;d++)for(int r=0;r<5;r++) {
  int dt=d?10000:7500, rate=rates[r], n=lc3_frame_samples(dt,rate);
  void* em=calloc(1,lc3_encoder_size(dt,rate));void* dm=calloc(1,lc3_decoder_size(dt,rate));
  lc3_encoder_t e=lc3_setup_encoder(dt,rate,0,em);lc3_decoder_t dec=lc3_setup_decoder(dt,rate,0,dm);
  if(!e||!dec)return 2;
  int16_t in[480];float out[960];uint8_t packet[100];
  for(int f=0;f<8;f++) {
   for(int i=0;i<n;i++)in[i]=(int16_t)(11000*sin((f*n+i)*0.073)+3000*cos((f*n+i)*0.191));
   if(lc3_encode(e,LC3_PCM_FORMAT_S16,in,1,100,packet))return 3;
   for(int i=0;i<2*n;i++)out[i]=123.0f;
   int loss=f==3;int ret=lc3_decode(dec,loss?NULL:packet,100,LC3_PCM_FORMAT_FLOAT,out,2);
   if(ret!=loss)return 4;
   for(int i=0;i<n;i++){if(!isfinite(out[2*i])||out[2*i+1]!=123.0f)return 5;energy+=out[2*i]*out[2*i];}
   frames++;
  }
  free(em);free(dm);
 }
 if(energy<1)return 6;
 printf("LC3_BASELINE configurations=10 frames=%u loss_frames=10 energy=%.6f\n",frames,energy);
}
