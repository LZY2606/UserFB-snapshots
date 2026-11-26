/* Task verification for seekless per-fragment prefix SIDX output. */
#include "task_support/check.h"
#include "common/internal.h"
#include "lsmash.h"
#include "core/box.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#define FRAGMENT_COUNT 3
#define TRACK_COUNT 2
#define MAX_TRACK_SAMPLES 3
#define REREAD_PATH ".task-build/prefix-index-reread.mp4"
#define LEGACY_PATH ".task-build/legacy-whole-sidx.mp4"

static const int fragment_counts[FRAGMENT_COUNT][TRACK_COUNT] = {
    { 3, 2 },
    { 2, 0 },
    { 2, 3 }
};

typedef struct
{
    uint8_t *data;
    size_t   size;
    size_t   cap;
    size_t   pos;
    int      writes;
    int      fail_at;
} sink_t;

static int sink_write( void *opaque, uint8_t *buf, int size )
{
    sink_t *s = opaque;
    ++s->writes;
    if( s->fail_at && s->writes == s->fail_at )
        return -1;
    if( size <= 0 )
        return -1;
    size_t need = s->pos + (size_t)size;
    if( need < s->pos )
        return -1;
    if( need > s->cap )
    {
        size_t new_cap = s->cap ? s->cap : 4096;
        while( new_cap < need )
        {
            if( new_cap > (SIZE_MAX - 1) / 2 )
                return -1;
            new_cap *= 2;
        }
        uint8_t *p = realloc( s->data, new_cap );
        if( !p )
            return -1;
        s->data = p;
        s->cap  = new_cap;
    }
    memcpy( s->data + s->pos, buf, (size_t)size );
    s->pos = need;
    if( s->pos > s->size )
        s->size = s->pos;
    return size;
}

static int sink_read( void *opaque, uint8_t *buf, int size )
{
    sink_t *s = opaque;
    if( size <= 0 )
        return size == 0 ? 0 : -1;
    size_t available = s->pos < s->size ? s->size - s->pos : 0;
    size_t got = available < (size_t)size ? available : (size_t)size;
    if( got )
        memcpy( buf, s->data + s->pos, got );
    s->pos += got;
    return (int)got;
}

static int64_t sink_seek( void *opaque, int64_t offset, int whence )
{
    sink_t *s = opaque;
    int64_t target;
    if( whence == SEEK_SET )
        target = offset;
    else if( whence == SEEK_CUR )
        target = (int64_t)s->pos + offset;
    else if( whence == SEEK_END )
        target = (int64_t)s->size + offset;
    else
        return -1;
    if( target < 0 )
        return -1;
    s->pos = (size_t)target;
    return target;
}

typedef struct
{
    int      prefix;
    int      legacy_index;
    int      segment;
    int      seekable;
    int      explicit_base;
    int      use_remux;
    int      huge_duration;
    int      fail_at;
    uint32_t mode_override;
    int      mode_override_set;
    uint32_t major_brand;
} build_options_t;

typedef struct
{
    lsmash_root_t *root;
    lsmash_file_t *file;
    sink_t         sink;
    uint32_t       track[TRACK_COUNT];
    uint32_t       entry[TRACK_COUNT];
    uint32_t       timescale[TRACK_COUNT];
    uint64_t       base[TRACK_COUNT];
    uint32_t       payload[FRAGMENT_COUNT][TRACK_COUNT][MAX_TRACK_SAMPLES];
    uint32_t       total_samples[TRACK_COUNT];
    int            cleanup_ok;
    int            empty_flush_ok;
    build_options_t opt;
} fixture_t;

static void fixture_destroy( fixture_t *fx )
{
    if( !fx )
        return;
    if( fx->root )
        lsmash_destroy_root( fx->root );
    free( fx->sink.data );
    fx->root = NULL;
    fx->file = NULL;
    fx->sink.data = NULL;
}

static void set_audio_parameters( lsmash_file_parameters_t *p, sink_t *sink, const build_options_t *opt )
{
    memset( p, 0, sizeof(*p) );
    p->mode = LSMASH_FILE_MODE_WRITE | LSMASH_FILE_MODE_BOX | LSMASH_FILE_MODE_FRAGMENTED
            | LSMASH_FILE_MODE_INITIALIZATION | LSMASH_FILE_MODE_MEDIA;
    if( opt->prefix )
        p->mode |= LSMASH_FILE_MODE_INDEX | LSMASH_FILE_MODE_FRAGMENT_INDEX;
    if( opt->legacy_index )
        p->mode |= LSMASH_FILE_MODE_INDEX;
    if( opt->segment )
        p->mode |= LSMASH_FILE_MODE_SEGMENT;
    if( opt->mode_override_set )
        p->mode = opt->mode_override;
    p->opaque = sink;
    p->write  = sink_write;
    if( opt->seekable )
    {
        p->read = sink_read;
        p->seek = sink_seek;
    }
    else
        p->seek = NULL;
    p->major_brand        = opt->major_brand ? opt->major_brand : ISOM_BRAND_TYPE_ISO6;
    p->max_chunk_duration = 0.001;
    p->max_async_tolerance = 2.0;
    p->max_chunk_size     = 1u << 20;
    p->max_read_size      = 1u << 20;
}

