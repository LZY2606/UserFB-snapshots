/* Multi-program transport stream cases for the supplied task runner.
 * All parsing, CRCs and clock math are implemented independently here;
 * no internal writer state is consulted for expectations. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "libmpegts.h"
#include "../task_support/check.h"

/* ---------------- independent CRC / time helpers ---------------- */

static uint32_t crc_mpeg( const uint8_t *s, size_t n )
{
    uint32_t c = ~0u;
    for( size_t i = 0; i < n; i++ )
    {
        c ^= (uint32_t)s[i] << 24;
        for( int j = 0; j < 8; j++ )
            c = ( c << 1 ) ^ ( ( c & 0x80000000u ) ? 0x04c11db7u : 0 );
    }
    return c;
}

static int pid_of( const uint8_t *p ) { return ( ( p[1] & 31 ) << 8 ) | p[2]; }
static int pusi_of( const uint8_t *p ) { return ( p[1] >> 6 ) & 1; }
static int afc_of( const uint8_t *p ) { return ( p[3] >> 4 ) & 3; }
static int cc_of( const uint8_t *p ) { return p[3] & 15; }

static int payload_pos( const uint8_t *p )
{
    int pos = 4;
    if( afc_of( p ) & 2 )
        pos = 5 + p[4];
    return pos;
}

static int pcr_decode( const uint8_t *p, uint64_t *v )
{
    if( !( afc_of( p ) & 2 ) || p[4] < 7 || !( p[5] & 0x10 ) )
        return 0;
    uint64_t base = ( (uint64_t)p[6] << 25 ) | ( (uint64_t)p[7] << 17 ) |
                    ( (uint64_t)p[8] << 9 ) | ( (uint64_t)p[9] << 1 ) | ( p[10] >> 7 );
    uint64_t ext = ( ( p[10] & 1 ) << 8 ) | p[11];
    *v = base * 300 + ext;
    return 1;
}

static uint64_t pts_stamp( const uint8_t *s )
{
    return ( (uint64_t)( s[0] & 14 ) << 29 ) | ( (uint64_t)s[1] << 22 ) |
           ( (uint64_t)( s[2] & 254 ) << 14 ) | ( (uint64_t)s[3] << 7 ) | ( s[4] >> 1 );
}

static int64_t round_time( int64_t numerator, int muxrate )
{
    return ( numerator + muxrate / 2 ) / muxrate;
}

#define TS_PCR_BYTE_OFFSET 10

/* ---------------- output sink ---------------- */

typedef struct
{
    ts_writer_t *w;
    uint8_t *buf;
    size_t len;
    int64_t *clock;
    size_t npk;
    int muxrate;
} sink_t;

static int sink_collect( sink_t *s, ts_frame_t *frames, int n )
{
    uint8_t *out = NULL;
    int len = 0;
    int64_t *cl = NULL;

    if( ts_write_frames( s->w, frames, n, &out, &len, &cl ) < 0 )
        return -1;
    if( len < 0 || len % 188 )
        return -2;
    if( len )
    {
        size_t add = (size_t)len / 188;
        uint8_t *nb = realloc( s->buf, s->len + len );
        int64_t *nc = realloc( s->clock, ( s->npk + add ) * sizeof(*nc) );
        if( !nb || !nc )
            return -3;
        s->buf = nb;
        s->clock = nc;
        memcpy( s->buf + s->len, out, len );
        memcpy( s->clock + s->npk, cl, add * sizeof(*cl) );
        s->len += len;
        s->npk += add;
    }
    return 0;
}

static void sink_done( sink_t *s )
{
    if( s->w )
        ts_close_writer( s->w );
    free( s->buf );
    free( s->clock );
    memset( s, 0, sizeof(*s) );
}

/* ---------------- section reassembly ---------------- */

typedef struct
{
    uint8_t *d;
    int len;
} section_t;

static int extract_sections( const uint8_t *ts, size_t npk, int wantpid,
                             section_t **sections, int *nsec )
{
    uint8_t *buf = NULL;
    int cap = 0, len = 0, need = -1;
    *sections = NULL;
    *nsec = 0;

    for( size_t k = 0; k < npk; k++ )
    {
        const uint8_t *p = ts + k * 188;
        if( pid_of( p ) != wantpid || !( afc_of( p ) & 1 ) )
            continue;

        int pos = payload_pos( p );

        if( pusi_of( p ) )
        {
            len = 0;
            need = -1;
            int pointer = p[pos];
            pos += 1 + pointer;
        }

        int avail = 188 - pos;
        if( avail <= 0 )
            continue;
        if( len + avail > cap )
        {
            cap = len + avail + 64;
            uint8_t *nb = realloc( buf, cap );
            if( !nb )
            {
                free( buf );
                return -1;
            }
            buf = nb;
        }
        memcpy( buf + len, p + pos, avail );
        len += avail;

        if( need < 0 && len >= 3 )
        {
            int slen = ( ( buf[1] & 15 ) << 8 ) | buf[2];
            if( slen > 1021 )
            {
                free( buf );
                return -2;
            }
            need = 3 + slen;
        }

        if( need > 0 && len >= need )
        {
            section_t *ns = realloc( *sections, ( *nsec + 1 ) * sizeof(*ns) );
            if( !ns )
            {
                free( buf );
                return -1;
            }
            *sections = ns;
            (*sections)[*nsec].d = malloc( need );
            if( !(*sections)[*nsec].d )
            {
                free( buf );
                return -1;
            }
            memcpy( (*sections)[*nsec].d, buf, need );
            (*sections)[*nsec].len = need;
            (*nsec)++;
            len = 0;
            need = -1;
        }
    }

    free( buf );
    return 0;
}

/* ---------------- PES reassembly ---------------- */

typedef struct
{
    uint8_t *es;
    int es_len;
    int64_t pts, dts;
    int first_pkt, last_pkt;
} pes_t;

static int store_one_pes( pes_t **pes_out, int *npes, const uint8_t *buf, int len,
                          int first_pkt, int last_pkt )
{
    if( len <= 9 )
        return 0;
    int hdl = 9 + buf[8];
    if( hdl > len )
        return 0;

    pes_t *np = realloc( *pes_out, ( *npes + 1 ) * sizeof(*np) );
    if( !np )
        return -1;
    *pes_out = np;
    pes_t *one = &(*pes_out)[*npes];
    memset( one, 0, sizeof(*one) );
    one->es_len = len - hdl;
    one->es = malloc( one->es_len > 0 ? one->es_len : 1 );
    if( !one->es )
        return -1;
    memcpy( one->es, buf + hdl, one->es_len );
    one->first_pkt = first_pkt;
    one->last_pkt = last_pkt;
    one->pts = one->dts = -1;
    if( ( buf[7] & 0x80 ) && hdl >= 14 )
    {
        one->pts = pts_stamp( buf + 9 );
        if( ( buf[7] & 0xc0 ) == 0xc0 )
            one->dts = pts_stamp( buf + 14 );
        else
            one->dts = one->pts;
    }
    (*npes)++;
    return 0;
}

