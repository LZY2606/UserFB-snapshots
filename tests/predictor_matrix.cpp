/*
    tests/predictor_matrix.cpp

    Independent numerical regression tests for the ALAC decoder's dynamic
    predictor (unpc_block), the matrixing stage (unmix16/24/32) and full
    compressed-packet decoding (ALACDecoder::Decode).

    The expected values in this file are produced by a self-contained
    reference model that works in natural sampling coordinates (history is
    out[j-1], out[j-2], ...), implements its own signed rounding, channel
    bit-width truncation and residual-sign-driven coefficient adaptation.
    It never calls the production predictor/matrix routines, so a numerical
    change in the unrolled 4/8-tap loops, the generic loop or the matrix
    rounding is caught by direct comparison.  See PREDICTOR_MATRIX.md for
    the reference formulas.
*/

#include "task_support/check.h"

#include "dplib.h"
#include "matrixlib.h"
#include "aglib.h"
#include "ALACBitUtilities.h"
#include "ALACAudioTypes.h"
#include "ALACDecoder.h"
#include "ALACEncoder.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

/* ------------------------------------------------------------------ */
/* independent reference model                                         */
/* ------------------------------------------------------------------ */

// sign function, defined without relying on production helpers
static int ref_sign( int32_t v )
{
    return ( v > 0 ) - ( v < 0 );
}

// arithmetic (floor-toward-minus-infinity) right shift, valid for
// negative inputs on any platform
static int32_t ref_shift_down64( int64_t w, unsigned bits )
{
    if ( bits == 0 )
        return (int32_t) w;
    int64_t d = (int64_t) 1 << bits;
    int64_t q = w / d;              // truncates toward zero
    if ( ( w < 0 ) && ( ( w % d ) != 0 ) )
        q -= 1;                     // floor
    return (int32_t) q;
}

static int32_t ref_shift_down( int32_t v, unsigned bits )
{
    return ref_shift_down64( v, bits );
}

// truncate to a signed "chanbits"-wide channel word (sign extension)
static int32_t ref_wrap( int32_t v, unsigned chanbits )
{
    int64_t     w = v;
    int64_t     mod = (int64_t) 1 << chanbits;
    int64_t     r = w % mod;
    if ( r < 0 )
        r += mod;
    if ( r >= ( mod >> 1 ) )
        r -= mod;
    return (int32_t) r;
}

struct RefPredStats
{
    int     fullPasses;     // adaptation ran all the way to the earliest tap
    int     earlyStops;     // adaptation stopped before the earliest tap
    int     zeroResiduals;  // residual sign was zero: no adaptation at all
    RefPredStats() : fullPasses( 0 ), earlyStops( 0 ), zeroResiduals( 0 ) {}
};

// Reference reconstruction of one predicted block.  "coefs" is updated in
// place exactly like the production routine so final coefficients can be
// compared.  Test vectors are sized so every accumulator stays well inside
// 32 bits; the reference uses 64-bit accumulation and never relies on
// overflow.
static void ref_unpc_block( const int32_t * pc1, int32_t * out, int num, int16_t * coefs,
                            int numactive, unsigned chanbits, unsigned denshift,
                            RefPredStats * stats )
{
    out[0] = pc1[0];

    if ( numactive == 0 )
    {
        // verbatim copy branch
        for ( int j = 1; j < num; j++ )
            out[j] = pc1[j];
        return;
    }

    if ( numactive == 31 )
    {
        // simple first-difference branch (the one the decoder runs in place)
        int32_t     prev = out[0];
        for ( int j = 1; j < num; j++ )
        {
            prev = ref_wrap( pc1[j] + prev, chanbits );
            out[j] = prev;
        }
        return;
    }

    // warm-up: first "numactive" outputs are plain differences
    for ( int j = 1; j <= numactive && j < num; j++ )
        out[j] = ref_wrap( pc1[j] + out[j - 1], chanbits );

    const int       lim = numactive + 1;
    const int32_t   denhalf = 1 << ( denshift - 1 );

    for ( int j = lim; j < num; j++ )
    {
        const int32_t   top = out[j - lim];

        // prediction from the history in natural sampling coordinates
        int64_t     sum = 0;
        for ( int k = 0; k < numactive; k++ )
            sum += (int64_t) coefs[k] * ( out[j - 1 - k] - top );

        int32_t     pred = ref_shift_down64( sum + denhalf, denshift );
        int32_t     del = pc1[j];
        int32_t     del0 = del;
        int         sg = ref_sign( del );

        out[j] = ref_wrap( del + top + pred, chanbits );

        if ( sg > 0 )
        {
            for ( int k = numactive - 1; k >= 0; k-- )
            {
                int32_t     dd = top - out[j - 1 - k];
                int         sgn = ref_sign( dd );
                coefs[k] = (int16_t) ( coefs[k] - sgn );
                del0 -= ( numactive - k ) * ref_shift_down( sgn * dd, denshift );
                if ( k == 0 ) { stats->fullPasses++; break; }
                if ( del0 <= 0 ) { stats->earlyStops++; break; }
            }
        }
        else if ( sg < 0 )
        {
            for ( int k = numactive - 1; k >= 0; k-- )
            {
                int32_t     dd = top - out[j - 1 - k];
                int         sgn = ref_sign( dd );
                coefs[k] = (int16_t) ( coefs[k] + sgn );
                del0 -= ( numactive - k ) * ref_shift_down( -sgn * dd, denshift );
                if ( k == 0 ) { stats->fullPasses++; break; }
                if ( del0 >= 0 ) { stats->earlyStops++; break; }
            }
        }
        else
        {
            stats->zeroResiduals++;
        }
    }
}

