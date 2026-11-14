/* MPTS test cases. Everything is reassembled from wire bytes: no internal writer
 * state is read. CRC32/MPEG, section and PES parsing are implemented locally. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libmpegts.h"
#include "task_support/check.h"

#define TS_PKT 188
#define CLOCK27 27000000LL
#define TS_START_TICKS 270000000LL
#define PTS_OFFSET 900000LL /* TS_START (10s) in 90kHz units */
#define MAX_PID 8192

/* ------------------------------------------------------------------ helpers */

static uint32_t crc32_mpeg( const uint8_t *s, size_t n )
{
    uint32_t c = ~0u;
    for( size_t i = 0; i < n; i++ )
    {
        c ^= (uint32_t)s[i] << 24;
        for( int j = 0; j < 8; j++ )
            c = (c << 1) ^ ( (c & 0x80000000u) ? 0x04c11db7u : 0 );
    }
    return c;
}

static uint64_t ts_stamp( const uint8_t *s )
{
    return ((uint64_t)(s[0] & 14) << 29) | ((uint64_t)s[1] << 22) |
           ((uint64_t)(s[2] & 254) << 14) | ((uint64_t)s[3] << 7) | (s[4] >> 1);
}

static uint16_t rd16( const uint8_t *p ) { return ((uint16_t)p[0] << 8) | p[1]; }

typedef struct
{
    uint8_t *data;
    size_t len;
    int64_t *ticks;
    size_t npackets;
} trace_t;

static void trace_init( trace_t *t ) { memset( t, 0, sizeof(*t) ); }

static void trace_free( trace_t *t )
{
    free( t->data );
    free( t->ticks );
    memset( t, 0, sizeof(*t) );
}

static int trace_collect( trace_t *t, ts_writer_t *w, ts_frame_t *f, int n )
{
    uint8_t *out = NULL;
    int len = 0;
    int64_t *clock = NULL;
    if( ts_write_frames( w, f, n, &out, &len, &clock ) < 0 )
        return -1;
    if( len < 0 || len % TS_PKT != 0 )
        return -1;
    if( !len )
        return 0;

    uint8_t *nd = realloc( t->data, t->len + len );
    int64_t *nt = realloc( t->ticks, (t->npackets + len / TS_PKT) * sizeof(*nt) );
    if( !nd || !nt )
        return -1;
    t->data = nd;
    t->ticks = nt;
    memcpy( t->data + t->len, out, len );
    memcpy( t->ticks + t->npackets, clock, (size_t)len / TS_PKT * sizeof(*nt) );
    t->len += len;
    t->npackets += len / TS_PKT;
    return 0;
}

static void drain( trace_t *t, ts_writer_t *w )
{
    for( ;; )
    {
        size_t before = t->len;
        if( trace_collect( t, w, NULL, 0 ) < 0 ) break;
        if( t->len == before ) break;
    }
}

/* ------------------------------------------------------- wire reassembly */

typedef struct
{
    int pid;
    int table_id;
    size_t pkt;
    uint8_t data[2048];
    size_t len;
} section_rec_t;

typedef struct
{
    int pid;
    size_t first_pkt, last_pkt;
    uint8_t *payload;
    size_t payload_len;
    int64_t pts, dts;
    int has_dts;
    int random_access;
} pes_rec_t;

typedef struct
{
    int pid;
    size_t pkt;
    uint64_t value;
} pcr_rec_t;

typedef struct
{
    section_rec_t secs[256];
    int nsecs;
    pes_rec_t pes[1024];
    int npes;
    pcr_rec_t pcrs[16384];
    int npcrs;
    int cc_break;
    int bad_sync;
} reasm_t;

/* per-PID scratch for the PES currently in flight */
static int64_t g_pes_pts[MAX_PID], g_pes_dts[MAX_PID];
static int g_pes_has_dts[MAX_PID];

static void reasm_free( reasm_t *r )
{
    for( int i = 0; i < r->npes; i++ )
        free( r->pes[i].payload );
}

/* Reassemble PSI sections and PES, collect PCRs and audit continuity counters.
 * psi_pid[] flags PAT/PMT PIDs; everything else carrying payload is treated as PES. */