static int extract_pes( const uint8_t *ts, size_t npk, int wantpid,
                        pes_t **pes_out, int *npes )
{
    uint8_t *buf = NULL;
    int cap = 0, len = 0;
    int first_pkt = -1, last_pkt = -1;
    *pes_out = NULL;
    *npes = 0;

    for( size_t k = 0; k < npk; k++ )
    {
        const uint8_t *p = ts + k * 188;

        if( pid_of( p ) == wantpid && pusi_of( p ) && ( afc_of( p ) & 1 ) )
        {
            if( len > 0 && first_pkt >= 0 &&
                store_one_pes( pes_out, npes, buf, len, first_pkt, last_pkt ) < 0 )
            {
                free( buf );
                return -1;
            }
            len = 0;
            first_pkt = (int)k;
        }

        if( pid_of( p ) != wantpid || !( afc_of( p ) & 1 ) )
            continue;

        int pos = payload_pos( p );
        int avail = 188 - pos;
        if( avail <= 0 )
            continue;

        if( len + avail > cap )
        {
            cap = len + avail + 256;
            uint8_t *nb = realloc( buf, cap );
            if( !nb )
            {
                free( buf );
                return -1;
            }
            buf = nb;
        }
        memcpy( buf + len, p + pos, avail );
        len += avail;
        last_pkt = (int)k;
    }

    if( len > 0 && first_pkt >= 0 &&
        store_one_pes( pes_out, npes, buf, len, first_pkt, last_pkt ) < 0 )
    {
        free( buf );
        return -1;
    }

    free( buf );
    return 0;
}

static void free_sections( section_t *s, int n )
{
    for( int i = 0; i < n; i++ )
        free( s[i].d );
    free( s );
}

static void free_pes( pes_t *p, int n )
{
    for( int i = 0; i < n; i++ )
        free( p[i].es );
    free( p );
}

/* payload continuity counters must advance exactly on payload packets */
static int check_continuity( const uint8_t *ts, size_t npk )
{
    int have[8192];
    int last[8192];
    memset( have, 0, sizeof(have) );
    memset( last, 0, sizeof(last) );

    for( size_t k = 0; k < npk; k++ )
    {
        const uint8_t *p = ts + k * 188;
        int pid = pid_of( p );
        if( pid == 0x1fff )
            continue;
        int a = afc_of( p );
        int cc = cc_of( p );

        if( a & 1 )
        {
            if( have[pid] && cc != ( ( last[pid] + 1 ) & 15 ) )
                return (int)k + 1;
            last[pid] = cc;
            have[pid] = 1;
        }
        else if( a == 2 && have[pid] )
        {
            /* adaptation-only repeats the most recent payload counter */
            if( cc != last[pid] )
                return -( (int)k + 1 );
        }
    }
    return 0;
}

/* every pcr_list entry is the unwrapped end-of-slot common clock */
static int check_common_clock( const sink_t *s )
{
    int mux = s->muxrate;
    for( size_t k = 0; k < s->npk; k++ )
    {
        int64_t expect = 270000000LL +
            round_time( (int64_t)( k + 1 ) * 188 * 8 * 27000000LL, mux );
        int64_t d = s->clock[k] - expect;
        if( d < -2 || d > 2 )
            return (int)k + 1;

        uint64_t pcr;
        if( pcr_decode( s->buf + k * 188, &pcr ) )
        {
            int64_t byte_time = 270000000LL +
                round_time( ( (int64_t)k * 188 + TS_PCR_BYTE_OFFSET ) * 8 * 27000000LL, mux );
            uint64_t mod = (uint64_t)1 << 33;
            uint64_t expect_mod = (uint64_t)byte_time % ( mod * 300 );
            uint64_t got_mod = pcr % ( mod * 300 );
            int64_t dd = (int64_t)got_mod - (int64_t)expect_mod;
            int64_t tol = s->muxrate < 100000 ? 2000 : 2;
            if( dd > 300 * 100000 )
                dd -= (int64_t)( mod * 300 );
            if( dd < -300 * 100000 )
                dd += (int64_t)( mod * 300 );
            if( llabs( dd ) > tol )
                return -( (int)k + 1 );
        }
    }
    return 0;
}

/* ---------------- standard two-program fixture ---------------- */

enum { FX_NFRAMES = 8 };

typedef struct
{
    int pnum, pmt, pcrpid, vpid;
    int avc;
    int naudio;
    int apid[6];
} prog_spec_t;

static void make_body( uint8_t *b, size_t n, int seed )
{
    for( size_t j = 0; j < n; j++ )
        b[j] = (uint8_t)( j * 17 + seed * 31 + ( j >> 3 ) * 7 );
}

/* Build and set up n programs.  Caller provides an opened sink_t (w zeroed). */
static int setup_programs( sink_t *sk, int n, const prog_spec_t *ps,
                           int muxrate, int pcr_period, int pat_period )
{
    ts_program_t progs[8];
    ts_stream_t streams[8][12];
    memset( progs, 0, sizeof(progs) );
    memset( streams, 0, sizeof(streams) );

    ts_main_t m;
    memset( &m, 0, sizeof(m) );
    m.num_programs = n;
    m.programs = progs;
    m.ts_id = 23;
    m.muxrate = muxrate;
    m.cbr = 1;
    m.lowlatency = 1;
    m.ts_type = TS_TYPE_GENERIC;
    m.pcr_period = pcr_period;
    m.pat_period = pat_period;

    for( int i = 0; i < n; i++ )
    {
        int ns = 0;
        streams[i][ns].pid = ps[i].vpid;
        streams[i][ns].stream_format = ps[i].avc ? LIBMPEGTS_VIDEO_AVC : LIBMPEGTS_VIDEO_MPEG2;
        streams[i][ns].stream_id = LIBMPEGTS_STREAM_ID_MPEGVIDEO;
        ns++;
        for( int a = 0; a < ps[i].naudio; a++ )
        {
            streams[i][ns].pid = ps[i].apid[a];
            streams[i][ns].stream_format = LIBMPEGTS_AUDIO_MPEG1;
            streams[i][ns].stream_id = LIBMPEGTS_STREAM_ID_MPEGAUDIO;
            streams[i][ns].audio_frame_size = 216;
            ns++;
        }
        progs[i].pmt_pid = ps[i].pmt;
        progs[i].program_num = ps[i].pnum;
        progs[i].pcr_pid = ps[i].pcrpid;
        progs[i].num_streams = ns;
        progs[i].streams = streams[i];
    }

    sk->w = ts_create_writer();
    if( !sk->w )
        return -1;
    sk->muxrate = muxrate;

    if( ts_setup_transport_stream( sk->w, &m ) < 0 )
        return -2;

    for( int i = 0; i < n; i++ )
    {
        if( ts_setup_mpegvideo_stream( sk->w, ps[i].vpid,
                ps[i].avc ? 30 : LIBMPEGTS_MPEG2_LEVEL_MAIN,
                ps[i].avc ? AVC_HIGH : LIBMPEGTS_MPEG2_PROFILE_MAIN,
                2000000, 200000, 3 ) < 0 )
            return -3;
    }
    return 0;
}

/* ---------------- mpts_tables ---------------- */