static int add_mp4a_track( fixture_t *fx, int slot )
{
    fx->track[slot] = lsmash_create_track( fx->root, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK );
    if( !fx->track[slot] )
        return -1;
    lsmash_track_parameters_t tp;
    lsmash_initialize_track_parameters( &tp );
    tp.mode = ISOM_TRACK_ENABLED | ISOM_TRACK_IN_MOVIE | ISOM_TRACK_IN_PREVIEW;
    if( lsmash_set_track_parameters( fx->root, fx->track[slot], &tp ) < 0 )
        return -1;
    lsmash_media_parameters_t mp;
    lsmash_initialize_media_parameters( &mp );
    mp.timescale = fx->timescale[slot];
    if( lsmash_set_media_parameters( fx->root, fx->track[slot], &mp ) < 0 )
        return -1;
    lsmash_audio_summary_t *a = (lsmash_audio_summary_t *)lsmash_create_summary( LSMASH_SUMMARY_TYPE_AUDIO );
    if( !a )
        return -1;
    a->sample_type         = ISOM_CODEC_TYPE_MP4A_AUDIO;
    a->aot                 = MP4A_AUDIO_OBJECT_TYPE_AAC_LC;
    a->frequency           = slot == 0 ? 48000 : 44100;
    a->channels            = 2;
    a->sample_size         = 16;
    a->samples_in_frame    = 1024;
    a->max_au_length       = 4;
    if( lsmash_setup_AudioSpecificConfig( a ) < 0 )
    {
        lsmash_cleanup_summary( (lsmash_summary_t *)a );
        return -1;
    }
    fx->entry[slot] = lsmash_add_sample_entry( fx->root, fx->track[slot], a );
    lsmash_cleanup_summary( (lsmash_summary_t *)a );
    return fx->entry[slot] > 0 ? 0 : -1;
}

static int append_encoded_sample( fixture_t *fx, int f, int slot, int local, uint32_t id )
{
    lsmash_sample_t *s = lsmash_create_sample( 4 );
    if( !s )
        return -1;
    s->length = 4;
    s->data[0] = (uint8_t)(id >> 24);
    s->data[1] = (uint8_t)(id >> 16);
    s->data[2] = (uint8_t)(id >> 8);
    s->data[3] = (uint8_t)id;
    uint64_t index = fx->total_samples[slot];
    s->dts = s->cts = fx->base[slot] + index * 1024ULL;
    s->index = fx->entry[slot];
    s->prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
    if( lsmash_append_sample( fx->root, fx->track[slot], s ) < 0 )
    {
        lsmash_delete_sample( s );
        return -1;
    }
    fx->payload[f][slot][local] = id;
    ++fx->total_samples[slot];
    return 0;
}

static int build_fixture( fixture_t *fx, const build_options_t *opt )
{
    memset( fx, 0, sizeof(*fx) );
    fx->opt = *opt;
    fx->sink.fail_at = opt->fail_at;
    fx->timescale[0] = 48000;
    fx->timescale[1] = 44100;
    fx->base[0] = UINT64_C(1) << 40;
    fx->base[1] = UINT64_C(1) << 41;
    lsmash_file_parameters_t p;
    set_audio_parameters( &p, &fx->sink, opt );
    fx->root = lsmash_create_root();
    if( !fx->root )
        goto fail;
    fx->file = lsmash_set_file( fx->root, &p );
    if( !fx->file )
        goto fail;
    if( opt->explicit_base )
        fx->file->allow_moof_base = 0;
    lsmash_movie_parameters_t mv;
    lsmash_initialize_movie_parameters( &mv );
    mv.timescale = 1000;
    if( lsmash_set_movie_parameters( fx->root, &mv ) < 0 )
        goto fail;
    if( add_mp4a_track( fx, 0 ) < 0 || add_mp4a_track( fx, 1 ) < 0 )
        goto fail;
    if( lsmash_create_fragment_movie( fx->root ) < 0 )
        goto fail;
    fx->cleanup_ok = 1;
    uint32_t next_id = 1;
    for( int f = 0; f < FRAGMENT_COUNT; ++f )
    {
        if( f == 1 )
        {
            /* Explicit empty flush on the track absent from this fragment. */
            fx->empty_flush_ok = (lsmash_flush_pooled_samples( fx->root, fx->track[1], 1024 ) == 0);
        }
        int remaining = 1;
        for( int j = 0; remaining; ++j )
        {
            remaining = 0;
            for( int slot = 0; slot < TRACK_COUNT; ++slot )
            {
                if( j >= fragment_counts[f][slot] )
                    continue;
                remaining = 1;
                if( append_encoded_sample( fx, f, slot, j, next_id++ ) < 0 )
                    goto fail;
            }
        }
        for( int slot = 0; slot < TRACK_COUNT; ++slot )
        {
            if( fragment_counts[f][slot] <= 0 )
                continue;
            uint32_t delta = (opt->huge_duration && f == 0 && slot == 0) ? UINT32_MAX : 1024;
            if( lsmash_flush_pooled_samples( fx->root, fx->track[slot], delta ) < 0 )
                goto fail;
        }
        if( f < FRAGMENT_COUNT - 1 )
        {
            if( lsmash_create_fragment_movie( fx->root ) < 0 )
                goto fail;
            if( fx->file->sidx_list.entry_count != 0 )
                fx->cleanup_ok = 0;
        }
    }
    if( opt->use_remux )
    {
        lsmash_adhoc_remux_t remux;
        memset( &remux, 0, sizeof(remux) );
        remux.buffer_size = 1u << 20;
        if( lsmash_finish_movie( fx->root, &remux ) < 0 )
            goto fail;
    }
    else if( lsmash_finish_movie( fx->root, NULL ) < 0 )
        goto fail;
    if( fx->file->sidx_list.entry_count != 0 )
        fx->cleanup_ok = 0;
    return 0;
fail:
    fixture_destroy( fx );
    return -1;
}

/* ---- independent MP4 box parsing ---- */

static uint16_t be16( const uint8_t *p )
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static uint32_t be32( const uint8_t *p )
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint64_t be64( const uint8_t *p )
{
    return (uint64_t)be32( p ) << 32 | be32( p + 4 );
}

static int64_t be32s( const uint8_t *p )
{
    return (int32_t)be32( p );
}

typedef struct
{
    size_t   start;
    size_t   head;
    size_t   end;
    uint32_t type;
    uint8_t  version;
    uint32_t flags;
} box_view_t;