// reference stereo de-matrixing: L = u + v - floor(mixRes*v / 2^mixBits)
static void ref_unmix( const int32_t * u, const int32_t * v, int num,
                       int mixbits, int mixres, int32_t * L, int32_t * R )
{
    for ( int j = 0; j < num; j++ )
    {
        if ( mixres != 0 )
        {
            int32_t     adj = ref_shift_down64( (int64_t) mixres * v[j], (unsigned) mixbits );
            L[j] = u[j] + v[j] - adj;
            R[j] = L[j] - v[j];
        }
        else
        {
            L[j] = u[j];
            R[j] = v[j];
        }
    }
}

// count samples where floor() vs truncation-toward-zero actually disagree,
// i.e. negative mixres*v products with nonzero low "mixbits" bits
static int count_rounding_cases( const int32_t * v, int num, int mixbits, int mixres )
{
    int         count = 0;
    int64_t     mask = ( (int64_t) 1 << mixbits ) - 1;
    for ( int j = 0; j < num; j++ )
    {
        int64_t     product = (int64_t) mixres * v[j];
        if ( ( mixres != 0 ) && ( product < 0 ) && ( ( product & mask ) != 0 ) )
            ++count;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* comparison helpers: report first difference, then let TASK_CHECK fail */
/* ------------------------------------------------------------------ */

static bool check_seq_i32( const char * label, const int32_t * got, const int32_t * exp, int num )
{
    for ( int i = 0; i < num; i++ )
    {
        if ( got[i] != exp[i] )
        {
            std::printf( "CONTENT_MISMATCH %s index=%d actual=%d expected=%d\n",
                         label, i, got[i], exp[i] );
            return false;
        }
    }
    return true;
}

static bool check_seq_i16( const char * label, const int16_t * got, const int16_t * exp, int num )
{
    for ( int i = 0; i < num; i++ )
    {
        if ( got[i] != exp[i] )
        {
            std::printf( "CONTENT_MISMATCH %s index=%d actual=%d expected=%d\n",
                         label, i, (int) got[i], (int) exp[i] );
            return false;
        }
    }
    return true;
}

static bool check_seq_u8( const char * label, const uint8_t * got, const uint8_t * exp, int num )
{
    for ( int i = 0; i < num; i++ )
    {
        if ( got[i] != exp[i] )
        {
            std::printf( "CONTENT_MISMATCH %s index=%d actual=%u expected=%u\n",
                         label, i, (unsigned) got[i], (unsigned) exp[i] );
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* independent bit stream writer/reader (MSB first)                    */
/* ------------------------------------------------------------------ */

struct TestBitWriter
{
    std::vector<uint8_t>    buf;
    uint32_t                bitPos;

    explicit TestBitWriter( size_t capacity ) : buf( capacity, 0 ), bitPos( 0 ) {}

    void put( uint32_t value, unsigned numBits )
    {
        for ( unsigned i = numBits; i-- > 0; )
        {
            if ( ( value >> i ) & 1u )
                buf[bitPos >> 3] |= (uint8_t) ( 1u << ( 7 - ( bitPos & 7 ) ) );
            ++bitPos;
        }
    }
};

struct TestBitReader
{
    const uint8_t * buf;
    uint32_t        bitPos;

    uint32_t get( unsigned numBits )
    {
        uint32_t    value = 0;
        for ( unsigned i = 0; i < numBits; i++ )
        {
            value = ( value << 1 ) | ( ( buf[bitPos >> 3] >> ( 7 - ( bitPos & 7 ) ) ) & 1u );
            ++bitPos;
        }
        return value;
    }
};

// entropy-code residuals with the production AG coder at the writer's
// current bit position (headers/structure remain test-written)
static bool dyn_comp_at( TestBitWriter & w, int32_t * residuals, int numSamples, int bitSize )
{
    AGParamRec      agParams;
    BitBuffer       bits;
    uint32_t        numBits = 0;

    // match the decoder's parameter derivation: mConfig.mb, (pb*pbFactor)/4,
    // mConfig.kb, fullwidth = sectorwidth = numSamples, mConfig.maxRun
    set_ag_params( &agParams, MB0, ( PB0 * 4 ) / 4, KB0,
                   (uint32_t) numSamples, (uint32_t) numSamples, MAX_RUN_DEFAULT );

    bits.cur        = &w.buf[w.bitPos >> 3];
    bits.end        = &w.buf[0] + w.buf.size();
    bits.bitIndex   = w.bitPos & 7;
    bits.byteSize   = (uint32_t) w.buf.size();

    if ( dyn_comp( &agParams, residuals, &bits, numSamples, bitSize, &numBits ) != ALAC_noErr )
        return false;

    w.bitPos += numBits;
    return true;
}

/* ------------------------------------------------------------------ */
/* shared test vectors                                                 */
/* ------------------------------------------------------------------ */

// residuals with alternating positive/negative/zero entries; magnitudes are
// chosen so that large residuals force full coefficient updates while +-1
// residuals stop the adaptation after the first taps
static void make_predictor_residuals( int32_t * pc, int num, unsigned seed )
{
    pc[0] = 7000;
    for ( int j = 1; j < num; j++ )
    {
        int32_t     dither = (int32_t) ( ( j * 271 + seed * 13 ) % 1001 ) - 500;
        switch ( ( j + seed ) & 7 )
        {
            case 0:     pc[j] =  12000 + dither;    break;
            case 1:     pc[j] = -12000 + dither;    break;
            case 2:     pc[j] =  1;                 break;
            case 3:     pc[j] = -1;                 break;
            case 4:     pc[j] =  0;                 break;
            case 5:     pc[j] =  5000 + dither;     break;
            case 6:     pc[j] = -5000 + dither;     break;
            default:    pc[j] =  0;                 break;
        }
    }
}

static void put_be32( uint8_t * p, uint32_t v )
{
    p[0] = (uint8_t) ( v >> 24 );
    p[1] = (uint8_t) ( v >> 16 );
    p[2] = (uint8_t) ( v >> 8 );
    p[3] = (uint8_t) v;
}

static void build_cookie( uint8_t * cookie, uint32_t frameLength, uint8_t bitDepth, uint8_t numChannels )
{
    std::memset( cookie, 0, 24 );
    put_be32( cookie + 0, frameLength );
    cookie[4] = 0;                          // compatibleVersion
    cookie[5] = bitDepth;
    cookie[6] = PB0;
    cookie[7] = MB0;
    cookie[8] = KB0;
    cookie[9] = numChannels;
    cookie[10] = 0;                         // maxRun, big endian
    cookie[11] = 255;
    put_be32( cookie + 20, 44100 );         // sampleRate
}

// store a 24-bit little-endian sample (matches the decoder's byte layout)
static void put_24le( uint8_t * p, int32_t value )
{
    uint32_t    w = (uint32_t) value;
    p[0] = (uint8_t) ( w & 0xffu );
    p[1] = (uint8_t) ( ( w >> 8 ) & 0xffu );
    p[2] = (uint8_t) ( ( w >> 16 ) & 0xffu );
}

/* ------------------------------------------------------------------ */
/* predictor cases                                                     */
/* ------------------------------------------------------------------ */

static int case_pred_order4( void )
{
    const int           num = 96, order = 4;
    const unsigned      chanbits = 16, denshift = 9;
    std::vector<int32_t> pc( num ), outProd( num, 0 ), outRef( num, 0 );
    int16_t             coefsProd[order] = { 90, -45, 20, -8 };
    int16_t             coefsRef[order]  = { 90, -45, 20, -8 };
    RefPredStats        stats;

    make_predictor_residuals( pc.data(), num, 0 );

    unpc_block( pc.data(), outProd.data(), num, coefsProd, order, chanbits, denshift );
    ref_unpc_block( pc.data(), outRef.data(), num, coefsRef, order, chanbits, denshift, &stats );

    TASK_CHECK( check_seq_i32( "pred4.samples", outProd.data(), outRef.data(), num ) );
    TASK_CHECK( check_seq_i16( "pred4.coefs", coefsProd, coefsRef, order ) );
    TASK_CHECK( stats.earlyStops > 0 );
    TASK_CHECK( stats.fullPasses > 0 );
    TASK_CHECK( stats.zeroResiduals > 0 );
    return 0;
}

static int case_pred_order8( void )
{
    const int           num = 96, order = 8;
    const unsigned      chanbits = 17, denshift = 10;
    std::vector<int32_t> pc( num ), outProd( num, 0 ), outRef( num, 0 );
    int16_t             coefsProd[order] = { 300, -150, 70, -25, 12, -6, 3, -1 };
    int16_t             coefsRef[order]  = { 300, -150, 70, -25, 12, -6, 3, -1 };
    RefPredStats        stats;

    make_predictor_residuals( pc.data(), num, 3 );

    unpc_block( pc.data(), outProd.data(), num, coefsProd, order, chanbits, denshift );
    ref_unpc_block( pc.data(), outRef.data(), num, coefsRef, order, chanbits, denshift, &stats );

    TASK_CHECK( check_seq_i32( "pred8.samples", outProd.data(), outRef.data(), num ) );
    TASK_CHECK( check_seq_i16( "pred8.coefs", coefsProd, coefsRef, order ) );
    TASK_CHECK( stats.earlyStops > 0 );
    TASK_CHECK( stats.fullPasses > 0 );
    TASK_CHECK( stats.zeroResiduals > 0 );
    return 0;
}

static int case_pred_general( void )
{
    // an order that takes the generic (non-unrolled) loop
    const int           num = 90, order = 6;
    const unsigned      chanbits = 16, denshift = 8;
    std::vector<int32_t> pc( num ), outProd( num, 0 ), outRef( num, 0 );
    int16_t             coefsProd[order] = { 150, -70, 30, -12, 6, -2 };
    int16_t             coefsRef[order]  = { 150, -70, 30, -12, 6, -2 };
    RefPredStats        stats;

    make_predictor_residuals( pc.data(), num, 5 );

    unpc_block( pc.data(), outProd.data(), num, coefsProd, order, chanbits, denshift );
    ref_unpc_block( pc.data(), outRef.data(), num, coefsRef, order, chanbits, denshift, &stats );

    TASK_CHECK( check_seq_i32( "predgen.samples", outProd.data(), outRef.data(), num ) );
    TASK_CHECK( check_seq_i16( "predgen.coefs", coefsProd, coefsRef, order ) );
    TASK_CHECK( stats.earlyStops > 0 );
    TASK_CHECK( stats.fullPasses > 0 );
    TASK_CHECK( stats.zeroResiduals > 0 );
    return 0;
}

static int case_pred_special( void )
{
    const int           num = 80;
    std::vector<int32_t> pc( num ), outProd( num, 0 ), outRef( num, 0 );
    RefPredStats        stats;

    // numactive == 0: verbatim copy, no channel-width truncation
    for ( int j = 0; j < num; j++ )
        pc[j] = j * 1000 - 45000;       // deliberately exceeds 16-bit range
    unpc_block( pc.data(), outProd.data(), num, NULL, 0, 16, 9 );
    ref_unpc_block( pc.data(), outRef.data(), num, NULL, 0, 16, 9, &stats );
    TASK_CHECK( check_seq_i32( "pred0.samples", outProd.data(), outRef.data(), num ) );

    // numactive == 31: first-difference branch, run in place like the decoder
    for ( int j = 0; j < num; j++ )
        pc[j] = (int32_t) ( ( j * 97 ) % 4000 ) - 2000;
    std::vector<int32_t> inPlace( pc );
    unpc_block( inPlace.data(), inPlace.data(), num, NULL, 31, 16, 0 );
    ref_unpc_block( pc.data(), outRef.data(), num, NULL, 31, 16, 0, &stats );
    TASK_CHECK( check_seq_i32( "pred31.samples", inPlace.data(), outRef.data(), num ) );

    // in-place operation through the unrolled 8-tap loop
    const int           order = 8;
    int16_t             coefsProd[order] = { 260, -130, 55, -18, 9, -4, 2, -1 };
    int16_t             coefsRef[order]  = { 260, -130, 55, -18, 9, -4, 2, -1 };
    make_predictor_residuals( pc.data(), num, 1 );
    inPlace = pc;
    unpc_block( inPlace.data(), inPlace.data(), num, coefsProd, order, 16, 9 );
    ref_unpc_block( pc.data(), outRef.data(), num, coefsRef, order, 16, 9, &stats );
    TASK_CHECK( check_seq_i32( "pred8inplace.samples", inPlace.data(), outRef.data(), num ) );
    TASK_CHECK( check_seq_i16( "pred8inplace.coefs", coefsProd, coefsRef, order ) );
    return 0;
}

/* ------------------------------------------------------------------ */
/* matrix cases                                                        */
/* ------------------------------------------------------------------ */

static int case_matrix16( void )
{
    const int           num = 24;
    std::vector<int32_t> u( num ), v( num ), L( num ), R( num );

    for ( int j = 0; j < num; j++ )
    {
        u[j] = (int32_t) ( ( j * 2654 + 1000 ) % 16001 ) - 8000;
        v[j] = (int32_t) ( ( j * 1723 + 500 ) % 16001 ) - 8000;
    }

    // nonzero mixRes with both signs (negative products exercise floor
    // rounding) plus the separated-stereo path
    const int           mixes[][2] = { { 2, 3 }, { 2, -3 }, { 4, 5 }, { 0, 0 } };
    for ( unsigned t = 0; t < sizeof( mixes ) / sizeof( mixes[0] ); t++ )
    {
        int                 mixbits = mixes[t][0], mixres = mixes[t][1];
        std::vector<int16_t> out( 2 * num, 0 ), exp( 2 * num, 0 );

        unmix16( u.data(), v.data(), out.data(), 2, num, mixbits, mixres );
        ref_unmix( u.data(), v.data(), num, mixbits, mixres, L.data(), R.data() );
        for ( int j = 0; j < num; j++ )
        {
            exp[2 * j + 0] = (int16_t) L[j];
            exp[2 * j + 1] = (int16_t) R[j];
        }
        TASK_CHECK( check_seq_i16( "unmix16", out.data(), exp.data(), 2 * num ) );
    }

    // stride larger than the channel count
    {
        std::vector<int16_t> out( 3 * num, (int16_t) 0x5555 ), exp( 3 * num, (int16_t) 0x5555 );
        unmix16( u.data(), v.data(), out.data(), 3, num, 2, 3 );
        ref_unmix( u.data(), v.data(), num, 2, 3, L.data(), R.data() );
        for ( int j = 0; j < num; j++ )
        {
            exp[3 * j + 0] = (int16_t) L[j];
            exp[3 * j + 1] = (int16_t) R[j];
        }
        TASK_CHECK( check_seq_i16( "unmix16.stride3", out.data(), exp.data(), 3 * num ) );
    }

    TASK_CHECK( count_rounding_cases( v.data(), num, 2, 3 ) > 0 );
    TASK_CHECK( count_rounding_cases( v.data(), num, 2, -3 ) > 0 );
    return 0;
}

static int case_matrix24( void )
{
    const int           num = 16;
    std::vector<int32_t> u( num ), v( num ), L( num ), R( num );

    for ( int j = 0; j < num; j++ )
    {
        u[j] = (int32_t) ( ( j * 45226 + 777 ) % 200001 ) - 100000;
        v[j] = (int32_t) ( ( j * 31234 + 901 ) % 200001 ) - 100000;
    }

    // asymmetric low-byte pattern for the shift-off path
    std::vector<uint16_t> shift( 2 * num );
    for ( int j = 0; j < num; j++ )
    {
        shift[2 * j + 0] = (uint16_t) ( ( j * 197 + 45 ) & 0xff );
        shift[2 * j + 1] = (uint16_t) ( ( j * 89 + 7 ) & 0xff );
    }

    struct { int mixbits, mixres, bytesShifted; } configs[] = { { 2, 3, 1 }, { 0, 0, 1 }, { 2, 3, 0 } };
    for ( unsigned t = 0; t < sizeof( configs ) / sizeof( configs[0] ); t++ )
    {
        int                 mixbits = configs[t].mixbits;
        int                 mixres = configs[t].mixres;
        int                 bytesShifted = configs[t].bytesShifted;
        std::vector<uint8_t> out( 6 * num, 0 ), exp( 6 * num, 0 );

        unmix24( u.data(), v.data(), out.data(), 2, num, mixbits, mixres,
                 bytesShifted ? shift.data() : NULL, bytesShifted );
        ref_unmix( u.data(), v.data(), num, mixbits, mixres, L.data(), R.data() );
        for ( int j = 0; j < num; j++ )
        {
            int32_t     l = L[j], r = R[j];
            if ( bytesShifted )
            {
                l = (int32_t) ( ( (uint32_t) l << 8 ) | shift[2 * j + 0] );
                r = (int32_t) ( ( (uint32_t) r << 8 ) | shift[2 * j + 1] );
            }
            put_24le( &exp[6 * j + 0], l );
            put_24le( &exp[6 * j + 3], r );
        }
        TASK_CHECK( check_seq_u8( "unmix24", out.data(), exp.data(), 6 * num ) );
    }

    TASK_CHECK( count_rounding_cases( v.data(), num, 2, 3 ) > 0 );
    return 0;
}

static int case_matrix32( void )
{
    const int           num = 12;
    std::vector<int32_t> u( num ), v( num ), L( num ), R( num );

    for ( int j = 0; j < num; j++ )
    {
        u[j] = (int32_t) ( ( j * 9134 + 333 ) % 16001 ) - 8000;
        v[j] = (int32_t) ( ( j * 6102 + 111 ) % 16001 ) - 8000;
    }

    std::vector<uint16_t> shift( 2 * num );
    for ( int j = 0; j < num; j++ )
    {
        shift[2 * j + 0] = (uint16_t) ( ( j * 7919 + 1234 ) & 0xffff );
        shift[2 * j + 1] = (uint16_t) ( ( j * 1049 + 4321 ) & 0xffff );
    }

    struct { int mixbits, mixres, bytesShifted; } configs[] = { { 2, 3, 2 }, { 0, 0, 0 }, { 0, 0, 1 } };
    for ( unsigned t = 0; t < sizeof( configs ) / sizeof( configs[0] ); t++ )
    {
        int                 mixbits = configs[t].mixbits;
        int                 mixres = configs[t].mixres;
        int                 bytesShifted = configs[t].bytesShifted;
        std::vector<int32_t> out( 2 * num, 0 ), exp( 2 * num, 0 );

        unmix32( u.data(), v.data(), out.data(), 2, num, mixbits, mixres,
                 bytesShifted ? shift.data() : NULL, bytesShifted );
        ref_unmix( u.data(), v.data(), num, mixbits, mixres, L.data(), R.data() );
        for ( int j = 0; j < num; j++ )
        {
            int32_t     l = L[j], r = R[j];
            if ( bytesShifted )
            {
                int     sh = bytesShifted * 8;
                l = (int32_t) ( ( (uint32_t) l << sh ) | shift[2 * j + 0] );
                r = (int32_t) ( ( (uint32_t) r << sh ) | shift[2 * j + 1] );
            }
            exp[2 * j + 0] = l;
            exp[2 * j + 1] = r;
        }
        TASK_CHECK( check_seq_i32( "unmix32", out.data(), exp.data(), 2 * num ) );
    }

    TASK_CHECK( count_rounding_cases( v.data(), num, 2, 3 ) > 0 );
    return 0;
}

/* ------------------------------------------------------------------ */
/* packet cases                                                        */
/* ------------------------------------------------------------------ */

static int case_packet_handbuilt( void )
{
    const int           num = 64;       // frames per element

    // residual plans (what dyn_decomp will recover, per channel)
    std::vector<int32_t> resU( num ), resV( num ), resC( num );
    for ( int j = 0; j < num; j++ )
    {
        switch ( j & 7 )
        {
            case 0:  resU[j] =  1400 + ( j * 37 ) % 300;  break;
            case 1:  resU[j] = -1400 - ( j * 41 ) % 300;  break;
            case 2:  resU[j] =  0;                        break;
            case 3:  resU[j] =  2200;                     break;
            case 4:  resU[j] = -2200;                     break;
            case 5:  resU[j] =  0;                        break;
            case 6:  resU[j] =  750;                      break;
            default: resU[j] = -750;                      break;
        }
        resV[j] = ( ( j & 3 ) < 2 ? 1 : -1 ) * ( 500 + (int32_t) ( ( j * 91 ) % 900 ) );
        if ( ( j & 7 ) == 5 )
            resV[j] = 0;
        resC[j] = ( ( j & 3 ) < 2 ? -1 : 1 ) * ( 200 + (int32_t) ( ( j * 53 ) % 600 ) );
        if ( ( j & 7 ) == 2 )
            resC[j] = 0;
    }

    // test-chosen transmission parameters
    const int           mixBits = 2, mixRes = 3;
    const int           denShift = 9;
    int16_t             coefsU[8] = { 280, -140, 60, -20, 10, -5, 3, -1 };
    int16_t             coefsV[4] = { 250, -120, 45, -9 };      // modeV = 1: two-stage
    int16_t             coefsC[4] = { 200, -100, 40, -8 };

    // asymmetric shift-off low bytes
    std::vector<uint8_t> shiftU( num ), shiftV( num ), shiftC( num );
    for ( int j = 0; j < num; j++ )
    {
        shiftU[j] = (uint8_t) ( ( j * 197 + 45 ) & 0xff );
        shiftV[j] = (uint8_t) ( ( j * 89 + 7 ) & 0xff );
        shiftC[j] = (uint8_t) ( ( j * 31 + 128 ) & 0xff );
    }

    // expected PCM: residual plan -> independent predictor -> independent matrix
    std::vector<int32_t> u( num ), stage( num ), v( num ), c( num ), L( num ), R( num );
    std::vector<int16_t> refU( coefsU, coefsU + 8 ), refV( coefsV, coefsV + 4 ), refC( coefsC, coefsC + 4 );
    RefPredStats        stats;

    ref_unpc_block( resU.data(), u.data(), num, refU.data(), 8, 17, denShift, &stats );
    ref_unpc_block( resV.data(), stage.data(), num, NULL, 31, 17, 0, &stats );      // mode 1, stage 1
    ref_unpc_block( stage.data(), v.data(), num, refV.data(), 4, 17, denShift, &stats ); // stage 2
    ref_unpc_block( resC.data(), c.data(), num, refC.data(), 4, 16, denShift, &stats );
    ref_unmix( u.data(), v.data(), num, mixBits, mixRes, L.data(), R.data() );

    std::vector<uint8_t> expect( num * 9, 0 );
    for ( int j = 0; j < num; j++ )
    {
        put_24le( &expect[j * 9 + 0], (int32_t) ( ( (uint32_t) L[j] << 8 ) | shiftU[j] ) );
        put_24le( &expect[j * 9 + 3], (int32_t) ( ( (uint32_t) R[j] << 8 ) | shiftV[j] ) );
        put_24le( &expect[j * 9 + 6], (int32_t) ( ( (uint32_t) c[j] << 8 ) | shiftC[j] ) );
    }

    // the packet must really exercise negative-product matrix rounding
    TASK_CHECK( count_rounding_cases( v.data(), num, mixBits, mixRes ) > 0 );

    // build the compressed packet with the independent writer
    TestBitWriter       w( 1 << 16 );

    w.put( ID_CPE, 3 );                 // channel pair element, instance 0
    w.put( 0, 4 );
    w.put( 0, 12 );                     // unused header
    w.put( 0xA, 4 );                    // partial frame, 1 byte shifted off, compressed
    w.put( (uint32_t) num, 32 );        // explicit frame length
    w.put( (uint32_t) mixBits, 8 );
    w.put( (uint32_t) mixRes, 8 );
    w.put( ( 0 << 4 ) | denShift, 8 );  // modeU = 0
    w.put( ( 4 << 5 ) | 8, 8 );         // pbFactorU = 4, numU = 8
    for ( int i = 0; i < 8; i++ )
        w.put( (uint16_t) coefsU[i], 16 );
    w.put( ( 1 << 4 ) | denShift, 8 );  // modeV = 1: two-stage prediction
    w.put( ( 4 << 5 ) | 4, 8 );         // pbFactorV = 4, numV = 4
    for ( int i = 0; i < 4; i++ )
        w.put( (uint16_t) coefsV[i], 16 );
    for ( int j = 0; j < num; j++ )     // interleaved shift-off bytes
    {
        w.put( shiftU[j], 8 );
        w.put( shiftV[j], 8 );
    }
    TASK_CHECK( dyn_comp_at( w, resU.data(), num, 17 ) );
    TASK_CHECK( dyn_comp_at( w, resV.data(), num, 17 ) );

    w.put( ID_SCE, 3 );                 // single channel element, instance 0
    w.put( 0, 4 );
    w.put( 0, 12 );
    w.put( 0x2, 4 );                    // not partial, 1 byte shifted off, compressed
    w.put( 0, 8 );                      // mixBits = mixRes = 0 for mono
    w.put( 0, 8 );
    w.put( ( 0 << 4 ) | denShift, 8 );
    w.put( ( 4 << 5 ) | 4, 8 );
    for ( int i = 0; i < 4; i++ )
        w.put( (uint16_t) coefsC[i], 16 );
    for ( int j = 0; j < num; j++ )
        w.put( shiftC[j], 8 );
    TASK_CHECK( dyn_comp_at( w, resC.data(), num, 16 ) );

    w.put( ID_END, 3 );
    uint32_t            totalBits = ( w.bitPos + 7 ) & ~7u;
    uint32_t            packetBytes = totalBits / 8;

    // decode through the real decoder
    uint8_t             cookie[24];
    build_cookie( cookie, (uint32_t) num, 24, 3 );

    ALACDecoder         decoder;
    TASK_CHECK( decoder.Init( cookie, sizeof( cookie ) ) == ALAC_noErr );

    std::vector<uint8_t> packet( w.buf.begin(), w.buf.begin() + packetBytes + 16 );
    BitBuffer           bits;
    BitBufferInit( &bits, packet.data(), packetBytes );

    std::vector<uint8_t> pcm( num * 9, 0 );
    uint32_t            got = 0;
    TASK_CHECK( decoder.Decode( &bits, pcm.data(), (uint32_t) num, 3, &got ) == ALAC_noErr );
    TASK_CHECK( got == (uint32_t) num );
    TASK_CHECK( check_seq_u8( "packet.pcm", pcm.data(), expect.data(), num * 9 ) );
    TASK_CHECK( BitBufferGetPosition( &bits ) == totalBits );
    return 0;
}

static int case_packet_compat( void )
{
    // compatibility control: a compressed packet from the production encoder
    const unsigned      frames = 300, channels = 2;
    std::vector<uint8_t> input( frames * channels * 2 );

    for ( unsigned i = 0; i < frames; i++ )
    {
        int32_t     t = (int32_t) ( ( i * 29 ) % 1600 ) - 800;
        int16_t     s0 = (int16_t) t;
        int16_t     s1 = (int16_t) ( t - ( t >> 2 ) + (int) ( i % 7 ) - 3 );
        std::memcpy( &input[( i * 2 + 0 ) * 2], &s0, 2 );
        std::memcpy( &input[( i * 2 + 1 ) * 2], &s1, 2 );
    }

    AudioFormatDescription  pcm = {}, out = {};
    pcm.mSampleRate = out.mSampleRate = 44100;
    pcm.mFormatID = kALACFormatLinearPCM;
    pcm.mFormatFlags = kALACFormatFlagIsSignedInteger | kALACFormatFlagIsPacked;
    pcm.mChannelsPerFrame = out.mChannelsPerFrame = channels;
    pcm.mBitsPerChannel = 16;
    pcm.mBytesPerPacket = pcm.mBytesPerFrame = channels * 2;
    pcm.mFramesPerPacket = 1;
    out.mFormatID = kALACFormatAppleLossless;
    out.mFormatFlags = 1;
    out.mFramesPerPacket = frames;

    ALACEncoder         encoder;
    encoder.SetFrameSize( 512 );        // 300 < 512: encoder emits a partial frame
    TASK_CHECK( encoder.InitializeEncoder( out ) == ALAC_noErr );

    uint32_t            cookieSize = encoder.GetMagicCookieSize( channels );
    std::vector<uint8_t> cookie( cookieSize + 16, 0 );
    encoder.GetMagicCookie( cookie.data(), &cookieSize );

    std::vector<uint8_t> encoded( input.size() * 4 + 1024, 0 );
    int32_t             size = (int32_t) input.size();
    TASK_CHECK( encoder.Encode( pcm, pcm, input.data(), encoded.data(), &size ) == ALAC_noErr );
    TASK_CHECK( size > 0 );

    // parse the packet header with the independent reader: it must be a real
    // compressed CPE packet, not an escape packet
    TestBitReader       reader = { encoded.data(), 0 };
    TASK_CHECK( reader.get( 3 ) == ID_CPE );
    reader.get( 4 );                                    // element instance tag
    TASK_CHECK( reader.get( 12 ) == 0 );                // unused header
    uint32_t            flags = reader.get( 4 );
    uint32_t            partial = flags >> 3;
    TASK_CHECK( ( flags & 1 ) == 0 );                   // escape flag must be clear
    if ( partial )
        TASK_CHECK( reader.get( 32 ) == frames );
    uint32_t            mixBits = reader.get( 8 );
    uint32_t            mixRes = reader.get( 8 );
    TASK_CHECK( mixRes != 0 );                          // matrixing expected for this material
    (void) mixBits;

    ALACDecoder         decoder;
    TASK_CHECK( decoder.Init( cookie.data(), cookieSize ) == ALAC_noErr );

    std::vector<uint8_t> decoded( input.size() + 64, 0 );
    BitBuffer           bits;
    BitBufferInit( &bits, encoded.data(), (uint32_t) size );
    uint32_t            got = 0;
    TASK_CHECK( decoder.Decode( &bits, decoded.data(), frames, channels, &got ) == ALAC_noErr );
    TASK_CHECK( got == frames );
    TASK_CHECK( check_seq_u8( "compat.pcm", decoded.data(), input.data(), (int) input.size() ) );
    return 0;
}

/* ------------------------------------------------------------------ */
/* registration                                                        */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] =
{
    { "predictor", "order4_unrolled",          case_pred_order4 },
    { "predictor", "order8_unrolled",          case_pred_order8 },
    { "predictor", "general_order",            case_pred_general },
    { "predictor", "special_branches",         case_pred_special },
    { "matrix",    "unmix16_rounding",         case_matrix16 },
    { "matrix",    "unmix24_shift_off",        case_matrix24 },
    { "matrix",    "unmix32_shift_off",        case_matrix32 },
    { "packets",   "handbuilt_cpe_sce_24bit",  case_packet_handbuilt },
    { "packets",   "production_encoder_compat", case_packet_compat },
};

const size_t task_case_count = sizeof( task_cases ) / sizeof( task_cases[0] );