static int case_pat_pmt_layout( void )
{
    sink_t sk;
    memset( &sk, 0, sizeof(sk) );
    prog_spec_t ps[2] = {
        { 1, 0x1000, 0x101, 0x100, 0, 1, { 0x101 } }, /* PCR on audio ES */
        { 2, 0x1001, 0x200, 0x200, 1, 0, { 0 } },
    };
    TASK_CHECK( setup_programs( &sk, 2, ps, 2000000, 20, 80 ) == 0 );

    /* one small key frame drives the mux long enough to retransmit tables */
    static uint8_t body[640];
    make_body( body, sizeof(body), 0 );
    ts_frame_t f;
    memset( &f, 0, sizeof(f) );
    f.pid = 0x100;
    f.data = body;
    f.size = sizeof(body);
    f.dts = 9000;
    f.pts = 9000;
    f.cpb_initial_arrival_time = 900 * 300;
    f.cpb_final_arrival_time = 3600 * 300;
    f.random_access = 1;
    TASK_CHECK( sink_collect( &sk, &f, 1 ) == 0 );
    TASK_CHECK( sink_collect( &sk, NULL, 0 ) == 0 );
    TASK_CHECK( sk.npk > 30 );

    /* PAT sections */
    section_t *pat = NULL;
    int npat = 0;
    TASK_CHECK( extract_sections( sk.buf, sk.npk, 0x0000, &pat, &npat ) == 0 );
    TASK_CHECK( npat >= 1 );
    for( int i = 0; i < npat; i++ )
    {
        TASK_CHECK( pat[i].d[0] == 0x00 );
        TASK_CHECK( crc_mpeg( pat[i].d, pat[i].len ) == 0 );
        int nprog = ( ( pat[i].len - 4 ) - 8 ) / 4;
        TASK_CHECK( nprog == 2 );
        for( int j = 0; j < 2; j++ )
        {
            int off = 8 + j * 4;
            int pnum = ( pat[0].d[off] << 8 ) | pat[0].d[off + 1];
            int ppid = ( ( pat[0].d[off + 2] & 0x1f ) << 8 ) | pat[0].d[off + 3];
            TASK_CHECK( pnum == ps[j].pnum );
            TASK_CHECK( ppid == ps[j].pmt );
        }
    }
    free_sections( pat, npat );

    /* each PMT: CRC, PCR pid, exactly its own ES set, descriptors kept */
    for( int pi = 0; pi < 2; pi++ )
    {
        section_t *pmt = NULL;
        int npmt = 0;
        TASK_CHECK( extract_sections( sk.buf, sk.npk, ps[pi].pmt, &pmt, &npmt ) == 0 );
        TASK_CHECK( npmt >= 1 );
        for( int s = 0; s < npmt; s++ )
        {
            TASK_CHECK( pmt[s].d[0] == 0x02 );
            TASK_CHECK( crc_mpeg( pmt[s].d, pmt[s].len ) == 0 );
            int pnum = ( pmt[s].d[3] << 8 ) | pmt[s].d[4];
            TASK_CHECK( pnum == ps[pi].pnum );
            int pcrpid = ( ( pmt[s].d[8] & 0x1f ) << 8 ) | pmt[s].d[9];
            TASK_CHECK( pcrpid == ps[pi].pcrpid );
            int pil = ( ( pmt[s].d[10] & 0x0f ) << 8 ) | pmt[s].d[11];
            int q = 12 + pil;
            int end = pmt[s].len - 4;
            int seen_video = 0, seen_audio = 0;
            while( q < end )
            {
                int stype = pmt[s].d[q];
                int epid = ( ( pmt[s].d[q + 1] & 0x1f ) << 8 ) | pmt[s].d[q + 2];
                int eil = ( ( pmt[s].d[q + 3] & 0x0f ) << 8 ) | pmt[s].d[q + 4];
                int d0 = q + 5;
                /* data stream alignment descriptor must be retained */
                TASK_CHECK( pmt[s].d[d0] == 0x06 && pmt[s].d[d0 + 1] == 1 );
                if( epid == ps[pi].vpid )
                {
                    seen_video = 1;
                    TASK_CHECK( stype == ( ps[pi].avc ? 0x1b : 0x02 ) );
                    if( ps[pi].avc )
                    {
                        /* AVC descriptor 0x28 somewhere in ES descriptors */
                        int found_avc = 0;
                        for( int z = d0; z < d0 + eil; )
                        {
                            if( pmt[s].d[z] == 0x28 )
                                found_avc = 1;
                            z += 2 + pmt[s].d[z + 1];
                        }
                        TASK_CHECK( found_avc );
                    }
                }
                for( int a = 0; a < ps[pi].naudio; a++ )
                    if( epid == ps[pi].apid[a] )
                        seen_audio = 1;
                /* PMT must never list the other program's ES */
                for( int oj = 0; oj < 2; oj++ )
                    if( oj != pi )
                    {
                        TASK_CHECK( epid != ps[oj].vpid );
                        for( int a = 0; a < ps[oj].naudio; a++ )
                            TASK_CHECK( epid != ps[oj].apid[a] );
                    }
                q += 5 + eil;
            }
            TASK_CHECK( seen_video );
            TASK_CHECK( seen_audio == ( ps[pi].naudio > 0 ) );
            TASK_CHECK( q == end );
        }
        free_sections( pmt, npmt );
    }

    /* PAT and both complete PMTs precede the first ES payload packet */
    size_t pat_done = 0, pmt_done = 0;
    int first_media = -1;
    for( size_t k = 0; k < sk.npk; k++ )
    {
        const uint8_t *p = sk.buf + k * 188;
        int pid = pid_of( p );
        if( first_media < 0 && ( pid == 0x100 || pid == 0x200 || pid == 0x101 ) &&
            ( afc_of( p ) & 1 ) && pusi_of( p ) )
        {
            int pos = payload_pos( p );
            if( pos + 3 <= 188 && p[pos] == 0 && p[pos + 1] == 0 && p[pos + 2] == 1 )
                first_media = (int)k;
        }
    }
    TASK_CHECK( first_media > 4 );
    int saw_pat = 0, saw_pmt1 = 0, saw_pmt2 = 0;
    for( int k = 0; k < first_media; k++ )
    {
        int pid = pid_of( sk.buf + k * 188 );
        if( pid == 0 )
            saw_pat = 1;
        if( pid == 0x1000 )
            saw_pmt1 = 1;
        if( pid == 0x1001 )
            saw_pmt2 = 1;
    }
    TASK_CHECK( saw_pat && saw_pmt1 && saw_pmt2 );
    (void)pat_done;
    (void)pmt_done;

    TASK_CHECK( check_continuity( sk.buf, sk.npk ) == 0 );
    sink_done( &sk );
    return 0;
}