static int box_at( const sink_t *s, size_t pos, box_view_t *box )
{
    memset( box, 0, sizeof(*box) );
    if( pos + 8 > s->size )
        return -1;
    uint32_t size32 = be32( s->data + pos );
    box->type  = be32( s->data + pos + 4 );
    box->start = pos;
    box->head  = 8;
    uint64_t size = size32;
    if( size32 == 1 )
    {
        if( pos + 16 > s->size )
            return -1;
        size = be64( s->data + pos + 8 );
        box->head = 16;
    }
    if( size < box->head || size > SIZE_MAX || pos + (size_t)size > s->size )
        return -1;
    box->end = pos + (size_t)size;
    if( box->end >= box->start + box->head + 4 )
    {
        const uint8_t *fp = s->data + box->start + box->head;
        box->version = fp[0];
        box->flags   = ((uint32_t)fp[1] << 16) | ((uint32_t)fp[2] << 8) | fp[3];
    }
    return 0;
}

static int find_child( const sink_t *s, const box_view_t *parent, uint32_t type, box_view_t *child )
{
    size_t pos = parent->start + 8;
    while( pos + 8 <= parent->end )
    {
        box_view_t b;
        if( box_at( s, pos, &b ) < 0 )
            return -1;
        if( b.type == type )
        {
            *child = b;
            return 1;
        }
        if( b.end <= pos )
            return -1;
        pos = b.end;
    }
    return 0;
}

static int next_child( const sink_t *s, const box_view_t *parent, size_t *pos, box_view_t *child )
{
    if( *pos + 8 > parent->end )
        return 0;
    if( box_at( s, *pos, child ) < 0 )
        return -1;
    *pos = child->end;
    return 1;
}

typedef struct
{
    uint32_t reference_id;
    uint32_t timescale;
    uint8_t  version;
    uint64_t ept;
    uint64_t first_offset;
    uint16_t reference_count;
    uint32_t reference_size;
    uint32_t subsegment_duration;
    uint8_t  starts_sap;
    uint8_t  sap_type;
    uint32_t sap_delta;
} sidx_view_t;

static int parse_sidx( const sink_t *s, const box_view_t *box, sidx_view_t *x )
{
    memset( x, 0, sizeof(*x) );
    const uint8_t *p = s->data;
    size_t q = box->start + box->head + 4;
    if( q + 8 > box->end )
        return -1;
    x->version = box->version;
    x->reference_id = be32( p + q ); q += 4;
    x->timescale    = be32( p + q ); q += 4;
    if( x->version == 0 )
    {
        if( q + 8 > box->end ) return -1;
        x->ept = be32( p + q ); q += 4;
        x->first_offset = be32( p + q ); q += 4;
    }
    else
    {
        if( q + 16 > box->end ) return -1;
        x->ept = be64( p + q ); q += 8;
        x->first_offset = be64( p + q ); q += 8;
    }
    if( q + 4 + 12 > box->end )
        return -1;
    x->reference_count = be16( p + q + 2 );
    q += 4;
    uint32_t ref_word = be32( p + q );
    x->reference_size = ref_word & 0x7fffffffU;
    q += 4;
    x->subsegment_duration = be32( p + q );
    q += 4;
    uint32_t sap_word = be32( p + q );
    x->starts_sap = (sap_word >> 31) & 1;
    x->sap_type   = (sap_word >> 28) & 7;
    x->sap_delta  = sap_word & 0x0fffffffU;
    return 0;
}

static int prior_samples( int f, int slot )
{
    int n = 0;
    for( int i = 0; i < f; ++i )
        n += fragment_counts[i][slot];
    return n;
}

