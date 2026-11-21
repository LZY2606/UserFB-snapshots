/* Unmodified pinned decoder, symbol names separated for independent test reference. */
#ifdef MINIMP3_CHECK_CRC
#undef MINIMP3_CHECK_CRC
#endif
#define MINIMP3_IMPLEMENTATION
#define mp3dec_t reference_mp3dec_t
#define mp3dec_frame_info_t reference_mp3dec_frame_info_t
#define mp3dec_init reference_mp3dec_init
#define mp3dec_decode_frame reference_mp3dec_decode_frame
#include "reference/minimp3.h"