/* Long PMT split across multiple TS packets, reassembled and CRC clean. */
static int case_cross_packet_pmt( void )
{
    sink_t sk;
    memset( &sk, 0, sizeof(sk) );

    enum { NAUD = 24 };
    ts_stream_t aud[NAUD + 1];
    memset( aud, 0, sizeof(aud) );
    /* program 0: one video plus many audio streams -> PMT longer than 183 B */
    aud[0].pid = 0x400;
    aud[0].stream_format = LIBMPEGTS_VIDEO_MPEG2;
    aud[0].stream_id = LIBMPEGTS_STREAM_ID_MPEGVIDEO;
    for( int i = 0; i < NAUD; i++ )
    {
        aud[i + 1].pid = 0x100 + i; /* 0x100..0x117 */
        aud[i + 1].stream_format = LIBMPEGTS_AUDIO_MPEG1;
        aud[i + 1].stream_id = LIBMPEGTS_STREAM_ID_MPEGAUDIO;
    }
    ts_program_t pr[2];
    memset( pr, 0, sizeof(pr) );
    pr[0].pmt_pid = 0x1200;
    pr[0].program_num = 9;
    pr[0].pcr_pid = 0x1300; /* exclusive PCR pid */
    pr[0].num_streams = NAUD + 1;
    pr[0].streams = aud;
    ts_stream_t v2 = { 0 };
    v2.pid = 0x500;
    v2.stream_format = LIBMPEGTS_VIDEO_AVC;
    v2.stream_id = LIBMPEGTS_STREAM_ID_MPEGVIDEO;
    pr[1].pmt_pid = 0x1400;
    pr[1].program_num = 10;
    pr[1].pcr_pid = 0x500; /* PCR on its ES */
    pr[1].num_streams = 1;
    pr[1].streams = &v2;

    ts_main_t m = { 0 };
    m.num_programs = 2;
    m.programs = pr;
    m.muxrate = 4000000;
    m.cbr = 1;
    m.lowlatency = 1;
    m.ts_type = TS_TYPE_GENERIC;
    m.pcr_period = 20;
    m.pat_period = 80;

    sk.w = ts_create_writer();
    TASK_CHECK( sk.w != NULL );
    sk.muxrate = 4000000;
    TASK_CHECK( ts_setup_transport_stream( sk.w, &m ) == 0 );
    TASK_CHECK( ts_setup_mpegvideo_stream( sk.w, 0x400,
        LIBMPEGTS_MPEG2_LEVEL_MAIN, LIBMPEGTS_MPEG2_PROFILE_MAIN, 2000000, 200000, 3 ) == 0 );
    TASK_CHECK( ts_setup_mpegvideo_stream( sk.w, 0x500, 30, AVC_HIGH, 2000000, 200000, 1 ) == 0 );
    TASK_CHECK( sink_collect( &sk, NULL, 0 ) == 0 );

    section_t *pmt = NULL;
    int n = 0;
    TASK_CHECK( extract_sections( sk.buf, sk.npk, 0x1200, &pmt, &n ) == 0 );
    TASK_CHECK( n >= 1 );
    TASK_CHECK( pmt[0].len > 183 ); /* genuinely cross-packet */
    TASK_CHECK( crc_mpeg( pmt[0].d, pmt[0].len ) == 0 );

    int end = pmt[0].len - 4;
    int q = 12 + ( ( ( pmt[0].d[10] & 15 ) << 8 ) | pmt[0].d[11] );
    int count = 0;
    while( q < end )
    {
        int eil = ( ( pmt[0].d[q + 3] & 15 ) << 8 ) | pmt[0].d[q + 4];
        count++;
        q += 5 + eil;
    }
    TASK_CHECK( count == NAUD + 1 );
    TASK_CHECK( q == end );
    free_sections( pmt, n );

    /* the second program's short PMT is present and valid too */
    section_t *pmt2 = NULL;
    int n2 = 0;
    TASK_CHECK( extract_sections( sk.buf, sk.npk, 0x1400, &pmt2, &n2 ) == 0 );
    TASK_CHECK( n2 >= 1 );
    TASK_CHECK( crc_mpeg( pmt2[0].d, pmt2[0].len ) == 0 );
    free_sections( pmt2, n2 );

    /* at least two TS packets carry the long PMT pid */
    int pmt_pkts = 0;
    for( size_t k = 0; k < sk.npk; k++ )
        if( pid_of( sk.buf + k * 188 ) == 0x1200 )
            pmt_pkts++;
    TASK_CHECK( pmt_pkts >= 2 );
    TASK_CHECK( check_continuity( sk.buf, sk.npk ) == 0 );
    sink_done( &sk );
    return 0;
}

/* every conflict / unsupported mode is negative and closable */
static int expect_bad_config( ts_main_t *m )
{
    ts_writer_t *w = ts_create_writer();
    if( !w )
        return -1;
    int rc = ts_setup_transport_stream( w, m );
    int close_rc = ts_close_writer( w );
    if( rc >= 0 || close_rc != 0 )
        return -2;
    return 0;
}

static int case_config_conflicts( void )
{
    ts_stream_t v1 = { 0 };
    v1.pid = 0x100;
    v1.stream_format = LIBMPEGTS_VIDEO_MPEG2;
    v1.stream_id = 0xe0;
    ts_stream_t v2 = { 0 };
    v2.pid = 0x200;
    v2.stream_format = LIBMPEGTS_VIDEO_AVC;
    v2.stream_id = 0xe0;

    ts_program_t base[2] = { 0 };
    base[0].pmt_pid = 0x1000; base[0].program_num = 1; base[0].pcr_pid = 0x110;
    base[0].num_streams = 1; base[0].streams = &v1;
    base[1].pmt_pid = 0x1001; base[1].program_num = 2; base[1].pcr_pid = 0x210;
    base[1].num_streams = 1; base[1].streams = &v2;

    ts_main_t m = { 0 };
    m.num_programs = 2; m.programs = base; m.muxrate = 2000000;
    m.cbr = 1; m.lowlatency = 1; m.ts_type = TS_TYPE_GENERIC;

    /* valid control case */
    ts_writer_t *good = ts_create_writer();
    TASK_CHECK( good != NULL );
    TASK_CHECK( ts_setup_transport_stream( good, &m ) == 0 );
    TASK_CHECK( ts_setup_mpegvideo_stream( good, 0x200, 30, AVC_HIGH, 1, 1, 1 ) == 0 );
    TASK_CHECK( ts_close_writer( good ) == 0 );

    ts_main_t t;

    t = m; { ts_program_t p[2]; memcpy( p, base, sizeof(base) ); p[1].program_num = 1; t.programs = p; TASK_CHECK( expect_bad_config( &t ) == 0 ); }
    t = m; { ts_program_t p[2]; memcpy( p, base, sizeof(base) ); p[0].program_num = 0; t.programs = p; TASK_CHECK( expect_bad_config( &t ) == 0 ); }
    t = m; { ts_program_t p[2]; memcpy( p, base, sizeof(base) ); p[0].program_num = 70000; t.programs = p; TASK_CHECK( expect_bad_config( &t ) == 0 ); }
    t = m; { ts_program_t p[2]; memcpy( p, base, sizeof(base) ); p[0].pmt_pid = 0x100; t.programs = p; TASK_CHECK( expect_bad_config( &t ) == 0 ); }
    t = m; { ts_stream_t a = v2; a.pid = 0x100; ts_program_t p[2]; memcpy( p, base, sizeof(base) ); p[1].streams = &a; t.programs = p; TASK_CHECK( expect_bad_config( &t ) == 0 ); }
    t = m; { ts_program_t p[2]; memcpy( p, base, sizeof(base) ); p[1].pcr_pid = 0x110; t.programs = p; TASK_CHECK( expect_bad_config( &t ) == 0 ); }
    t = m; { ts_program_t p[2]; memcpy( p, base, sizeof(base) ); p[0].pcr_pid = 0x1001; t.programs = p; TASK_CHECK( expect_bad_config( &t ) == 0 ); }
    t = m; { ts_program_t p[2]; memcpy( p, base, sizeof(base) ); p[1].pmt_pid = 0x1000; t.programs = p; TASK_CHECK( expect_bad_config( &t ) == 0 ); }
    t = m; { ts_stream_t a = v1; a.pid = 0x10; ts_program_t p[1] = { base[0] }; p[0].streams = &a; t.num_programs = 2; ts_program_t pp[2] = { p[0], base[1] }; t.programs = pp; TASK_CHECK( expect_bad_config( &t ) == 0 ); }
    t = m; { ts_program_t p[2]; memcpy( p, base, sizeof(base) ); p[0].pmt_pid = 0x1fff; t.programs = p; TASK_CHECK( expect_bad_config( &t ) == 0 ); }
    t = m; { m.network_pid = 0x1000; TASK_CHECK( expect_bad_config( &m ) == 0 ); m.network_pid = 0; }
    t = m; t.ts_type = TS_TYPE_DVB; TASK_CHECK( expect_bad_config( &t ) == 0 ); t = m;
    t.cbr = 0; TASK_CHECK( expect_bad_config( &t ) == 0 ); t = m;
    t.lowlatency = 0; TASK_CHECK( expect_bad_config( &t ) == 0 ); t = m;
    t.num_programs = 9; TASK_CHECK( expect_bad_config( &t ) == 0 ); t = m;
    t.muxrate = 0; TASK_CHECK( expect_bad_config( &t ) == 0 );

    /* rejected setup must leave a pre-existing good config closable */
    ts_writer_t *w2 = ts_create_writer();
    TASK_CHECK( w2 != NULL );
    TASK_CHECK( ts_setup_transport_stream( w2, &m ) == 0 );
    ts_main_t bad = m;
    bad.cbr = 0;
    TASK_CHECK( ts_setup_transport_stream( w2, &bad ) < 0 );
    TASK_CHECK( ts_close_writer( w2 ) == 0 );

    return 0;
}