#define VCHECK(cond) do { \
    if( !(cond) ) { fprintf( stderr, "verify %s:%d: %s\n", __FILE__, __LINE__, #cond ); return -1; } \
} while(0)

static int verify_fragment_samples( const fixture_t *fx, int f, const box_view_t *moof, const box_view_t *mdat, int *trun_total )
{
    const sink_t *s = &fx->sink;
    size_t child_pos = moof->start + 8;
    int traf_seen = 0;
    while( child_pos + 8 <= moof->end )
    {
        box_view_t traf;
        int nr = next_child( s, moof, &child_pos, &traf );
        VCHECK( nr >= 0 );
        if( nr == 0 )
            break;
        if( traf.type != LSMASH_4CC( 't', 'r', 'a', 'f' ) )
            continue;
        ++traf_seen;
        box_view_t tfhd_b, tfdt_b;
        VCHECK( find_child( s, &traf, LSMASH_4CC( 't', 'f', 'h', 'd' ), &tfhd_b ) == 1 );
        const uint8_t *p = s->data;
        size_t q = tfhd_b.start + tfhd_b.head + 4;
        uint32_t tfhd_flags = tfhd_b.flags;
        uint32_t track_id = be32( p + q ); q += 4;
        int slot = track_id == fx->track[0] ? 0 : 1;
        VCHECK( track_id == fx->track[slot] );
        VCHECK( fragment_counts[f][slot] > 0 );
        uint64_t base_data_offset = 0;
        if( tfhd_flags & 0x000001U )
        {
            base_data_offset = be64( p + q );
            q += 8;
        }
        if( tfhd_flags & 0x000002U ) q += 4;
        uint32_t default_duration = 1024;
        if( tfhd_flags & 0x000008U )
        {
            default_duration = be32( p + q );
            q += 4;
        }
        uint32_t default_size = 4;
        if( tfhd_flags & 0x000010U )
        {
            default_size = be32( p + q );
            q += 4;
        }
        if( tfhd_flags & 0x000020U ) q += 4;
        int uses_explicit = (tfhd_flags & 0x000001U) != 0;
        int uses_moof_base = (tfhd_flags & 0x020000U) != 0;
        if( fx->opt.explicit_base )
        {
            VCHECK( uses_explicit );
            VCHECK( !uses_moof_base );
            VCHECK( base_data_offset == mdat->start + 8 );
        }
        else
        {
            VCHECK( uses_moof_base );
            VCHECK( !uses_explicit );
        }
        VCHECK( find_child( s, &traf, LSMASH_4CC( 't', 'f', 'd', 't' ), &tfdt_b ) == 1 );
        uint64_t decode_time;
        if( tfdt_b.version == 1 )
            decode_time = be64( p + tfdt_b.start + tfdt_b.head + 4 );
        else
            decode_time = be32( p + tfdt_b.start + tfdt_b.head + 4 );
        VCHECK( decode_time == fx->base[slot] + (uint64_t)prior_samples( f, slot ) * 1024ULL );
        int seen = 0;
        int trun_count = 0;
        size_t traf_pos = traf.start + 8;
        while( traf_pos + 8 <= traf.end )
        {
            box_view_t trun;
            int rn = next_child( s, &traf, &traf_pos, &trun );
            VCHECK( rn >= 0 );
            if( rn == 0 )
                break;
            if( trun.type != LSMASH_4CC( 't', 'r', 'u', 'n' ) )
                continue;
            ++trun_count;
            ++*trun_total;
            size_t tq = trun.start + trun.head + 4;
            uint32_t trun_flags = trun.flags;
            uint32_t sample_count = be32( p + tq ); tq += 4;
            int64_t data_offset = 0;
            if( trun_flags & 0x000001U )
            {
                data_offset = be32s( p + tq );
                tq += 4;
            }
            if( trun_flags & 0x000004U )
                tq += 4;
            uint64_t address_base = uses_explicit ? base_data_offset : moof->start;
            uint64_t sample_abs = address_base + (uint64_t)data_offset;
            for( uint32_t i = 0; i < sample_count; ++i )
            {
                uint32_t duration = default_duration;
                uint32_t size = default_size;
                if( trun_flags & 0x000100U ) { duration = be32( p + tq ); tq += 4; }
                if( trun_flags & 0x000200U ) { size = be32( p + tq ); tq += 4; }
                if( trun_flags & 0x000400U ) tq += 4;
                if( trun_flags & 0x000800U ) tq += 4;
                VCHECK( duration == 1024 );
                VCHECK( size == 4 );
                VCHECK( sample_abs + 4 <= mdat->end );
                uint32_t id = be32( p + sample_abs );
                VCHECK( seen < fragment_counts[f][slot] );
                VCHECK( id == fx->payload[f][slot][seen] );
                uint64_t expected_dts = fx->base[slot]
                                       + (uint64_t)(prior_samples( f, slot ) + seen) * 1024ULL;
                VCHECK( decode_time == expected_dts );
                decode_time += duration;
                sample_abs += size;
                ++seen;
            }
        }
        VCHECK( seen == fragment_counts[f][slot] );
        VCHECK( trun_count >= 1 );
        if( fragment_counts[f][slot] >= 2 )
            VCHECK( trun_count >= 2 );
    }
    int expected_trafs = (fragment_counts[f][0] > 0) + (fragment_counts[f][1] > 0);
    VCHECK( traf_seen == expected_trafs );
    return 0;
}

static int verify_tfra_offsets( const fixture_t *fx )
{
    const sink_t *s = &fx->sink;
    size_t pos = 0;
    box_view_t box;
    int found_mfra = 0;
    while( box_at( s, pos, &box ) == 0 )
    {
        if( box.type == LSMASH_4CC( 'm', 'f', 'r', 'a' ) )
        {
            found_mfra = 1;
            size_t cp = box.start + 8;
            while( cp + 8 <= box.end )
            {
                box_view_t tfra;
                int rn = next_child( s, &box, &cp, &tfra );
                VCHECK( rn >= 0 );
                if( rn == 0 )
                    break;
                if( tfra.type != LSMASH_4CC( 't', 'f', 'r', 'a' ) )
                    continue;
                const uint8_t *p = s->data;
                size_t q = tfra.start + tfra.head + 4;
                uint32_t track_id = be32( p + q ); q += 4;
                VCHECK( track_id == fx->track[0] || track_id == fx->track[1] );
                uint32_t lengths = be32( p + q ); q += 4;
                uint32_t entries = be32( p + q ); q += 4;
                VCHECK( entries > 0 );
                int offset_bytes = tfra.version == 1 ? 8 : 4;
                int traf_bytes = ((lengths >> 4) & 3) + 1;
                int trun_bytes = ((lengths >> 2) & 3) + 1;
                int samp_bytes = (lengths & 3) + 1;
                for( uint32_t i = 0; i < entries; ++i )
                {
                    q += offset_bytes; /* time */
                    uint64_t moof_offset = offset_bytes == 8 ? be64( p + q ) : be32( p + q );
                    q += offset_bytes;
                    q += (size_t)traf_bytes + trun_bytes + samp_bytes;
                    box_view_t candidate;
                    VCHECK( box_at( s, (size_t)moof_offset, &candidate ) == 0 );
                    VCHECK( candidate.type == LSMASH_4CC( 'm', 'o', 'o', 'f' ) );
                }
            }
        }
        pos = box.end;
    }
    VCHECK( found_mfra );
    return 0;
}

static int verify_prefix_output( const fixture_t *fx )
{
    const sink_t *s = &fx->sink;
    VCHECK( s->size > 64 );
    size_t pos = 0;
    box_view_t ftyp, moov;
    VCHECK( box_at( s, 0, &ftyp ) == 0 );
    VCHECK( ftyp.type == LSMASH_4CC( 'f', 't', 'y', 'p' ) );
    VCHECK( box_at( s, ftyp.end, &moov ) == 0 );
    VCHECK( moov.type == LSMASH_4CC( 'm', 'o', 'o', 'v' ) );
    pos = moov.end;
    int trun_total = 0;
    for( int f = 0; f < FRAGMENT_COUNT; ++f )
    {
        int active[TRACK_COUNT];
        int n = 0;
        for( int slot = 0; slot < TRACK_COUNT; ++slot )
            if( fragment_counts[f][slot] > 0 )
                active[n++] = slot;
        VCHECK( n > 0 );
        box_view_t sidx_box[TRACK_COUNT];
        sidx_view_t sidx[TRACK_COUNT];
        for( int k = 0; k < n; ++k )
        {
            VCHECK( box_at( s, pos, &sidx_box[k] ) == 0 );
            VCHECK( sidx_box[k].type == LSMASH_4CC( 's', 'i', 'd', 'x' ) );
            VCHECK( parse_sidx( s, &sidx_box[k], &sidx[k] ) == 0 );
            VCHECK( sidx[k].reference_id == fx->track[active[k]] );
            VCHECK( sidx[k].timescale == fx->timescale[active[k]] );
            VCHECK( sidx[k].reference_count == 1 );
            VCHECK( (sidx_box[k].version == 1) == (sidx[k].ept > UINT32_MAX || sidx[k].first_offset > UINT32_MAX) );
            VCHECK( sidx[k].starts_sap == 1 );
            VCHECK( sidx[k].sap_type == 1 );
            VCHECK( sidx[k].sap_delta == 0 );
            uint64_t fragment_ept = fx->base[active[k]]
                                  + (uint64_t)prior_samples( f, active[k] ) * 1024ULL;
            VCHECK( sidx[k].ept == fragment_ept );
            VCHECK( sidx[k].subsegment_duration == (uint32_t)fragment_counts[f][active[k]] * 1024U );
            pos = sidx_box[k].end;
        }
        for( int k = 1; k < n; ++k )
            VCHECK( sidx[k - 1].reference_id < sidx[k].reference_id );
        box_view_t moof, mdat;
        VCHECK( box_at( s, pos, &moof ) == 0 );
        VCHECK( moof.type == LSMASH_4CC( 'm', 'o', 'o', 'f' ) );
        VCHECK( box_at( s, moof.end, &mdat ) == 0 );
        VCHECK( mdat.type == LSMASH_4CC( 'm', 'd', 'a', 't' ) );
        uint64_t referenced_size = (uint64_t)(moof.end - moof.start) + (mdat.end - mdat.start);
        for( int k = 0; k < n; ++k )
        {
            VCHECK( sidx[k].reference_size == referenced_size );
            uint64_t expected_first_offset = 0;
            for( int j = k + 1; j < n; ++j )
                expected_first_offset += sidx_box[j].end - sidx_box[j].start;
            VCHECK( sidx[k].first_offset == expected_first_offset );
            VCHECK( sidx_box[k].end + sidx[k].first_offset == moof.start );
        }
        VCHECK( verify_fragment_samples( fx, f, &moof, &mdat, &trun_total ) == 0 );
        pos = mdat.end;
    }
    VCHECK( trun_total >= FRAGMENT_COUNT + TRACK_COUNT );
    if( fx->opt.seekable )
    {
        VCHECK( verify_tfra_offsets( fx ) == 0 );
        /* The tail mfra is the only data after the final mdat. */
        box_view_t tail;
        VCHECK( box_at( s, pos, &tail ) == 0 );
        VCHECK( tail.type == LSMASH_4CC( 'm', 'f', 'r', 'a' ) );
        VCHECK( tail.end == s->size );
    }
    else
        VCHECK( pos == s->size );
    return 0;
}

static int expected_payload_at_global( const fixture_t *fx, int slot, uint32_t global, uint32_t *id )
{
    uint32_t start = 0;
    for( int f = 0; f < FRAGMENT_COUNT; ++f )
    {
        uint32_t n = fragment_counts[f][slot];
        if( global < start + n )
        {
            *id = fx->payload[f][slot][global - start];
            return 0;
        }
        start += n;
    }
    return -1;
}

static int write_sink_to_path( const sink_t *s, const char *path )
{
    FILE *fp = fopen( path, "wb" );
    if( !fp )
        return -1;
    size_t n = fwrite( s->data, 1, s->size, fp );
    int rc = fclose( fp );
    return n == s->size && rc == 0 ? 0 : -1;
}

static int verify_library_reread( const fixture_t *fx )
{
    VCHECK( write_sink_to_path( &fx->sink, REREAD_PATH ) == 0 );
    lsmash_root_t *root = lsmash_create_root();
    VCHECK( root != NULL );
    lsmash_file_parameters_t p;
    VCHECK( lsmash_open_file( REREAD_PATH, 1, &p ) == 0 );
    lsmash_file_t *file = lsmash_set_file( root, &p );
    VCHECK( file != NULL );
    VCHECK( lsmash_read_file( file, &p ) >= 0 );
    for( int slot = 0; slot < TRACK_COUNT; ++slot )
    {
        VCHECK( lsmash_construct_timeline( root, fx->track[slot] ) == 0 );
        uint32_t count = lsmash_get_sample_count_in_media_timeline( root, fx->track[slot] );
        VCHECK( count == fx->total_samples[slot] );
        for( uint32_t i = 1; i <= count; ++i )
        {
            lsmash_sample_t *sample = lsmash_get_sample_from_media_timeline( root, fx->track[slot], i );
            VCHECK( sample != NULL );
            uint32_t expected_id;
            VCHECK( expected_payload_at_global( fx, slot, i - 1, &expected_id ) == 0 );
            VCHECK( sample->length == 4 );
            VCHECK( be32( sample->data ) == expected_id );
            /* The high-resolution absolute base is checked directly in tfdt/sidx;
             * the public timeline normalizes the first media timestamp to zero. */
            VCHECK( sample->dts == (uint64_t)(i - 1) * 1024ULL );
            VCHECK( sample->cts == sample->dts );
            lsmash_delete_sample( sample );
        }
    }
    lsmash_destroy_root( root );
    VCHECK( lsmash_close_file( &p ) == 0 );
    remove( REREAD_PATH );
    return 0;
}

/* ---- task cases ---- */

static int case_layout_boxes( void )
{
    fixture_t fx;
    build_options_t opt = { .prefix = 1 };
    TASK_CHECK( build_fixture( &fx, &opt ) == 0 );
    TASK_CHECK( fx.sink.writes > 0 );
    TASK_CHECK( fx.empty_flush_ok );
    TASK_CHECK( verify_prefix_output( &fx ) == 0 );
    TASK_CHECK( fx.cleanup_ok );
    fixture_destroy( &fx );
    return 0;
}

static int case_offsets_seekless_runs( void )
{
    fixture_t fx;
    build_options_t opt = { .prefix = 1 };
    TASK_CHECK( build_fixture( &fx, &opt ) == 0 );
    TASK_CHECK( verify_prefix_output( &fx ) == 0 );
    TASK_CHECK( verify_library_reread( &fx ) == 0 );
    fixture_destroy( &fx );
    return 0;
}

static int case_offsets_seekable_default( void )
{
    fixture_t fx;
    build_options_t opt = { .prefix = 1, .seekable = 1 };
    TASK_CHECK( build_fixture( &fx, &opt ) == 0 );
    TASK_CHECK( verify_prefix_output( &fx ) == 0 );
    fixture_destroy( &fx );
    return 0;
}

static int case_offsets_explicit_base( void )
{
    fixture_t fx;
    build_options_t opt = { .prefix = 1, .seekable = 1, .explicit_base = 1 };
    TASK_CHECK( build_fixture( &fx, &opt ) == 0 );
    TASK_CHECK( verify_prefix_output( &fx ) == 0 );
    fixture_destroy( &fx );
    return 0;
}

static int case_lifecycle_release( void )
{
    fixture_t fx;
    build_options_t opt = { .prefix = 1 };
    TASK_CHECK( build_fixture( &fx, &opt ) == 0 );
    TASK_CHECK( fx.cleanup_ok );
    TASK_CHECK( fx.file->sidx_list.entry_count == 0 );
    fixture_destroy( &fx );
    return 0;
}

static int case_lifecycle_write_failure( void )
{
    sink_t sink = { 0 };
    sink.fail_at = 1;
    lsmash_file_parameters_t p;
    build_options_t opt = { .prefix = 1, .fail_at = 1 };
    set_audio_parameters( &p, &sink, &opt );
    lsmash_root_t *root = lsmash_create_root();
    TASK_CHECK( root != NULL );
    lsmash_file_t *file = lsmash_set_file( root, &p );
    TASK_CHECK( file != NULL );
    lsmash_movie_parameters_t mv;
    lsmash_initialize_movie_parameters( &mv );
    mv.timescale = 1000;
    TASK_CHECK( lsmash_set_movie_parameters( root, &mv ) == 0 );
    /* Minimal valid track setup. */
    uint32_t track = lsmash_create_track( root, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK );
    TASK_CHECK( track != 0 );
    lsmash_track_parameters_t tp;
    lsmash_initialize_track_parameters( &tp );
    tp.mode = ISOM_TRACK_ENABLED | ISOM_TRACK_IN_MOVIE;
    TASK_CHECK( lsmash_set_track_parameters( root, track, &tp ) == 0 );
    lsmash_media_parameters_t mp;
    lsmash_initialize_media_parameters( &mp );
    mp.timescale = 48000;
    TASK_CHECK( lsmash_set_media_parameters( root, track, &mp ) == 0 );
    lsmash_audio_summary_t *a = (lsmash_audio_summary_t *)lsmash_create_summary( LSMASH_SUMMARY_TYPE_AUDIO );
    TASK_CHECK( a != NULL );
    a->sample_type = ISOM_CODEC_TYPE_MP4A_AUDIO;
    a->aot = MP4A_AUDIO_OBJECT_TYPE_AAC_LC;
    a->frequency = 48000;
    a->channels = 2;
    a->sample_size = 16;
    a->samples_in_frame = 1024;
    a->max_au_length = 4;
    TASK_CHECK( lsmash_setup_AudioSpecificConfig( a ) == 0 );
    int entry = lsmash_add_sample_entry( root, track, a );
    lsmash_cleanup_summary( (lsmash_summary_t *)a );
    TASK_CHECK( entry > 0 );

    TASK_CHECK( lsmash_create_fragment_movie( root ) < 0 );
    TASK_CHECK( sink.writes == 1 );
    lsmash_sample_t *sample = lsmash_create_sample( 4 );
    TASK_CHECK( sample != NULL );
    memset( sample->data, 0x5a, 4 );
    sample->dts = sample->cts = 0;
    sample->index = (uint32_t)entry;
    TASK_CHECK( lsmash_append_sample( root, track, sample ) < 0 );
    lsmash_delete_sample( sample );
    TASK_CHECK( lsmash_flush_pooled_samples( root, track, 1024 ) < 0 );
    TASK_CHECK( lsmash_create_fragment_movie( root ) < 0 );
    TASK_CHECK( lsmash_finish_movie( root, NULL ) < 0 );
    TASK_CHECK( sink.writes == 1 );
    lsmash_destroy_root( root );
    free( sink.data );
    return 0;
}

static int case_compat_mode_validation( void )
{
    static const uint32_t required = LSMASH_FILE_MODE_WRITE | LSMASH_FILE_MODE_BOX
                                   | LSMASH_FILE_MODE_FRAGMENTED | LSMASH_FILE_MODE_INDEX
                                   | LSMASH_FILE_MODE_INITIALIZATION | LSMASH_FILE_MODE_MEDIA;
    uint32_t bad_modes[] = {
        LSMASH_FILE_MODE_FRAGMENT_INDEX,
        required & ~LSMASH_FILE_MODE_WRITE,
        required & ~LSMASH_FILE_MODE_BOX,
        required & ~LSMASH_FILE_MODE_FRAGMENTED,
        required & ~LSMASH_FILE_MODE_INDEX,
        required & ~LSMASH_FILE_MODE_INITIALIZATION,
        required & ~LSMASH_FILE_MODE_MEDIA,
        required | LSMASH_FILE_MODE_FRAGMENT_INDEX | LSMASH_FILE_MODE_SEGMENT
    };
    for( size_t i = 0; i < sizeof(bad_modes) / sizeof(bad_modes[0]); ++i )
    {
        sink_t sink = { 0 };
        lsmash_file_parameters_t p;
        build_options_t opt = { .mode_override = bad_modes[i] | LSMASH_FILE_MODE_FRAGMENT_INDEX,
                                .mode_override_set = 1 };
        set_audio_parameters( &p, &sink, &opt );
        lsmash_root_t *root = lsmash_create_root();
        TASK_CHECK( root != NULL );
        TASK_CHECK( lsmash_set_file( root, &p ) == NULL );
        TASK_CHECK( sink.writes == 0 );
        lsmash_destroy_root( root );
        free( sink.data );
    }
    /* Old INDEX on an unseekable stream remains rejected unless prefix mode is present. */
    {
        sink_t sink = { 0 };
        lsmash_file_parameters_t p;
        build_options_t opt = { .legacy_index = 1 };
        set_audio_parameters( &p, &sink, &opt );
        lsmash_root_t *root = lsmash_create_root();
        TASK_CHECK( lsmash_set_file( root, &p ) == NULL );
        lsmash_destroy_root( root );
        free( sink.data );
    }
    /* ISO5 is too old for the restricted prefix mode. */
    {
        sink_t sink = { 0 };
        lsmash_file_parameters_t p;
        build_options_t opt = { .prefix = 1, .major_brand = ISOM_BRAND_TYPE_ISO5 };
        set_audio_parameters( &p, &sink, &opt );
        lsmash_root_t *root = lsmash_create_root();
        TASK_CHECK( lsmash_set_file( root, &p ) == NULL );
        lsmash_destroy_root( root );
        free( sink.data );
    }
    return 0;
}

static int case_compat_explicit_edit_rejected( void )
{
    fixture_t fx;
    memset( &fx, 0, sizeof(fx) );
    build_options_t opt = { .prefix = 1 };
    lsmash_file_parameters_t p;
    set_audio_parameters( &p, &fx.sink, &opt );
    fx.timescale[0] = 48000;
    fx.root = lsmash_create_root();
    TASK_CHECK( fx.root != NULL );
    TASK_CHECK( lsmash_set_file( fx.root, &p ) != NULL );
    lsmash_movie_parameters_t mv;
    lsmash_initialize_movie_parameters( &mv );
    TASK_CHECK( lsmash_set_movie_parameters( fx.root, &mv ) == 0 );
    TASK_CHECK( add_mp4a_track( &fx, 0 ) == 0 );
    lsmash_edit_t edit = { .duration = 1000, .start_time = 0, .rate = ISOM_EDIT_MODE_NORMAL };
    TASK_CHECK( lsmash_create_explicit_timeline_map( fx.root, fx.track[0], edit ) == 0 );
    TASK_CHECK( fx.sink.writes == 0 );
    TASK_CHECK( lsmash_create_fragment_movie( fx.root ) < 0 );
    TASK_CHECK( fx.sink.writes == 0 );
    fixture_destroy( &fx );
    return 0;
}

static int case_compat_capacity_rejected( void )
{
    fixture_t fx;
    build_options_t opt = { .prefix = 1, .huge_duration = 1 };
    /* build_fixture is expected to fail after setup; construct manually to assert no SIDX write. */
    memset( &fx, 0, sizeof(fx) );
    fx.opt = opt;
    fx.sink.fail_at = 0;
    fx.timescale[0] = 48000;
    fx.timescale[1] = 44100;
    fx.base[0] = 0;
    fx.base[1] = 0;
    lsmash_file_parameters_t p;
    set_audio_parameters( &p, &fx.sink, &opt );
    fx.root = lsmash_create_root();
    TASK_CHECK( fx.root != NULL );
    fx.file = lsmash_set_file( fx.root, &p );
    TASK_CHECK( fx.file != NULL );
    lsmash_movie_parameters_t mv;
    lsmash_initialize_movie_parameters( &mv );
    TASK_CHECK( lsmash_set_movie_parameters( fx.root, &mv ) == 0 );
    TASK_CHECK( add_mp4a_track( &fx, 0 ) == 0 );
    TASK_CHECK( lsmash_create_fragment_movie( fx.root ) == 0 );
    TASK_CHECK( append_encoded_sample( &fx, 0, 0, 0, 0x11111111U ) == 0 );
    TASK_CHECK( append_encoded_sample( &fx, 0, 0, 1, 0x22222222U ) == 0 );
    TASK_CHECK( lsmash_flush_pooled_samples( fx.root, fx.track[0], UINT32_MAX ) == 0 );
    int writes_before = fx.sink.writes;
    size_t size_before = fx.sink.size;
    TASK_CHECK( lsmash_create_fragment_movie( fx.root ) < 0 );
    TASK_CHECK( fx.sink.writes == writes_before );
    TASK_CHECK( fx.sink.size == size_before );
    fixture_destroy( &fx );
    return 0;
}

static int case_compat_legacy_no_index( void )
{
    fixture_t fx;
    build_options_t opt = { 0 };
    TASK_CHECK( build_fixture( &fx, &opt ) == 0 );
    unsigned sidx = 0;
    size_t pos = 0;
    box_view_t b;
    while( box_at( &fx.sink, pos, &b ) == 0 )
    {
        if( b.type == LSMASH_4CC( 's', 'i', 'd', 'x' ) )
            ++sidx;
        pos = b.end;
    }
    TASK_CHECK( sidx == 0 );
    TASK_CHECK( verify_library_reread( &fx ) == 0 );
    fixture_destroy( &fx );
    return 0;
}

static int case_compat_legacy_whole_sidx( void )
{
    /* Use the repository file IO for the seekable/remux path. */
    remove( LEGACY_PATH );
    lsmash_root_t *root = lsmash_create_root();
    TASK_CHECK( root != NULL );
    lsmash_file_parameters_t p;
    TASK_CHECK( lsmash_open_file( LEGACY_PATH, 0, &p ) == 0 );
    p.mode |= LSMASH_FILE_MODE_FRAGMENTED | LSMASH_FILE_MODE_INDEX | LSMASH_FILE_MODE_SEGMENT;
    p.major_brand = ISOM_BRAND_TYPE_ISO6;
    p.max_chunk_duration = 0.01;
    lsmash_file_t *file = lsmash_set_file( root, &p );
    TASK_CHECK( file != NULL );
    lsmash_movie_parameters_t mv;
    lsmash_initialize_movie_parameters( &mv );
    TASK_CHECK( lsmash_set_movie_parameters( root, &mv ) == 0 );
    uint32_t track = lsmash_create_track( root, ISOM_MEDIA_HANDLER_TYPE_AUDIO_TRACK );
    TASK_CHECK( track != 0 );
    lsmash_track_parameters_t tp;
    lsmash_initialize_track_parameters( &tp );
    tp.mode = ISOM_TRACK_ENABLED | ISOM_TRACK_IN_MOVIE;
    TASK_CHECK( lsmash_set_track_parameters( root, track, &tp ) == 0 );
    lsmash_media_parameters_t mp;
    lsmash_initialize_media_parameters( &mp );
    mp.timescale = 48000;
    TASK_CHECK( lsmash_set_media_parameters( root, track, &mp ) == 0 );
    lsmash_audio_summary_t *a = (lsmash_audio_summary_t *)lsmash_create_summary( LSMASH_SUMMARY_TYPE_AUDIO );
    TASK_CHECK( a != NULL );
    a->sample_type = ISOM_CODEC_TYPE_MP4A_AUDIO;
    a->aot = MP4A_AUDIO_OBJECT_TYPE_AAC_LC;
    a->frequency = 48000;
    a->channels = 2;
    a->sample_size = 16;
    a->samples_in_frame = 1024;
    a->max_au_length = 4;
    TASK_CHECK( lsmash_setup_AudioSpecificConfig( a ) == 0 );
    int entry = lsmash_add_sample_entry( root, track, a );
    lsmash_cleanup_summary( (lsmash_summary_t *)a );
    TASK_CHECK( entry > 0 );
    TASK_CHECK( lsmash_create_fragment_movie( root ) == 0 );
    for( unsigned f = 0; f < 2; ++f )
    {
        for( unsigned i = 0; i < 2; ++i )
        {
            lsmash_sample_t *s = lsmash_create_sample( 4 );
            TASK_CHECK( s != NULL );
            memset( s->data, (int)(f * 2 + i + 1), 4 );
            s->dts = s->cts = (f * 2 + i) * 1024;
            s->index = (uint32_t)entry;
            s->prop.ra_flags = ISOM_SAMPLE_RANDOM_ACCESS_FLAG_SYNC;
            TASK_CHECK( lsmash_append_sample( root, track, s ) == 0 );
            TASK_CHECK( lsmash_flush_pooled_samples( root, track, 1024 ) == 0 );
        }
        if( f == 0 )
            TASK_CHECK( lsmash_create_fragment_movie( root ) == 0 );
    }
    lsmash_adhoc_remux_t remux = { 0 };
    remux.buffer_size = 1u << 20;
    TASK_CHECK( lsmash_finish_movie( root, &remux ) == 0 );
    lsmash_destroy_root( root );
    TASK_CHECK( lsmash_close_file( &p ) == 0 );

    FILE *fp = fopen( LEGACY_PATH, "rb" );
    TASK_CHECK( fp != NULL );
    TASK_CHECK( fseek( fp, 0, SEEK_END ) == 0 );
    long len = ftell( fp );
    TASK_CHECK( len > 64 );
    rewind( fp );
    uint8_t *buf = malloc( (size_t)len );
    TASK_CHECK( buf != NULL );
    TASK_CHECK( fread( buf, 1, (size_t)len, fp ) == (size_t)len );
    fclose( fp );
    unsigned sidx_boxes = 0;
    uint16_t first_refs = 0;
    size_t pos = 0;
    while( pos + 8 <= (size_t)len )
    {
        uint32_t size = be32( buf + pos );
        uint32_t type = be32( buf + pos + 4 );
        if( size < 8 || pos + size > (size_t)len )
            break;
        if( type == LSMASH_4CC( 's', 'i', 'd', 'x' ) )
        {
            ++sidx_boxes;
            size_t q = pos + 8 + 4 + 8; /* fullbox + id + timescale */
            uint8_t version = buf[pos + 8];
            q += version ? 16 : 8;
            first_refs = be16( buf + q + 2 );
        }
        pos += size;
    }
    TASK_CHECK( sidx_boxes >= 1 );
    TASK_CHECK( first_refs == 2 );
    free( buf );
    remove( LEGACY_PATH );
    return 0;
}

const struct task_case task_cases[] = {
    { "layout",    "sorted-prefix-boxes",        case_layout_boxes },
    { "offsets",   "seekless-tfhd-trun-reread",  case_offsets_seekless_runs },
    { "offsets",   "seekable-default-base-tfra", case_offsets_seekable_default },
    { "offsets",   "explicit-base-tfra",         case_offsets_explicit_base },
    { "lifecycle", "sidx-released-per-fragment", case_lifecycle_release },
    { "lifecycle", "write-failure-locks-io",     case_lifecycle_write_failure },
    { "compat",    "mode-and-brand-validation",  case_compat_mode_validation },
    { "compat",    "explicit-edit-rejected",     case_compat_explicit_edit_rejected },
    { "compat",    "capacity-rejection",         case_compat_capacity_rejected },
    { "compat",    "legacy-no-index",            case_compat_legacy_no_index },
    { "compat",    "legacy-whole-segment-sidx",  case_compat_legacy_whole_sidx }
};
const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
