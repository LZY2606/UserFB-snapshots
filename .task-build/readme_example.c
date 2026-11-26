#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libmpegts.h"

int main(void)
{
    /* program 1: MPEG-2 video + MPEG-1 audio, independent PCR PID */
    ts_stream_t s1[2] = {
        { .pid = 0x0100, .stream_format = LIBMPEGTS_VIDEO_MPEG2,
          .stream_id = LIBMPEGTS_STREAM_ID_MPEGVIDEO },
        { .pid = 0x0101, .stream_format = LIBMPEGTS_AUDIO_MPEG1,
          .stream_id = LIBMPEGTS_STREAM_ID_MPEGAUDIO,
          .audio_frame_size = 216 },
    };
    ts_program_t p1 = { .pmt_pid = 0x1000, .program_num = 1,
                        .pcr_pid = 0x0110, .num_streams = 2, .streams = s1 };

    /* program 2: AVC video with PCR carried on its elementary PID */
    ts_stream_t s2[1] = {
        { .pid = 0x0200, .stream_format = LIBMPEGTS_VIDEO_AVC,
          .stream_id = LIBMPEGTS_STREAM_ID_MPEGVIDEO },
    };
    ts_program_t p2 = { .pmt_pid = 0x1001, .program_num = 2,
                        .pcr_pid = 0x0200, .num_streams = 1, .streams = s2 };

    ts_program_t programs[2] = { p1, p2 };
    ts_main_t m = { .num_programs = 2, .programs = programs, .ts_id = 1,
                    .muxrate = 4000000, .cbr = 1, .lowlatency = 1,
                    .ts_type = TS_TYPE_GENERIC,
                    .pcr_period = 20, .pat_period = 80 };

    ts_writer_t *w = ts_create_writer();
    if( ts_setup_transport_stream( w, &m ) < 0 )
        return 1;
    ts_setup_mpegvideo_stream( w, 0x0100, LIBMPEGTS_MPEG2_LEVEL_MAIN,
                               LIBMPEGTS_MPEG2_PROFILE_MAIN, 2000000, 200000, 3 );
    ts_setup_mpegvideo_stream( w, 0x0200, 30, AVC_HIGH,
                               2000000, 200000, 1 );

    FILE *outfile = fopen( "mpts.ts", "wb" );
    uint8_t body[640];
    uint8_t *out = NULL;
    int len = 0;
    int64_t *pcr_list = NULL;

    for( int i = 0; i < 8; i++ )
    {
        for( size_t j = 0; j < sizeof(body); j++ )
            body[j] = (uint8_t)( j * 17 + i * 31 );

        ts_frame_t f[2];
        memset( f, 0, sizeof(f) );
        f[0].pid = 0x0100; f[0].data = body; f[0].size = sizeof(body);
        f[0].dts = f[0].pts = 9000 + i * 3600;
        f[0].cpb_initial_arrival_time = (int64_t)( i * 3600 + 900 ) * 300;
        f[0].cpb_final_arrival_time   = (int64_t)( i * 3600 + 3600 ) * 300;
        f[0].random_access = ( i == 0 );

        f[1].pid = 0x0200; f[1].data = body; f[1].size = 500;
        f[1].dts = f[1].pts = 9000 + i * 3600 + 1800;
        f[1].cpb_initial_arrival_time = (int64_t)( i * 3600 + 1200 ) * 300;
        f[1].cpb_final_arrival_time   = (int64_t)( i * 3600 + 3600 ) * 300;
        f[1].random_access = ( i == 0 );

        /* frames from both programs may be interleaved in one call */
        if( ts_write_frames( w, f, 2, &out, &len, &pcr_list ) == 0 )
            fwrite( out, 1, len, outfile );
    }

    /* num_frames == 0 drains queued PES data; pcr_list has len/188 entries */
    if( ts_write_frames( w, NULL, 0, &out, &len, &pcr_list ) == 0 )
        fwrite( out, 1, len, outfile );

    fclose( outfile );
    ts_close_writer( w );
    return 0;
}