/* exact single-program byte/FNV regression, same fixture as task baseline */
static int case_single_program_bytes( void )
{
    ts_stream_t s = { 0 };
    s.pid = 0x100;
    s.stream_format = LIBMPEGTS_VIDEO_MPEG2;
    s.stream_id = LIBMPEGTS_STREAM_ID_MPEGVIDEO;
    ts_program_t p = { 0 };
    p.pmt_pid = 0x1000; p.program_num = 7; p.pcr_pid = 0x110;
    p.num_streams = 1; p.streams = &s;
    ts_main_t mm = { 0 };
    mm.num_programs = 1; mm.programs = &p; mm.ts_id = 23; mm.muxrate = 2000000;
    mm.cbr = 1; mm.lowlatency = 1; mm.ts_type = TS_TYPE_GENERIC;
    mm.pcr_period = 20; mm.pat_period = 80;

    sink_t sk = { 0 };
    sk.w = ts_create_writer();
    sk.muxrate = 2000000;
    TASK_CHECK( sk.w );
    TASK_CHECK( ts_setup_transport_stream( sk.w, &mm ) == 0 );
    TASK_CHECK( ts_setup_mpegvideo_stream( sk.w, s.pid,
        LIBMPEGTS_MPEG2_LEVEL_MAIN, LIBMPEGTS_MPEG2_PROFILE_MAIN, 1000000, 100000, 3 ) == 0 );

    uint8_t body[640];
    for( int i = 0; i < 8; i++ )
    {
        for( size_t j = 0; j < sizeof(body); j++ )
            body[j] = (uint8_t)( j * 17 + i * 31 );
        ts_frame_t f = { 0 };
        f.pid = s.pid; f.data = body; f.size = sizeof(body);
        f.dts = 9000 + i * 3600; f.pts = f.dts;
        f.cpb_initial_arrival_time = (int64_t)( i * 3600 + 900 ) * 300;
        f.cpb_final_arrival_time = (int64_t)( i * 3600 + 3600 ) * 300;
        f.random_access = i == 0;
        TASK_CHECK( sink_collect( &sk, &f, 1 ) == 0 );
        memset( body, 0xee, sizeof(body) );
    }
    TASK_CHECK( sink_collect( &sk, NULL, 0 ) == 0 );
    TASK_CHECK( sk.len == 80088 );

    uint64_t fnv = 14695981039346656037ULL;
    for( size_t i = 0; i < sk.len; i++ )
    {
        fnv ^= sk.buf[i];
        fnv *= 1099511628211ULL;
    }
    TASK_CHECK( fnv == 0x1a31e3a3681f738eULL );
    TASK_CHECK( check_common_clock( &sk ) == 0 );
    TASK_CHECK( check_continuity( sk.buf, sk.npk ) == 0 );
    sink_done( &sk );
    return 0;
}

/* ---------------- mpts_clocks ---------------- */

static int pcr_gaps_ok( const sink_t *sk, int pcrpid, int nprog, int64_t *maxgap )
{
    int64_t period = 20LL * 27000LL;
    int64_t slot = 188LL * 8 * 27000000LL / sk->muxrate;
    /* pcr_period + one slot, plus up to N-1 slots when N PCRs collide */
    int64_t bound = period + (int64_t)nprog * slot + 4;
    uint64_t last = 0;
    int have = 0;
    int count = 0;
    *maxgap = 0;

    for( size_t k = 0; k < sk->npk; k++ )
    {
        const uint8_t *p = sk->buf + k * 188;
        if( pid_of( p ) != pcrpid )
            continue;
        uint64_t v;
        if( !pcr_decode( p, &v ) )
            continue;
        count++;
        if( have )
        {
            /* short tests never wrap; compare straight values */
            int64_t gap = (int64_t)v - (int64_t)last;
            if( gap < 0 )
                gap += (int64_t)( ( (uint64_t)1 << 33 ) * 300 );
            if( gap > *maxgap )
                *maxgap = gap;
            if( gap > bound )
                return -1;
        }
        last = v;
        have = 1;
    }
    return count >= 2 ? 0 : -2;
}