static int reassemble( reasm_t *r, const trace_t *t, const int psi_pid[MAX_PID] )
{
    /* 16 MiB: keep out of the stack (default stack is 8 MiB) */
    static uint8_t sec_buf[MAX_PID][2048];
    static size_t sec_len[MAX_PID];
    static int sec_active[MAX_PID];
    uint8_t *pes_buf[MAX_PID];
    size_t pes_len[MAX_PID], pes_cap[MAX_PID];
    size_t pes_first[MAX_PID], pes_last[MAX_PID];
    int pes_active[MAX_PID], pes_ra[MAX_PID];
    int cc_seen[MAX_PID], cc_last[MAX_PID];

    memset( sec_len, 0, sizeof(sec_len) );
    memset( sec_active, 0, sizeof(sec_active) );
    memset( pes_buf, 0, sizeof(pes_buf) );
    memset( pes_len, 0, sizeof(pes_len) );
    memset( pes_cap, 0, sizeof(pes_cap) );
    memset( pes_active, 0, sizeof(pes_active) );
    memset( cc_seen, 0, sizeof(cc_seen) );
    memset( g_pes_pts, 0, sizeof(g_pes_pts) );
    memset( g_pes_dts, 0, sizeof(g_pes_dts) );
    memset( g_pes_has_dts, 0, sizeof(g_pes_has_dts) );
    memset( r, 0, sizeof(*r) );

    int fail = 0;

    for( size_t k = 0; k < t->npackets && !fail; k++ )
    {
        const uint8_t *b = t->data + k * TS_PKT;
        if( b[0] != 0x47 ) { r->bad_sync = 1; fail = 1; break; }
        int pid = ((b[1] & 31) << 8) | b[2];
        int pusi = (b[1] >> 6) & 1;
        int afc = (b[3] >> 4) & 3;
        int cc = b[3] & 15;
        int pos = 4;

        if( pid != 0x1fff )
        {
            if( afc == 2 ) /* adaptation-only does not advance the payload CC */
            {
                if( cc_seen[pid] && cc != cc_last[pid] )
                    r->cc_break = 1, fail = 1;
            }
            else if( afc & 1 )
            {
                if( cc_seen[pid] && cc != ((cc_last[pid] + 1) & 15) )
                    r->cc_break = 1, fail = 1;
                cc_seen[pid] = 1;
                cc_last[pid] = cc;
            }
        }

        if( afc & 2 )
        {
            int afl = b[4];
            if( afl >= 7 && (b[5] & 0x10) )
            {
                uint64_t base = ((uint64_t)b[6] << 25) | ((uint64_t)b[7] << 17) |
                                ((uint64_t)b[8] << 9) | ((uint64_t)b[9] << 1) | (b[10] >> 7);
                uint64_t ext = ((uint64_t)(b[10] & 1) << 8) | b[11];
                if( r->npcrs < (int)(sizeof(r->pcrs) / sizeof(r->pcrs[0])) )
                    r->pcrs[r->npcrs++] = (pcr_rec_t){ pid, k, base * 300 + ext };
            }
            pos = 5 + afl;
        }
        if( fail ) break;
        if( !(afc & 1) || pos == TS_PKT )
            continue;

        int payload_len = TS_PKT - pos;
        const uint8_t *pl = b + pos;

        if( psi_pid[pid] )
        {
            if( pusi )
            {
                int pointer = pl[0];
                if( sec_active[pid] || pointer != 0 ) { r->cc_break = 1; fail = 1; break; }
                sec_active[pid] = 1;
                sec_len[pid] = 0;
                pl++;
                payload_len--;
            }
            if( !sec_active[pid] ) { r->cc_break = 1; fail = 1; break; }
            if( sec_len[pid] + payload_len > sizeof(sec_buf[pid]) ) { fail = 1; break; }
            memcpy( sec_buf[pid] + sec_len[pid], pl, payload_len );
            sec_len[pid] += payload_len;

            if( sec_len[pid] >= 3 )
            {
                size_t total = 3 + (rd16( sec_buf[pid] + 1 ) & 0x0fff);
                if( total > sizeof(sec_buf[pid]) ) { fail = 1; break; }
                if( sec_len[pid] >= total )
                {
                    if( r->nsecs >= (int)(sizeof(r->secs) / sizeof(r->secs[0])) ) { fail = 1; break; }
                    section_rec_t *s = &r->secs[r->nsecs++];
                    s->pid = pid;
                    s->table_id = sec_buf[pid][0];
                    s->pkt = k;
                    s->len = total;
                    memcpy( s->data, sec_buf[pid], total );
                    sec_active[pid] = 0;
                    sec_len[pid] = 0;
                }
            }
        }
        else if( pid != 0x1fff ) /* media PES */
        {
            if( pusi )
            {
                if( pes_active[pid] )
                {
                    if( r->npes >= (int)(sizeof(r->pes) / sizeof(r->pes[0])) ) { fail = 1; break; }
                    pes_rec_t *p = &r->pes[r->npes++];
                    p->pid = pid;
                    p->first_pkt = pes_first[pid];
                    p->last_pkt = pes_last[pid];
                    p->payload = pes_buf[pid];
                    p->payload_len = pes_len[pid];
                    p->pts = g_pes_pts[pid];
                    p->dts = g_pes_dts[pid];
                    p->has_dts = g_pes_has_dts[pid];
                    p->random_access = pes_ra[pid];
                    pes_buf[pid] = NULL;
                }
                pes_buf[pid] = malloc( 65536 );
                if( !pes_buf[pid] ) { fail = 1; break; }
                pes_cap[pid] = 65536;
                pes_len[pid] = 0;
                pes_active[pid] = 1;
                pes_first[pid] = pes_last[pid] = k;
                pes_ra[pid] = ( afc & 2 ) && b[4] >= 1 ? (b[5] >> 6) & 1 : 0;

                if( payload_len < 9 || pl[0] != 0 || pl[1] != 0 || pl[2] != 1 ) { fail = 1; break; }
                int hdr_data_len = pl[8];
                int pts_dts = pl[7] & 0xc0;
                int hdr_total = 9 + hdr_data_len;
                if( hdr_total > payload_len ) { fail = 1; break; }
                g_pes_pts[pid] = ts_stamp( pl + 9 );
                g_pes_has_dts[pid] = pts_dts == 0xc0;
                g_pes_dts[pid] = g_pes_has_dts[pid] ? ts_stamp( pl + 14 ) : g_pes_pts[pid];
                memcpy( pes_buf[pid], pl + hdr_total, payload_len - hdr_total );
                pes_len[pid] += payload_len - hdr_total;
            }
            else
            {
                if( !pes_active[pid] ) { r->cc_break = 1; fail = 1; break; }
                pes_last[pid] = k;
                if( pes_len[pid] + payload_len > pes_cap[pid] )
                {
                    uint8_t *nb = realloc( pes_buf[pid], pes_cap[pid] * 2 );
                    if( !nb ) { fail = 1; break; }
                    pes_buf[pid] = nb;
                    pes_cap[pid] *= 2;
                }
                memcpy( pes_buf[pid] + pes_len[pid], pl, payload_len );
                pes_len[pid] += payload_len;
            }
        }
    }

    if( !fail )
    {
        for( int pid = 0; pid < MAX_PID; pid++ )
        {
            if( !pes_active[pid] )
                continue;
            if( r->npes >= (int)(sizeof(r->pes) / sizeof(r->pes[0])) ) { fail = 1; break; }
            pes_rec_t *p = &r->pes[r->npes++];
            p->pid = pid;
            p->first_pkt = pes_first[pid];
            p->last_pkt = pes_last[pid];
            p->payload = pes_buf[pid];
            p->payload_len = pes_len[pid];
            p->pts = g_pes_pts[pid];
            p->dts = g_pes_dts[pid];
            p->has_dts = g_pes_has_dts[pid];
            p->random_access = pes_ra[pid];
            pes_buf[pid] = NULL;
        }
    }

    if( fail )
    {
        for( int pid = 0; pid < MAX_PID; pid++ )
            free( pes_buf[pid] );
        reasm_free( r );
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------- section parsing */

typedef struct
{
    int program_num;
    int pmt_pid;
} pat_entry_t;

static int parse_pat( const section_rec_t *s, pat_entry_t *out, int max )
{
    if( s->table_id != 0x00 ) return -1;
    size_t end = s->len - 4; /* CRC */
    size_t pos = 8;
    int n = 0;
    while( pos + 4 <= end )
    {
        int prog = rd16( s->data + pos );
        int pid = rd16( s->data + pos + 2 ) & 0x1fff;
        pos += 4;
        if( prog == 0 ) continue; /* network entry */
        if( n >= max ) return -1;
        out[n].program_num = prog;
        out[n].pmt_pid = pid;
        n++;
    }
    return n;
}

typedef struct
{
    int stream_type;
    int es_pid;
    int es_info_len;
    int has_alignment_desc;
    int has_avc_desc;
    int has_lang_desc;
} pmt_es_t;

typedef struct
{
    int program_num;
    int pcr_pid;
    int program_info_len;
    pmt_es_t es[64];
    int nes;
} pmt_parsed_t;

static int parse_pmt( const section_rec_t *s, pmt_parsed_t *p )
{
    if( s->table_id != 0x02 ) return -1;
    memset( p, 0, sizeof(*p) );
    p->program_num = rd16( s->data + 3 );
    p->pcr_pid = rd16( s->data + 8 ) & 0x1fff;
    p->program_info_len = rd16( s->data + 10 ) & 0x0fff;
    size_t pos = 12 + p->program_info_len;
    size_t end = s->len - 4;

    while( pos + 5 <= end )
    {
        if( p->nes >= (int)(sizeof(p->es) / sizeof(p->es[0])) ) return -1;
        pmt_es_t *e = &p->es[p->nes++];
        e->stream_type = s->data[pos];
        e->es_pid = rd16( s->data + pos + 1 ) & 0x1fff;
        e->es_info_len = rd16( s->data + pos + 3 ) & 0x0fff;
        size_t dpos = pos + 5;
        size_t dend = dpos + e->es_info_len;
        if( dend > end ) return -1;
        while( dpos + 2 <= dend )
        {
            int tag = s->data[dpos];
            int dlen = s->data[dpos + 1];
            if( tag == 0x06 ) e->has_alignment_desc = 1;
            if( tag == 0x28 ) e->has_avc_desc = 1;
            if( tag == 0x0a ) e->has_lang_desc = 1;
            dpos += 2 + dlen;
        }
        if( dpos != dend ) return -1;
        pos = dend;
    }
    return pos == end ? 0 : -1;
}

static const section_rec_t *find_section( const reasm_t *r, int pid, int occurrence )
{
    int seen = 0;
    for( int i = 0; i < r->nsecs; i++ )
    {
        if( r->secs[i].pid != pid ) continue;
        if( seen++ == occurrence ) return &r->secs[i];
    }
    return NULL;
}

static int count_sections( const reasm_t *r, int pid )
{
    int n = 0;
    for( int i = 0; i < r->nsecs; i++ )
        if( r->secs[i].pid == pid ) n++;
    return n;
}

/* ------------------------------------------------------- fixtures */

typedef struct
{
    int program_num;
    int pmt_pid;
    int pcr_pid;     /* may equal video_pid */
    int video_pid;
    int video_format;
    int audio_pid;   /* 0 disables audio */
    int extra_audios;/* extra audio ES for long-PMT cases */
} prog_def_t;

typedef struct
{
    ts_main_t main_cfg;
    ts_program_t programs[8];
    ts_stream_t streams[8][64];
    int nstreams[8];
    int psi_pid[MAX_PID];
    int nprograms;
} fixture_t;

static int build_fixture( fixture_t *fx, const prog_def_t *defs, int n, int muxrate,
                          int pcr_period, int pat_period, int cbr, int lowlatency, int ts_type )
{
    memset( fx, 0, sizeof(*fx) );
    fx->nprograms = n;

    for( int p = 0; p < n; p++ )
    {
        const prog_def_t *d = &defs[p];
        ts_stream_t *arr = fx->streams[p];
        int ns = 0;

        arr[ns].pid = d->video_pid;
        arr[ns].stream_format = d->video_format;
        arr[ns].stream_id = LIBMPEGTS_STREAM_ID_MPEGVIDEO;
        ns++;

        if( d->audio_pid )
        {
            arr[ns].pid = d->audio_pid;
            arr[ns].stream_format = LIBMPEGTS_AUDIO_MPEG1;
            arr[ns].stream_id = LIBMPEGTS_STREAM_ID_MPEGAUDIO;
            arr[ns].audio_frame_size = 2160;
            ns++;
        }
        for( int e = 0; e < d->extra_audios; e++ )
        {
            arr[ns].pid = d->video_pid + 0x40 + e;
            arr[ns].stream_format = LIBMPEGTS_AUDIO_MPEG2;
            arr[ns].stream_id = LIBMPEGTS_STREAM_ID_MPEGAUDIO;
            arr[ns].audio_frame_size = 2160;
            ns++;
        }

        fx->nstreams[p] = ns;
        fx->programs[p].pmt_pid = d->pmt_pid;
        fx->programs[p].program_num = d->program_num;
        fx->programs[p].pcr_pid = d->pcr_pid;
        fx->programs[p].num_streams = ns;
        fx->programs[p].streams = arr;
        fx->psi_pid[d->pmt_pid] = 1;
    }
    fx->psi_pid[0] = 1; /* PAT */

    fx->main_cfg.num_programs = n;
    fx->main_cfg.programs = fx->programs;
    fx->main_cfg.ts_id = 23;
    fx->main_cfg.muxrate = muxrate;
    fx->main_cfg.cbr = cbr;
    fx->main_cfg.lowlatency = lowlatency;
    fx->main_cfg.ts_type = ts_type;
    fx->main_cfg.pcr_period = pcr_period;
    fx->main_cfg.pat_period = pat_period;
    return 0;
}

static ts_writer_t *open_fixture( fixture_t *fx )
{
    ts_writer_t *w = ts_create_writer();
    if( !w ) return NULL;
    if( ts_setup_transport_stream( w, &fx->main_cfg ) < 0 )
    {
        ts_close_writer( w );
        return NULL;
    }
    for( int p = 0; p < fx->nprograms; p++ )
        for( int i = 0; i < fx->nstreams[p]; i++ )
        {
            ts_stream_t *s = &fx->streams[p][i];
            if( s->stream_format != LIBMPEGTS_VIDEO_MPEG2 && s->stream_format != LIBMPEGTS_VIDEO_AVC )
                continue;
            if( s->stream_format == LIBMPEGTS_VIDEO_MPEG2 &&
                ts_setup_mpegvideo_stream( w, s->pid, LIBMPEGTS_MPEG2_LEVEL_MAIN,
                                           LIBMPEGTS_MPEG2_PROFILE_MAIN, 1000000, 100000, 3 ) < 0 )
            { ts_close_writer( w ); return NULL; }
            if( s->stream_format == LIBMPEGTS_VIDEO_AVC &&
                ts_setup_mpegvideo_stream( w, s->pid, 30, AVC_MAIN, 1000000, 100000, 0 ) < 0 )
            { ts_close_writer( w ); return NULL; }
        }
    return w;
}

static void fill_body( uint8_t *body, int size, int pid, int idx )
{
    for( int j = 0; j < size; j++ )
        body[j] = (uint8_t)(j * 17 + pid * 7 + idx * 31);
}

static void make_video_frame( ts_frame_t *f, int pid, int idx, int dts,
                              int64_t iarr, int64_t farr, int ra, uint8_t *body, int size )
{
    memset( f, 0, sizeof(*f) );
    fill_body( body, size, pid, idx );
    f->pid = pid;
    f->data = body;
    f->size = size;
    f->dts = dts;
    f->pts = dts;
    f->cpb_initial_arrival_time = iarr;
    f->cpb_final_arrival_time = farr;
    f->random_access = ra;
}

/* CPB window derived from the 90kHz DTS exactly like the historical single-program
 * fixture: arrival opens 8100 ticks after the DTS base and closes 5400 ticks before
 * the DTS, so final arrival always precedes the DTS */
static void make_aligned_video_frame( ts_frame_t *f, int pid, int idx, int dts90,
                                      int ra, uint8_t *body, int size )
{
    make_video_frame( f, pid, idx, dts90,
                      (int64_t)(dts90 - 8100) * 300, (int64_t)(dts90 - 5400) * 300,
                      ra, body, size );
}

static void aligned_window( int dts90, int64_t *iarr, int64_t *farr )
{
    *iarr = (int64_t)(dts90 - 8100) * 300 + TS_START_TICKS;
    *farr = (int64_t)(dts90 - 5400) * 300 + TS_START_TICKS;
}

static void make_audio_frame( ts_frame_t *f, int pid, int idx, int dts, uint8_t *body, int size )
{
    memset( f, 0, sizeof(*f) );
    fill_body( body, size, pid, idx );
    f->pid = pid;
    f->data = body;
    f->size = size;
    f->dts = dts;
    f->pts = dts;
}

/* declared arrival window of a video frame, shifted onto the muxer clock */
typedef struct
{
    int64_t iarr, farr;
    int64_t dts;
    int ra;
} frame_expect_t;

/* ------------------------------------------------------- generic checks */

static int check_common_clock( const trace_t *t, int muxrate )
{
    for( size_t k = 0; k < t->npackets; k++ )
    {
        int64_t expect = TS_START_TICKS +
            ((int64_t)(k + 1) * TS_PKT * 8 * CLOCK27 + muxrate / 2) / muxrate;
        TASK_CHECK( t->ticks[k] == expect );
    }
    return 0;
}

static int check_pcr_encoding_and_spacing( const reasm_t *r, const trace_t *t,
                                           const int pcr_pid[8], int nprog,
                                           int muxrate, int pcr_period )
{
    int64_t pkt_time = (int64_t)TS_PKT * 8 * CLOCK27 / muxrate;
    /* PCR is sampled at byte 10; pcr_list entries are packet-end times */
    int64_t tail = ((int64_t)(TS_PKT - 10) * 8 * CLOCK27 + muxrate / 2) / muxrate;
    int64_t last_val[MAX_PID];
    int have_last[MAX_PID] = {0};
    int count_per_pid[MAX_PID] = {0};

    for( int i = 0; i < r->npcrs; i++ )
    {
        const pcr_rec_t *x = &r->pcrs[i];
        int known = 0;
        for( int p = 0; p < nprog; p++ )
            if( x->pid == pcr_pid[p] ) known = 1;
        TASK_CHECK( known );

        int64_t expect_val = t->ticks[x->pkt] - tail;
        TASK_CHECK( llabs( (long long)(x->value - expect_val) ) <= 1 );
        TASK_CHECK( x->value % 300 < 300 );

        if( have_last[x->pid] )
        {
            int64_t gap = x->value - last_val[x->pid];
            /* period plus one slot, plus N-1 extra slots when programs expire together */
            TASK_CHECK( gap <= (int64_t)pcr_period * 27000 + nprog * pkt_time );
            TASK_CHECK( gap > 0 );
        }
        last_val[x->pid] = x->value;
        have_last[x->pid] = 1;
        count_per_pid[x->pid]++;
    }

    for( int p = 0; p < nprog; p++ )
        TASK_CHECK( count_per_pid[pcr_pid[p]] >= 2 );
    return 0;
}

/* first media payload packet index per PID */
static size_t first_media_packet( const trace_t *t, int pid )
{
    for( size_t k = 0; k < t->npackets; k++ )
    {
        const uint8_t *b = t->data + k * TS_PKT;
        if( (((b[1] & 31) << 8) | b[2]) != pid ) continue;
        int afc = (b[3] >> 4) & 3;
        if( afc & 1 ) return k;
    }
    return (size_t)-1;
}

/* ============================================================ mpts_tables */

static int case_pat_and_pmts( void )
{
    prog_def_t defs[2] = {
        { 7,  0x1000, 0x110,  0x100, LIBMPEGTS_VIDEO_MPEG2, 0x101, 0 },
        { 8,  0x1010, 0x200,  0x200, LIBMPEGTS_VIDEO_AVC,   0x201, 0 },
    };
    fixture_t fx;
    build_fixture( &fx, defs, 2, 12000000, 20, 80, 1, 1, TS_TYPE_GENERIC );
    ts_writer_t *w = open_fixture( &fx );
    TASK_CHECK( w );

    trace_t t; trace_init( &t );
    enum { NPAT_FR = 4 };
    uint8_t bodies[2 * NPAT_FR][300];
    ts_frame_t batch[2 * NPAT_FR];
    int nb = 0;
    for( int i = 0; i < NPAT_FR; i++ )
    {
        make_aligned_video_frame( &batch[nb++], 0x100, i, 9000 + i * 3600, i == 0,
                                  bodies[2 * i], sizeof(bodies[2 * i]) );
        make_aligned_video_frame( &batch[nb++], 0x200, i, 9000 + i * 3600 + 1800, i == 0,
                                  bodies[2 * i + 1], sizeof(bodies[2 * i + 1]) );
    }
    TASK_CHECK( trace_collect( &t, w, batch, nb ) == 0 );
    drain( &t, w );

    reasm_t r;
    TASK_CHECK( reassemble( &r, &t, fx.psi_pid ) == 0 );
    TASK_CHECK( !r.cc_break && !r.bad_sync );

    /* PAT lists both programs with their PMT PIDs */
    const section_rec_t *pat = find_section( &r, 0, 0 );
    TASK_CHECK( pat );
    TASK_CHECK( crc32_mpeg( pat->data, pat->len ) == 0 );
    pat_entry_t entries[8];
    int npat = parse_pat( pat, entries, 8 );
    TASK_CHECK( npat == 2 );
    TASK_CHECK( entries[0].program_num == 7 && entries[0].pmt_pid == 0x1000 );
    TASK_CHECK( entries[1].program_num == 8 && entries[1].pmt_pid == 0x1010 );

    /* each PMT lists only its own ES and its PCR PID; descriptors survive */
    const section_rec_t *pmt0 = find_section( &r, 0x1000, 0 );
    const section_rec_t *pmt1 = find_section( &r, 0x1010, 0 );
    TASK_CHECK( pmt0 && pmt1 );
    TASK_CHECK( crc32_mpeg( pmt0->data, pmt0->len ) == 0 );
    TASK_CHECK( crc32_mpeg( pmt1->data, pmt1->len ) == 0 );

    pmt_parsed_t p0, p1;
    TASK_CHECK( parse_pmt( pmt0, &p0 ) == 0 );
    TASK_CHECK( parse_pmt( pmt1, &p1 ) == 0 );
    TASK_CHECK( p0.program_num == 7 && p0.pcr_pid == 0x110 && p0.nes == 2 );
    TASK_CHECK( p1.program_num == 8 && p1.pcr_pid == 0x200 && p1.nes == 2 );

    int saw_v0 = 0, saw_a0 = 0, saw_v1 = 0, saw_a1 = 0;
    for( int i = 0; i < p0.nes; i++ )
    {
        TASK_CHECK( p0.es[i].es_pid == 0x100 || p0.es[i].es_pid == 0x101 );
        TASK_CHECK( p0.es[i].has_alignment_desc );
        if( p0.es[i].es_pid == 0x100 ) { saw_v0 = 1; TASK_CHECK( p0.es[i].stream_type == 0x02 ); }
        if( p0.es[i].es_pid == 0x101 ) { saw_a0 = 1; TASK_CHECK( p0.es[i].stream_type == 0x03 ); TASK_CHECK( p0.es[i].has_lang_desc == 0 ); }
    }
    for( int i = 0; i < p1.nes; i++ )
    {
        TASK_CHECK( p1.es[i].es_pid == 0x200 || p1.es[i].es_pid == 0x201 );
        TASK_CHECK( p1.es[i].has_alignment_desc );
        if( p1.es[i].es_pid == 0x200 ) { saw_v1 = 1; TASK_CHECK( p1.es[i].stream_type == 0x1b ); TASK_CHECK( p1.es[i].has_avc_desc ); }
        if( p1.es[i].es_pid == 0x201 ) { saw_a1 = 1; TASK_CHECK( p1.es[i].stream_type == 0x03 ); }
    }
    TASK_CHECK( saw_v0 && saw_a0 && saw_v1 && saw_a1 );

    /* PAT/PMT retransmitted as a set on pat_period */
    TASK_CHECK( count_sections( &r, 0 ) >= 2 );
    TASK_CHECK( count_sections( &r, 0x1000 ) == count_sections( &r, 0 ) );
    TASK_CHECK( count_sections( &r, 0x1010 ) == count_sections( &r, 0 ) );

    /* the first complete PAT and every first complete PMT precede the first media packet */
    size_t first_media = first_media_packet( &t, 0x100 );
    TASK_CHECK( first_media != (size_t)-1 );
    TASK_CHECK( find_section( &r, 0, 0 )->pkt < first_media );
    TASK_CHECK( find_section( &r, 0x1000, 0 )->pkt < first_media );
    TASK_CHECK( find_section( &r, 0x1010, 0 )->pkt < first_media );

    reasm_free( &r );
    trace_free( &t );
    TASK_CHECK( ts_close_writer( w ) == 0 );
    return 0;
}

static int case_long_pmt_multipacket( void )
{
    /* one video plus enough audio ES to push the PMT across several TS packets */
    prog_def_t defs[2] = {
        { 1, 0x1000, 0x100, 0x100, LIBMPEGTS_VIDEO_MPEG2, 0x101, 30 },
        { 2, 0x1010, 0x300, 0x300, LIBMPEGTS_VIDEO_AVC,   0x301, 0  },
    };
    fixture_t fx;
    build_fixture( &fx, defs, 2, 16000000, 20, 80, 1, 1, TS_TYPE_GENERIC );
    ts_writer_t *w = open_fixture( &fx );
    TASK_CHECK( w );

    trace_t t; trace_init( &t );
    uint8_t body0[200], body1[200];
    ts_frame_t fr[2];
    make_aligned_video_frame( &fr[0], 0x100, 0, 9000, 1, body0, sizeof(body0) );
    make_aligned_video_frame( &fr[1], 0x300, 0, 9000, 1, body1, sizeof(body1) );
    TASK_CHECK( trace_collect( &t, w, fr, 2 ) == 0 );
    drain( &t, w );

    reasm_t r;
    TASK_CHECK( reassemble( &r, &t, fx.psi_pid ) == 0 );
    TASK_CHECK( !r.cc_break );

    const section_rec_t *pmt = find_section( &r, 0x1000, 0 );
    TASK_CHECK( pmt );
    TASK_CHECK( pmt->len > 183 );                 /* does not fit in one packet payload */
    TASK_CHECK( crc32_mpeg( pmt->data, pmt->len ) == 0 );
    pmt_parsed_t pp;
    TASK_CHECK( parse_pmt( pmt, &pp ) == 0 );
    TASK_CHECK( pp.nes == 32 );                  /* 1 video + 31 audio */
    TASK_CHECK( pp.pcr_pid == 0x100 );

    /* long PMT finished before any media packet */
    size_t fm = first_media_packet( &t, 0x100 );
    TASK_CHECK( fm != (size_t)-1 );
    TASK_CHECK( pmt->pkt < fm );

    reasm_free( &r );
    trace_free( &t );
    TASK_CHECK( ts_close_writer( w ) == 0 );
    return 0;
}

static int case_config_conflicts( void )
{
    /* duplicate program number */
    {
        prog_def_t defs[2] = {
            { 7, 0x1000, 0x110, 0x100, LIBMPEGTS_VIDEO_MPEG2, 0x101, 0 },
            { 7, 0x1010, 0x200, 0x200, LIBMPEGTS_VIDEO_AVC,   0x201, 0 },
        };
        fixture_t fx;
        build_fixture( &fx, defs, 2, 12000000, 20, 80, 1, 1, TS_TYPE_GENERIC );
        ts_writer_t *w = ts_create_writer();
        TASK_CHECK( ts_setup_transport_stream( w, &fx.main_cfg ) < 0 );
        TASK_CHECK( ts_close_writer( w ) == 0 );
    }
    /* PMT PID collides with an ES PID */
    {
        prog_def_t defs[2] = {
            { 7, 0x200, 0x110, 0x100, LIBMPEGTS_VIDEO_MPEG2, 0x101, 0 },
            { 8, 0x1010, 0x200, 0x200, LIBMPEGTS_VIDEO_AVC,   0x201, 0 },
        };
        fixture_t fx;
        build_fixture( &fx, defs, 2, 12000000, 20, 80, 1, 1, TS_TYPE_GENERIC );
        ts_writer_t *w = ts_create_writer();
        TASK_CHECK( ts_setup_transport_stream( w, &fx.main_cfg ) < 0 );
        TASK_CHECK( ts_close_writer( w ) == 0 );
    }
    /* ES PID shared across programs */
    {
        prog_def_t defs[2] = {
            { 7, 0x1000, 0x110, 0x100, LIBMPEGTS_VIDEO_MPEG2, 0x101, 0 },
            { 8, 0x1010, 0x200, 0x100, LIBMPEGTS_VIDEO_AVC,   0x201, 0 },
        };
        fixture_t fx;
        build_fixture( &fx, defs, 2, 12000000, 20, 80, 1, 1, TS_TYPE_GENERIC );
        ts_writer_t *w = ts_create_writer();
        TASK_CHECK( ts_setup_transport_stream( w, &fx.main_cfg ) < 0 );
        TASK_CHECK( ts_close_writer( w ) == 0 );
    }
    /* standalone PCR PID collides with network PID */
    {
        prog_def_t defs[2] = {
            { 7, 0x1000, 0x50, 0x100, LIBMPEGTS_VIDEO_MPEG2, 0x101, 0 },
            { 8, 0x1010, 0x200, 0x200, LIBMPEGTS_VIDEO_AVC,   0x201, 0 },
        };
        fixture_t fx;
        build_fixture( &fx, defs, 2, 12000000, 20, 80, 1, 1, TS_TYPE_GENERIC );
        fx.main_cfg.network_pid = 0x50;
        ts_writer_t *w = ts_create_writer();
        TASK_CHECK( ts_setup_transport_stream( w, &fx.main_cfg ) < 0 );
        TASK_CHECK( ts_close_writer( w ) == 0 );
    }
    /* PID below 0x20 */
    {
        prog_def_t defs[2] = {
            { 7, 0x1000, 0x110, 0x100, LIBMPEGTS_VIDEO_MPEG2, 0x101, 0 },
            { 8, 0x1010, 0x200, 0x200, LIBMPEGTS_VIDEO_AVC,   0x201, 0 },
        };
        fixture_t fx;
        build_fixture( &fx, defs, 2, 12000000, 20, 80, 1, 1, TS_TYPE_GENERIC );
        fx.programs[1].pcr_pid = 0x1f;
        ts_writer_t *w = ts_create_writer();
        TASK_CHECK( ts_setup_transport_stream( w, &fx.main_cfg ) < 0 );
        TASK_CHECK( ts_close_writer( w ) == 0 );
    }
    /* unsupported MPTS modes: VBR, non-generic, no lowlatency, too many programs */
    for( int mode = 0; mode < 4; mode++ )
    {
        prog_def_t defs[2] = {
            { 7, 0x1000, 0x110, 0x100, LIBMPEGTS_VIDEO_MPEG2, 0x101, 0 },
            { 8, 0x1010, 0x200, 0x200, LIBMPEGTS_VIDEO_AVC,   0x201, 0 },
        };
        fixture_t fx;
        build_fixture( &fx, defs, 2, 12000000, 20, 80, 1, 1, TS_TYPE_GENERIC );
        if( mode == 0 ) fx.main_cfg.cbr = 0;
        if( mode == 1 ) fx.main_cfg.ts_type = TS_TYPE_DVB;
        if( mode == 2 ) fx.main_cfg.lowlatency = 0;
        if( mode == 3 ) fx.main_cfg.num_programs = 9;
        ts_writer_t *w = ts_create_writer();
        TASK_CHECK( ts_setup_transport_stream( w, &fx.main_cfg ) < 0 );
        TASK_CHECK( ts_close_writer( w ) == 0 );
    }
    return 0;
}

/* ============================================================ mpts_clocks */

static int case_per_program_pcrs( void )
{
    prog_def_t defs[2] = {
        { 7, 0x1000, 0x110, 0x100, LIBMPEGTS_VIDEO_MPEG2, 0x101, 0 }, /* standalone PCR */
        { 8, 0x1010, 0x200, 0x200, LIBMPEGTS_VIDEO_AVC,   0x201, 0 }, /* PCR on ES */
    };
    fixture_t fx;
    build_fixture( &fx, defs, 2, 12000000, 20, 80, 1, 1, TS_TYPE_GENERIC );
    ts_writer_t *w = open_fixture( &fx );
    TASK_CHECK( w );

    trace_t t; trace_init( &t );
    uint8_t body0[400], body1[400];
    for( int i = 0; i < 8; i++ )
    {
        ts_frame_t fr[2];
        make_aligned_video_frame( &fr[0], 0x100, i, 9000 + i * 3600, i == 0, body0, sizeof(body0) );
        make_aligned_video_frame( &fr[1], 0x200, i, 9000 + i * 3600 + 1800, i == 0, body1, sizeof(body1) );
        TASK_CHECK( trace_collect( &t, w, fr, 2 ) == 0 );
    }
    drain( &t, w );

    reasm_t r;
    TASK_CHECK( reassemble( &r, &t, fx.psi_pid ) == 0 );
    TASK_CHECK( !r.cc_break && !r.bad_sync );
    TASK_CHECK( check_common_clock( &t, 12000000 ) == 0 );

    int pcr_pid[2] = { 0x110, 0x200 };
    TASK_CHECK( check_pcr_encoding_and_spacing( &r, &t, pcr_pid, 2, 12000000, 20 ) == 0 );

    /* first round: one initial PCR per program, before any media packet */
    TASK_CHECK( r.npcrs >= 2 );
    TASK_CHECK( r.pcrs[0].pid == 0x110 && r.pcrs[1].pid == 0x200 );
    size_t fm0 = first_media_packet( &t, 0x100 );
    size_t fm1 = first_media_packet( &t, 0x200 );
    TASK_CHECK( r.pcrs[0].pkt < fm0 && r.pcrs[1].pkt < fm1 );

    /* standalone PCR PID only ever sends adaptation-only packets: CC never moves */
    for( size_t k = 0; k < t.npackets; k++ )
    {
        const uint8_t *b = t.data + k * TS_PKT;
        if( (((b[1] & 31) << 8) | b[2]) != 0x110 ) continue;
        TASK_CHECK( ((b[3] >> 4) & 3) == 2 );
    }

    reasm_free( &r );
    trace_free( &t );
    TASK_CHECK( ts_close_writer( w ) == 0 );
    return 0;
}

static int case_silent_program_pcr( void )
{
    prog_def_t defs[2] = {
        { 7, 0x1000, 0x110, 0x100, LIBMPEGTS_VIDEO_MPEG2, 0, 0 },
        { 8, 0x1010, 0x210, 0x200, LIBMPEGTS_VIDEO_AVC,   0, 0 },
    };
    fixture_t fx;
    build_fixture( &fx, defs, 2, 12000000, 20, 80, 1, 1, TS_TYPE_GENERIC );
    ts_writer_t *w = open_fixture( &fx );
    TASK_CHECK( w );

    trace_t t; trace_init( &t );
    uint8_t body[300];
    /* only program 0 is fed for the whole run; program 1 stays silent */
    for( int i = 0; i < 8; i++ )
    {
        ts_frame_t fr;
        make_aligned_video_frame( &fr, 0x100, i, 9000 + i * 3600, i == 0, body, sizeof(body) );
        TASK_CHECK( trace_collect( &t, w, &fr, 1 ) == 0 );
    }
    drain( &t, w );

    reasm_t r;
    TASK_CHECK( reassemble( &r, &t, fx.psi_pid ) == 0 );
    TASK_CHECK( !r.cc_break );
    TASK_CHECK( check_common_clock( &t, 12000000 ) == 0 );

    int pcr_pid[2] = { 0x110, 0x210 };
    TASK_CHECK( check_pcr_encoding_and_spacing( &r, &t, pcr_pid, 2, 12000000, 20 ) == 0 );

    /* the silent program still got several PCRs, on its own PID */
    int silent_pcrs = 0;
    for( int i = 0; i < r.npcrs; i++ )
        if( r.pcrs[i].pid == 0x210 ) silent_pcrs++;
    TASK_CHECK( silent_pcrs >= 3 );

    reasm_free( &r );
    trace_free( &t );
    TASK_CHECK( ts_close_writer( w ) == 0 );
    return 0;
}

/* ============================================================ mpts_media */

static int case_overlapping_windows( void )
{
    prog_def_t defs[2] = {
        { 7, 0x1000, 0x110, 0x100, LIBMPEGTS_VIDEO_MPEG2, 0x101, 0 },
        { 8, 0x1010, 0x200, 0x200, LIBMPEGTS_VIDEO_AVC,   0x201, 0 },
    };
    fixture_t fx;
    const int muxrate = 12000000;
    build_fixture( &fx, defs, 2, muxrate, 20, 80, 1, 1, TS_TYPE_GENERIC );
    ts_writer_t *w = open_fixture( &fx );
    TASK_CHECK( w );

    enum { NFR = 6 };
    frame_expect_t exp0[NFR], exp1[NFR];
    trace_t t; trace_init( &t );
    /* one body per frame: a single interleaved submission keeps every frame queued
     * before its arrival window opens, so no frame is ever late */
    uint8_t vbodies[2 * NFR][500], abodies[2 * NFR][120];
    ts_frame_t batch[4 * NFR];
    int nb = 0;

    for( int i = 0; i < NFR; i++ )
    {
        int d0 = 9000 + i * 3600;
        int d1 = 9000 + i * 3600 + 1800;   /* half a frame later, so the CPB windows overlap */
        int64_t i0, f0, i1, f1;
        aligned_window( d0, &i0, &f0 );
        aligned_window( d1, &i1, &f1 );
        exp0[i] = (frame_expect_t){ i0, f0, d0, i == 0 };
        exp1[i] = (frame_expect_t){ i1, f1, d1, i == 0 };

        make_aligned_video_frame( &batch[nb++], 0x100, i, d0, i == 0, vbodies[2 * i], 500 );
        make_aligned_video_frame( &batch[nb++], 0x200, i, d1, i == 0, vbodies[2 * i + 1], 500 );
        make_audio_frame( &batch[nb++], 0x101, i, d0, abodies[2 * i], 120 );
        make_audio_frame( &batch[nb++], 0x201, i, d1, abodies[2 * i + 1], 120 );
    }
    TASK_CHECK( trace_collect( &t, w, batch, nb ) == 0 );
    drain( &t, w );

    reasm_t r;
    TASK_CHECK( reassemble( &r, &t, fx.psi_pid ) == 0 );
    TASK_CHECK( !r.cc_break && !r.bad_sync );
    TASK_CHECK( check_common_clock( &t, muxrate ) == 0 );

    int64_t pkt_time = (int64_t)TS_PKT * 8 * CLOCK27 / muxrate;
    const int vlen = 500, alen = 120;
    int n0 = 0, n1 = 0, na0 = 0, na1 = 0;
    int64_t prev_pts[8192]; int seen_pts[8192] = {0};

    for( int i = 0; i < r.npes; i++ )
    {
        const pes_rec_t *p = &r.pes[i];
        /* same-PID PES order must be preserved and PTS strictly increasing */
        if( seen_pts[p->pid] )
            TASK_CHECK( p->pts > prev_pts[p->pid] );
        prev_pts[p->pid] = p->pts;
        seen_pts[p->pid] = 1;

        int64_t first_tick = t.ticks[p->first_pkt];
        int64_t last_tick = t.ticks[p->last_pkt];

        if( p->pid == 0x100 )
        {
            frame_expect_t *e = &exp0[n0++];
            TASK_CHECK( p->payload_len == vlen );
            TASK_CHECK( (int64_t)p->pts == (e->dts + PTS_OFFSET) % (1LL << 33) );
            /* equal PTS/DTS is encoded PTS-only, and the parser mirrors it into dts */
            TASK_CHECK( p->dts == p->pts );
            TASK_CHECK( p->random_access == e->ra );
            TASK_CHECK( first_tick >= e->iarr - pkt_time && first_tick <= e->farr + pkt_time );
            TASK_CHECK( last_tick  >= e->iarr - pkt_time && last_tick  <= e->farr + pkt_time );
            TASK_CHECK( last_tick >= first_tick );
            for( int j = 0; j < vlen; j++ )
                TASK_CHECK( p->payload[j] == (uint8_t)(j * 17 + 0x100 * 7 + (n0 - 1) * 31) );
        }
        else if( p->pid == 0x200 )
        {
            frame_expect_t *e = &exp1[n1++];
            TASK_CHECK( p->payload_len == vlen );
            TASK_CHECK( (int64_t)p->pts == (e->dts + PTS_OFFSET) % (1LL << 33) );
            TASK_CHECK( p->random_access == e->ra );
            TASK_CHECK( first_tick >= e->iarr - pkt_time && first_tick <= e->farr + pkt_time );
            TASK_CHECK( last_tick  >= e->iarr - pkt_time && last_tick  <= e->farr + pkt_time );
            for( int j = 0; j < vlen; j++ )
                TASK_CHECK( p->payload[j] == (uint8_t)(j * 17 + 0x200 * 7 + (n1 - 1) * 31) );
        }
        else if( p->pid == 0x101 )
        {
            na0++;
            /* audio earliest-arrival constraint: not before dts*300 - max_frame_size*300 */
            int64_t earliest = (p->dts * 300) - 2160 * 300;
            TASK_CHECK( first_tick >= earliest - pkt_time );
            TASK_CHECK( p->payload_len == alen );
        }
        else if( p->pid == 0x201 )
        {
            na1++;
            int64_t earliest = (p->dts * 300) - 2160 * 300;
            TASK_CHECK( first_tick >= earliest - pkt_time );
        }
    }
    TASK_CHECK( n0 == NFR && n1 == NFR && na0 == NFR && na1 == NFR );

    reasm_free( &r );
    trace_free( &t );
    TASK_CHECK( ts_close_writer( w ) == 0 );
    return 0;
}

static int case_batched_submission( void )
{
    prog_def_t defs[2] = {
        { 7, 0x1000, 0x110, 0x100, LIBMPEGTS_VIDEO_MPEG2, 0, 0 },
        { 8, 0x1010, 0x200, 0x200, LIBMPEGTS_VIDEO_AVC,   0, 0 },
    };
    fixture_t fx;
    const int muxrate = 12000000;
    build_fixture( &fx, defs, 2, muxrate, 20, 80, 1, 1, TS_TYPE_GENERIC );
    ts_writer_t *w = open_fixture( &fx );
    TASK_CHECK( w );

    trace_t t; trace_init( &t );
    uint8_t body[350];
    int64_t advanced = 0;

    /* batch 1: program 0 only, one frame per call */
    for( int i = 0; i < 3; i++ )
    {
        ts_frame_t fr;
        make_aligned_video_frame( &fr, 0x100, i, 9000 + i * 3600, i == 0, body, sizeof(body) );
        TASK_CHECK( trace_collect( &t, w, &fr, 1 ) == 0 );
    }
    advanced = t.ticks[t.npackets - 1];

    /* batch 2: interleaved frames from both programs, arrivals no earlier than advanced */
    for( int i = 3; i < 6; i++ )
    {
        ts_frame_t fr[2];
        uint8_t body0[350], body1[350];
        int d0 = 9000 + i * 3600;
        int d1 = 9000 + i * 3600 + 1800;
        int64_t i0, f0, i1, f1;
        aligned_window( d0, &i0, &f0 );
        aligned_window( d1, &i1, &f1 );
        TASK_CHECK( i0 >= advanced && i1 >= advanced );
        make_aligned_video_frame( &fr[0], 0x100, i, d0, 0, body0, sizeof(body0) );
        make_aligned_video_frame( &fr[1], 0x200, i - 3, d1, i == 3, body1, sizeof(body1) );
        TASK_CHECK( trace_collect( &t, w, fr, 2 ) == 0 );
    }

    /* drain with num_frames = 0 */
    drain( &t, w );

    reasm_t r;
    TASK_CHECK( reassemble( &r, &t, fx.psi_pid ) == 0 );
    TASK_CHECK( !r.cc_break );
    TASK_CHECK( check_common_clock( &t, muxrate ) == 0 );

    int n0 = 0, n1 = 0;
    for( int i = 0; i < r.npes; i++ )
    {
        if( r.pes[i].pid == 0x100 ) n0++;
        if( r.pes[i].pid == 0x200 ) n1++;
    }
    TASK_CHECK( n0 == 6 && n1 == 3 );

    /* input media is reusable after the call: rewrite the same buffer and mux again */
    ts_frame_t again;
    make_aligned_video_frame( &again, 0x100, 99, 9000 + 99 * 3600, 1, body, sizeof(body) );
    TASK_CHECK( trace_collect( &t, w, &again, 1 ) == 0 );
    drain( &t, w );

    reasm_free( &r );
    reasm_t r2;
    TASK_CHECK( reassemble( &r2, &t, fx.psi_pid ) == 0 );
    int found = 0;
    for( int i = 0; i < r2.npes; i++ )
        if( r2.pes[i].pid == 0x100 && r2.pes[i].random_access &&
            r2.pes[i].payload_len == (int)sizeof(body) )
            found = 1;
    TASK_CHECK( found );

    reasm_free( &r2 );
    trace_free( &t );
    TASK_CHECK( ts_close_writer( w ) == 0 );
    return 0;
}

/* single-program output must remain byte-identical to the historical fixture */
static int case_single_program_regression( void )
{
    ts_stream_t s = {0};
    s.pid = 0x100;
    s.stream_format = LIBMPEGTS_VIDEO_MPEG2;
    s.stream_id = LIBMPEGTS_STREAM_ID_MPEGVIDEO;
    ts_program_t p = {0};
    p.pmt_pid = 0x1000; p.program_num = 7; p.pcr_pid = 0x110; p.num_streams = 1; p.streams = &s;
    ts_main_t m = {0};
    m.num_programs = 1; m.programs = &p; m.ts_id = 23; m.muxrate = 2000000;
    m.cbr = 1; m.lowlatency = 1; m.ts_type = TS_TYPE_GENERIC; m.pcr_period = 20; m.pat_period = 80;

    ts_writer_t *w = ts_create_writer();
    TASK_CHECK( w );
    TASK_CHECK( ts_setup_transport_stream( w, &m ) == 0 );
    TASK_CHECK( ts_setup_mpegvideo_stream( w, s.pid, LIBMPEGTS_MPEG2_LEVEL_MAIN,
                                           LIBMPEGTS_MPEG2_PROFILE_MAIN, 1000000, 100000, 3 ) == 0 );

    trace_t t; trace_init( &t );
    uint8_t body[640];
    for( int i = 0; i < 8; i++ )
    {
        for( size_t j = 0; j < sizeof(body); j++ ) body[j] = (uint8_t)(j * 17 + i * 31);
        ts_frame_t f = {0};
        f.pid = s.pid; f.data = body; f.size = sizeof(body);
        f.dts = 9000 + i * 3600; f.pts = f.dts;
        f.cpb_initial_arrival_time = (int64_t)(i * 3600 + 900) * 300;
        f.cpb_final_arrival_time = (int64_t)(i * 3600 + 3600) * 300;
        f.random_access = i == 0;
        TASK_CHECK( trace_collect( &t, w, &f, 1 ) == 0 );
        memset( body, 0xee, sizeof(body) );
    }
    drain( &t, w );
    TASK_CHECK( ts_close_writer( w ) == 0 );

    uint64_t fp = 14695981039346656037ULL;
    for( size_t i = 0; i < t.len; i++ )
    {
        fp ^= t.data[i];
        fp *= 1099511628211ULL;
    }
    TASK_CHECK( fp == 0x1a31e3a3681f738eULL );
    TASK_CHECK( t.len == 80088 );
    trace_free( &t );
    return 0;
}

/* ---------------------------------------------------------------- registry */

const struct task_case task_cases[] =
{
    { "mpts_tables", "pat_and_pmts",              case_pat_and_pmts },
    { "mpts_tables", "long_pmt_multipacket",      case_long_pmt_multipacket },
    { "mpts_tables", "config_conflicts",          case_config_conflicts },
    { "mpts_clocks", "per_program_pcrs",          case_per_program_pcrs },
    { "mpts_clocks", "silent_program_pcr",        case_silent_program_pcr },
    { "mpts_media",  "overlapping_windows",       case_overlapping_windows },
    { "mpts_media",  "batched_submission",        case_batched_submission },
    { "mpts_media",  "single_program_regression", case_single_program_regression },
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
