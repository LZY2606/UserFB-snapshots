#include <stdio.h>
#include <math.h>
#include <speex/speex.h>
int main(int argc, char **argv) {
 for(int mode=1;mode<=2;mode++) {
  const SpeexMode *m=speex_lib_get_mode(mode);
  void *enc=speex_encoder_init(m), *dec=speex_decoder_init(&speex_nb_mode);
  SpeexBits bits; speex_bits_init(&bits);
  spx_int16_t in[640],out[160];int size=0,quality=5,got=0;
  if(!enc||!dec)return 1;
  speex_encoder_ctl(enc,SPEEX_GET_FRAME_SIZE,&size);
  speex_encoder_ctl(enc,SPEEX_SET_QUALITY,&quality);
  for(int f=0;f<6;f++) {
   if(argc>2) { int qs[3]={1,3,8};quality=qs[f%3];speex_encoder_ctl(enc,SPEEX_SET_QUALITY,&quality); }
   for(int i=0;i<size;i++)in[i]=(spx_int16_t)(3000*sin((f*size+i)*0.031));
   if(argc>1) { speex_bits_pack(&bits,14,5);speex_bits_pack(&bits,10,4);speex_bits_pack(&bits,0x5aa5,16); }
   if(speex_encode_int(enc,in,&bits)<0)return 2;
  }
  speex_bits_rewind(&bits);
  for(int f=0;f<6;f++) {if(speex_decode_int(dec,&bits,out)!=0)return 3;got++;}
  if(got!=6)return 4;
  printf("SPEEX_BASELINE_OK mode=%d frames=%d remaining_bits=%d\n",mode,got,speex_bits_remaining(&bits));
  speex_bits_destroy(&bits);speex_encoder_destroy(enc);speex_decoder_destroy(dec);
 }
 return 0;
}