static int case_clock_two_programs( void )
{
    sink_t sk;
    memset( &sk, 0, sizeof(sk) );
    prog_spec_t ps[2] = {
        { 11, 0x1000, 0x110, 0x100, 0, 0, { 0 } },  /* independent PCR pid */
        { 12, 0x1001, 0x200, 0x200, 0, 0, { 0 } },  /* PCR on video ES */
    };
    TASK_CHECK( setup_programs( &sk, 2, ps, 2000000, 20, 80 ) == 0 );

    static uint8_t bodies[FX_NFRAMES][640];
    ts_frame_t frames[FX_NFRAMES * 2];
    int nf = 0;
    for( int i = 0; i < FX_NFRAMES; i++ )
    {
        make_body( bodies[i], 640, i );
        frames[nf] = (ts_frame_t){ 0 };
        frames[nf].pid = 0x100; frames[nf].data = bodies[i]; frames[nf].size = 640;
        frames[nf].dts = 9000 + i * 3600; frames[nf].pts = frames[nf].dts;
        frames[nf].cpb_initial_arrival_time = (int64_t)( i * 3600 + 900 ) * 300;
        frames[nf].cpb_final_arrival_time = (int64_t)( i * 3600 + 3600 ) * 300;
        frames[nf].random_access = i == 0;
        nf++;
        frames[nf] = (ts_frame_t){ 0 };
        frames[nf].pid = 0x200; frames[nf].data = bodies[i]; frames[nf].size = 640;
        frames[nf].dts = 9000 + i * 3600 + 1800; frames[nf].pts = frames[nf].dts;
        frames[nf].cpb_initial_arrival_time = (int64_t)( i * 3600 + 1200 ) * 300;
        frames[nf].cpb_final_arrival_time = (int64_t)( i * 3600 + 3600 ) * 300;
        frames[nf].random_access = i == 0;
        nf++;
    }
    TASK_CHECK( sink_collect( &sk, frames, nf ) == 0 );
    TASK_CHECK( sink_collect( &sk, NULL, 0 ) == 0 );

    TASK_CHECK( sk.npk > 200 );
    TASK_CHECK( check_common_clock( &sk ) == 0 );
    TASK_CHECK( check_continuity( sk.buf, sk.npk ) == 0 );

    int64_t mg1 = 0, mg2 = 0;
    TASK_CHECK( pcr_gaps_ok( &sk, 0x110, 2, &mg1 ) == 0 );
    TASK_CHECK( pcr_gaps_ok( &sk, 0x200, 2, &mg2 ) == 0 );

    /* independent PCR pid must actually carry adaptation-only packets */
    int adapt_only = 0;
    for( size_t k = 0; k < sk.npk; k++ )
        if( pid_of( sk.buf + k * 188 ) == 0x110 && afc_of( sk.buf + k * 188 ) == 2 )
            adapt_only++;
    TASK_CHECK( adapt_only >= 2 );

    /* each program gets an initial PCR among the first three packets */
    int first110 = -1, first200 = -1;
    for( size_t k = 0; k < sk.npk; k++ )
    {
        int pid = pid_of( sk.buf + k * 188 );
        uint64_t v;
        if( pid == 0x110 && first110 < 0 && pcr_decode( sk.buf + k * 188, &v ) )
            first110 = (int)k;
        if( pid == 0x200 && first200 < 0 && pcr_decode( sk.buf + k * 188, &v ) )
            first200 = (int)k;
    }
    TASK_CHECK( first110 == 0 );
    TASK_CHECK( first200 == 1 );

    sink_done( &sk );
    return 0;
}

/* N programs with synchronized PCR periods all keep their own cadence */
static int case_clock_eight_programs( void )
{
    enum { N = 8 };
    sink_t sk;
    memset( &sk, 0, sizeof(sk) );
    prog_spec_t ps[N];
    for( int i = 0; i < N; i++ )
    {
        ps[i].pnum = 100 + i;
        ps[i].pmt = 0x1000 + i;
        ps[i].pcrpid = 0x0300 + i; /* exclusive PCR pids */
        ps[i].vpid = 0x0100 + i;
        ps[i].avc = 0;
        ps[i].naudio = 0;
    }
    TASK_CHECK( setup_programs( &sk, N, ps, 4000000, 20, 200 ) == 0 );

    /* drive ~200 ms through program 0's video */
    static uint8_t body[600];
    make_body( body, sizeof(body), 3 );
    ts_frame_t f[6];
    for( int i = 0; i < 6; i++ )
    {
        memset( &f[i], 0, sizeof(f[i]) );
        f[i].pid = 0x100;
        f[i].data = body;
        f[i].size = sizeof(body);
        f[i].dts = 9000 + i * 3600;
        f[i].pts = f[i].dts;
        f[i].cpb_initial_arrival_time = (int64_t)( i * 3600 + 600 ) * 300;
        f[i].cpb_final_arrival_time = (int64_t)( i * 3600 + 3600 ) * 300;
        f[i].random_access = i == 0;
    }
    TASK_CHECK( sink_collect( &sk, f, 6 ) == 0 );
    TASK_CHECK( sink_collect( &sk, NULL, 0 ) == 0 );
    TASK_CHECK( check_common_clock( &sk ) == 0 );
    TASK_CHECK( check_continuity( sk.buf, sk.npk ) == 0 );

    for( int i = 0; i < N; i++ )
    {
        int64_t mg;
        TASK_CHECK( pcr_gaps_ok( &sk, ps[i].pcrpid, N, &mg ) == 0 );
    }
    sink_done( &sk );
    return 0;
}

/* a program with no frames still gets paced PCRs while another muxes */
static int case_silent_program_clock( void )
{
    sink_t sk;
    memset( &sk, 0, sizeof(sk) );
    prog_spec_t ps[2] = {
        { 1, 0x1000, 0x110, 0x100, 0, 0, { 0 } },
        { 2, 0x1001, 0x220, 0x200, 1, 0, { 0 } }, /* silent, exclusive PCR */
    };
    TASK_CHECK( setup_programs( &sk, 2, ps, 2000000, 20, 80 ) == 0 );

    static uint8_t bodies[6][640];
    ts_frame_t frames[6];
    for( int i = 0; i < 6; i++ )
    {
        make_body( bodies[i], 640, 50 + i );
        memset( &frames[i], 0, sizeof(frames[i]) );
        frames[i].pid = 0x100;
        frames[i].data = bodies[i];
        frames[i].size = 640;
        frames[i].dts = 9000 + i * 3600;
        frames[i].pts = frames[i].dts;
        frames[i].cpb_initial_arrival_time = (int64_t)( i * 3600 + 900 ) * 300;
        frames[i].cpb_final_arrival_time = (int64_t)( i * 3600 + 3600 ) * 300;
        frames[i].random_access = i == 0;
    }
    TASK_CHECK( sink_collect( &sk, frames, 6 ) == 0 );
    TASK_CHECK( sink_collect( &sk, NULL, 0 ) == 0 );

    int64_t mg;
    TASK_CHECK( pcr_gaps_ok( &sk, 0x110, 2, &mg ) == 0 );
    TASK_CHECK( pcr_gaps_ok( &sk, 0x220, 2, &mg ) == 0 );
    TASK_CHECK( check_common_clock( &sk ) == 0 );
    sink_done( &sk );
    return 0;
}

