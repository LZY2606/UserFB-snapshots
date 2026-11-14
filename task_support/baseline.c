#include "miniaudio.h"
#include <math.h>
#include <stdio.h>

int main(void)
{
    ma_engine engine;
    ma_engine_config ec = ma_engine_config_init();
    ma_audio_buffer_ref source;
    ma_sound sound;
    float input[64], output[24];
    ma_uint64 read = 0;
    unsigned n;
    ec.noDevice = MA_TRUE;
    ec.channels = 2;
    ec.sampleRate = 48000;
    ec.periodSizeInFrames = 0;
    ec.defaultVolumeSmoothTimeInPCMFrames = 0;
    for (n = 0; n < 32; ++n) {
        input[2*n] = (float)(n+1)/64;
        input[2*n+1] = -(float)(n+1)/128;
    }
    if (ma_engine_init(&ec, &engine) != MA_SUCCESS) return 1;
    if (ma_audio_buffer_ref_init(ma_format_f32, 2, input, 32, &source) != MA_SUCCESS) return 2;
    if (ma_data_source_set_range_in_pcm_frames(&source, 2, 10) != MA_SUCCESS) return 3;
    if (ma_sound_init_from_data_source(&engine, &source, MA_SOUND_FLAG_NO_SPATIALIZATION | MA_SOUND_FLAG_NO_PITCH, NULL, &sound) != MA_SUCCESS) return 4;
    ma_sound_set_looping(&sound, MA_TRUE);
    if (ma_sound_start(&sound) != MA_SUCCESS) return 5;
    if (ma_engine_read_pcm_frames(&engine, output, 12, &read) != MA_SUCCESS || read != 12) return 6;
    for (n = 0; n < 12; ++n) {
        unsigned at = 2 + n % 8;
        if (fabsf(output[2*n] - input[2*at]) > 1e-6f || fabsf(output[2*n+1] - input[2*at+1]) > 1e-6f) {
            fprintf(stderr,"frame=%u got=%g/%g expected=%g/%g\n",n,output[2*n],output[2*n+1],input[2*at],input[2*at+1]);
            return 7;
        }
    }
    ma_sound_uninit(&sound);
    ma_audio_buffer_ref_uninit(&source);
    ma_engine_uninit(&engine);
    printf("BASELINE_SOUND_OK stereo_frames=12 range=2..10 loop=8\n");
    return 0;
}
