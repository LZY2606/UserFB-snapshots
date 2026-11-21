#ifndef TASK_REFERENCE_H
#define TASK_REFERENCE_H
#include <stdint.h>
typedef struct {int frame_bytes,frame_offset,channels,hz,layer,bitrate_kbps;} reference_mp3dec_frame_info_t;
typedef struct {float mdct_overlap[2][9*32],qmf_state[15*2*32];int reserv,free_format_bytes;unsigned char header[4],reserv_buf[511];} reference_mp3dec_t;
void reference_mp3dec_init(reference_mp3dec_t*);
int reference_mp3dec_decode_frame(reference_mp3dec_t*,const uint8_t*,int,int16_t*,reference_mp3dec_frame_info_t*);
#endif