/* PCR base wraps at 2^33 while pcr_list keeps unwrapped 27 MHz values */
static int case_pcr_wrap( void )
{
    sink_t sk;
    memset( &sk, 0, sizeof(sk) );
    prog_spec_t ps[2] = {
        { 1, 0x1000, 0x110, 0x100, 0, 0, { 0 } },
        { 2, 0x1001, 0x111, 0x200, 0, 0, { 0 } },
    };
    TASK_CHECK( setup_programs( &sk, 2, ps, 100, 100, 100000 ) == 0 );

    /* far final arrival drives ~64 slots, enough to cross the 33-bit wrap */
    static uint8_t body[64];
    ts_frame_t f = { 0 };
    f.pid = 0x100;
    f.data = body;
    f.size = sizeof(body);
    f.dts = 9000;
    f.pts = 9000;
    f.cpb_initial_arrival_time = (int64_t)2500000000000LL;
    f.cpb_final_arrival_time = (int64_t)2600000000000LL;
    TASK_CHECK( sink_collect( &sk, &f, 1 ) == 0 );
    TASK_CHECK( sink_collect( &sk, NULL, 0 ) == 0 );
    TASK_CHECK( sk.npk > 60 );

    uint64_t mod = (uint64_t)1 << 33;
    int saw_wrap_pid1 = 0;
    uint64_t prev = 0;
    int have = 0;
    for( size_t k = 0; k < sk.npk; k++ )
    {
        const uint8_t *p = sk.buf + k * 188;
        if( pid_of( p ) != 0x110 )
            continue;
        uint64_t pcr;
        if( !pcr_decode( p, &pcr ) )
            continue;
        TASK_CHECK( pcr < mod * 300 );
        if( have && pcr < prev )
            saw_wrap_pid1 = 1;
        prev = pcr;
        have = 1;
    }
    TASK_CHECK( saw_wrap_pid1 );
    TASK_CHECK( check_common_clock( &sk ) == 0 );
    sink_done( &sk );
    return 0;
}

/* ---------------- mpts_media ---------------- */

typedef struct
{
    int pid;
    int is_video;
    int nframes;
    const int *sizes;
    uint8_t (*bodies)[700];
    const int64_t *ini;
    const int64_t *fin;
} media_track_t;

static int verify_track( const sink_t *sk, const media_track_t *t )
{
    pes_t *pes = NULL;
    int n = 0;
    if( extract_pes( sk->buf, sk->npk, t->pid, &pes, &n ) < 0 )
        return -1;
    if( n != t->nframes )
    {
        free_pes( pes, n );
        return -2;
    }

    int64_t slot = 188LL * 8 * 27000000LL / sk->muxrate;

    for( int i = 0; i < n; i++ )
    {
        if( pes[i].es_len != t->sizes[i] )
        {
            free_pes( pes, n );
            return -3;
        }
        if( memcmp( pes[i].es, t->bodies[i], t->sizes[i] ) )
        {
            free_pes( pes, n );
            return -4;
        }
        if( t->is_video )
        {
            if( pes[i].pts != 900000 + 9000 + i * 3600 + ( t->pid == 0x200 ? 1800 : 0 ) )
            {
                free_pes( pes, n );
                return -5;
            }
            int64_t first_t = sk->clock[pes[i].first_pkt] - 270000000LL;
            int64_t last_t = sk->clock[pes[i].last_pkt] - 270000000LL;
            if( first_t < t->ini[i] - slot )
            {
                free_pes( pes, n );
                return -6;
            }
            if( last_t > t->fin[i] + slot )
            {
                free_pes( pes, n );
                return -7;
            }
        }
        else
        {
            /* audio keeps the existing earliest-arrival restriction */
            int64_t earliest = ( 900000 + 9000 + i * 3600 + 1800 - 216 ) * 300 - 270000000LL;
            int64_t first_t = sk->clock[pes[i].first_pkt] - 270000000LL;
            if( first_t < earliest - slot )
            {
                free_pes( pes, n );
                return -8;
            }
        }
    }
    free_pes( pes, n );
    return 0;
}

static int case_media_overlap( void )
{
    sink_t sk;
    memset( &sk, 0, sizeof(sk) );
    prog_spec_t ps[2] = {
        { 1, 0x1000, 0x101, 0x100, 0, 1, { 0x101 } }, /* PCR on audio ES */
        { 2, 0x1001, 0x200, 0x200, 1, 0, { 0 } },
    };
    TASK_CHECK( setup_programs( &sk, 2, ps, 2000000, 20, 80 ) == 0 );

    static uint8_t b1[FX_NFRAMES][700];
    static uint8_t b2[FX_NFRAMES][700];
    static uint8_t ba[FX_NFRAMES][700];
    static int sz1[FX_NFRAMES], sz2[FX_NFRAMES], sza[FX_NFRAMES];
    static int64_t ini1[FX_NFRAMES], fin1[FX_NFRAMES];
    static int64_t ini2[FX_NFRAMES], fin2[FX_NFRAMES];

    ts_frame_t frames[FX_NFRAMES * 3];
    int nf = 0;
    for( int i = 0; i < FX_NFRAMES; i++ )
    {
        sz1[i] = 640; sz2[i] = 500; sza[i] = 200;
        make_body( b1[i], sz1[i], i );
        make_body( b2[i], sz2[i], i + 40 );
        memset( ba[i], 0xcd, sza[i] );
        ini1[i] = (int64_t)( i * 3600 + 900 ) * 300;
        fin1[i] = (int64_t)( i * 3600 + 3600 ) * 300;
        ini2[i] = (int64_t)( i * 3600 + 1200 ) * 300;
        fin2[i] = fin1[i];

        frames[nf] = (ts_frame_t){ 0 };
        frames[nf].pid = 0x100; frames[nf].data = b1[i]; frames[nf].size = sz1[i];
        frames[nf].dts = 9000 + i * 3600; frames[nf].pts = frames[nf].dts;
        frames[nf].cpb_initial_arrival_time = ini1[i];
        frames[nf].cpb_final_arrival_time = fin1[i];
        frames[nf].random_access = i == 0;
        nf++;

        frames[nf] = (ts_frame_t){ 0 };
        frames[nf].pid = 0x101; frames[nf].data = ba[i]; frames[nf].size = sza[i];
        frames[nf].pts = frames[nf].dts = 9000 + i * 3600 + 1800;
        nf++;

        frames[nf] = (ts_frame_t){ 0 };
        frames[nf].pid = 0x200; frames[nf].data = b2[i]; frames[nf].size = sz2[i];
        frames[nf].dts = 9000 + i * 3600 + 1800; frames[nf].pts = frames[nf].dts;
        frames[nf].cpb_initial_arrival_time = ini2[i];
        frames[nf].cpb_final_arrival_time = fin2[i];
        frames[nf].random_access = i == 0;
        nf++;
    }

    TASK_CHECK( sink_collect( &sk, frames, nf ) == 0 );
    TASK_CHECK( sink_collect( &sk, NULL, 0 ) == 0 );

    media_track_t t1 = { 0x100, 1, FX_NFRAMES, sz1, b1, ini1, fin1 };
    media_track_t t2 = { 0x200, 1, FX_NFRAMES, sz2, b2, ini2, fin2 };
    media_track_t ta = { 0x101, 0, FX_NFRAMES, sza, ba, NULL, NULL };
    TASK_CHECK( verify_track( &sk, &t1 ) == 0 );
    TASK_CHECK( verify_track( &sk, &t2 ) == 0 );
    TASK_CHECK( verify_track( &sk, &ta ) == 0 );

    TASK_CHECK( check_common_clock( &sk ) == 0 );
    TASK_CHECK( check_continuity( sk.buf, sk.npk ) == 0 );

    /* random access indicator on the first packet of each RA PES only */
    pes_t *pv = NULL;
    int npv = 0;
    TASK_CHECK( extract_pes( sk.buf, sk.npk, 0x100, &pv, &npv ) == 0 );
    TASK_CHECK( npv == FX_NFRAMES );
    TASK_CHECK( sk.buf[pv[0].first_pkt * 188 + 5] & 0x40 );
    for( int i = 1; i < npv; i++ )
        TASK_CHECK( !( sk.buf[pv[i].first_pkt * 188 + 5] & 0x40 ) );
    free_pes( pv, npv );

    sink_done( &sk );
    return 0;
}

/* only program 1 carries media; program 2 stays silent */
static int case_media_silent_program( void )
{
    sink_t sk;
    memset( &sk, 0, sizeof(sk) );
    prog_spec_t ps[2] = {
        { 1, 0x1000, 0x110, 0x100, 0, 0, { 0 } },
        { 2, 0x1001, 0x200, 0x200, 0, 0, { 0 } }, /* silent ES carries PCR */
    };
    TASK_CHECK( setup_programs( &sk, 2, ps, 2000000, 20, 80 ) == 0 );

    static uint8_t bodies[5][700];
    static int sizes[5];
    static int64_t ini[5], fin[5];
    ts_frame_t frames[5];
    for( int i = 0; i < 5; i++ )
    {
        sizes[i] = 640;
        make_body( bodies[i], 640, 70 + i );
        ini[i] = (int64_t)( i * 3600 + 900 ) * 300;
        fin[i] = (int64_t)( i * 3600 + 3600 ) * 300;
        memset( &frames[i], 0, sizeof(frames[i]) );
        frames[i].pid = 0x100; frames[i].data = bodies[i]; frames[i].size = 640;
        frames[i].dts = 9000 + i * 3600; frames[i].pts = frames[i].dts;
        frames[i].cpb_initial_arrival_time = ini[i];
        frames[i].cpb_final_arrival_time = fin[i];
        frames[i].random_access = i == 0;
    }
    TASK_CHECK( sink_collect( &sk, frames, 5 ) == 0 );
    TASK_CHECK( sink_collect( &sk, NULL, 0 ) == 0 );

    media_track_t t = { 0x100, 1, 5, sizes, bodies, ini, fin };
    TASK_CHECK( verify_track( &sk, &t ) == 0 );

    /* no PES started on the silent video pid */
    pes_t *p2 = NULL;
    int n2 = 0;
    TASK_CHECK( extract_pes( sk.buf, sk.npk, 0x200, &p2, &n2 ) == 0 );
    TASK_CHECK( n2 == 0 );
    free_pes( p2, n2 );

    int64_t mg;
    TASK_CHECK( pcr_gaps_ok( &sk, 0x200, 2, &mg ) == 0 );
    sink_done( &sk );
    return 0;
}

/* three monotonic batches plus a final drain lose or reorder nothing */
static int case_media_batches( void )
{
    sink_t sk;
    memset( &sk, 0, sizeof(sk) );
    prog_spec_t ps[2] = {
        { 1, 0x1000, 0x110, 0x100, 0, 0, { 0 } },
        { 2, 0x1001, 0x200, 0x200, 0, 0, { 0 } },
    };
    TASK_CHECK( setup_programs( &sk, 2, ps, 2000000, 20, 80 ) == 0 );

    enum { TOT = 9 };
    static uint8_t b1[TOT][700], b2[TOT][700];
    static int sz[TOT];
    static int64_t ini[TOT], fin[TOT];

    for( int batch = 0; batch < 3; batch++ )
    {
        ts_frame_t fr[6];
        int nf = 0;
        for( int j = 0; j < 3; j++ )
        {
            int i = batch * 3 + j;
            sz[i] = 600;
            make_body( b1[i], sz[i], i );
            make_body( b2[i], sz[i], i + 99 );
            ini[i] = (int64_t)( i * 3600 + 900 ) * 300;
            fin[i] = (int64_t)( i * 3600 + 3600 ) * 300;

            memset( &fr[nf], 0, sizeof(fr[nf]) );
            fr[nf].pid = 0x100; fr[nf].data = b1[i]; fr[nf].size = sz[i];
            fr[nf].dts = 9000 + i * 3600; fr[nf].pts = fr[nf].dts;
            fr[nf].cpb_initial_arrival_time = ini[i];
            fr[nf].cpb_final_arrival_time = fin[i];
            fr[nf].random_access = ( i % 3 ) == 0;
            nf++;

            memset( &fr[nf], 0, sizeof(fr[nf]) );
            fr[nf].pid = 0x200; fr[nf].data = b2[i]; fr[nf].size = sz[i];
            fr[nf].dts = 9000 + i * 3600 + 1800; fr[nf].pts = fr[nf].dts;
            fr[nf].cpb_initial_arrival_time = ini[i] + 300 * 300;
            fr[nf].cpb_final_arrival_time = fin[i];
            fr[nf].random_access = ( i % 3 ) == 0;
            nf++;
        }
        TASK_CHECK( sink_collect( &sk, fr, nf ) == 0 );
    }
    TASK_CHECK( sink_collect( &sk, NULL, 0 ) == 0 );

    media_track_t t1 = { 0x100, 1, TOT, sz, b1, ini, fin };
    media_track_t t2 = { 0x200, 1, TOT, sz, b2, ini, fin };
    /* t2 PTS/window offsets differ; check payload integrity separately */
    TASK_CHECK( verify_track( &sk, &t1 ) == 0 );
    pes_t *pes = NULL;
    int n = 0;
    TASK_CHECK( extract_pes( sk.buf, sk.npk, 0x200, &pes, &n ) == 0 );
    TASK_CHECK( n == TOT );
    for( int i = 0; i < n; i++ )
    {
        TASK_CHECK( pes[i].es_len == sz[i] );
        TASK_CHECK( memcmp( pes[i].es, b2[i], sz[i] ) == 0 );
        TASK_CHECK( pes[i].pts == 900000 + 9000 + i * 3600 + 1800 );
    }
    free_pes( pes, n );

    TASK_CHECK( check_continuity( sk.buf, sk.npk ) == 0 );
    TASK_CHECK( check_common_clock( &sk ) == 0 );
    sink_done( &sk );
    return 0;
}

/* ---------------- registry ---------------- */

const struct task_case task_cases[] = {
    { "mpts_tables", "pat_pmt_layout",      case_pat_pmt_layout },
    { "mpts_tables", "cross_packet_pmt",     case_cross_packet_pmt },
    { "mpts_tables", "config_conflicts",    case_config_conflicts },
    { "mpts_tables", "single_program_bytes", case_single_program_bytes },
    { "mpts_clocks", "clock_two_programs",  case_clock_two_programs },
    { "mpts_clocks", "clock_eight_programs", case_clock_eight_programs },
    { "mpts_clocks", "silent_program_clock", case_silent_program_clock },
    { "mpts_clocks", "pcr_wrap",             case_pcr_wrap },
    { "mpts_media",  "media_overlap",        case_media_overlap },
    { "mpts_media",  "media_silent_program", case_media_silent_program },
    { "mpts_media",  "media_batches",        case_media_batches },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
