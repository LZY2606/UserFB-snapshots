/**
 * @file target.cpp
 * @brief Verification program for r8b::CDSPResamplerBank.
 *
 * Four independently selectable test groups:
 *
 *   equivalence - per-channel numerical equivalence against independent
 *                 r8b::CDSPResampler objects over the required rate ratios,
 *                 phases, channel counts and input block schedules.
 *   schedule    - shared control-work counters (fractional phase advances,
 *                 polynomial coefficient evaluations, half-band scheduling)
 *                 are non-zero where the corresponding path is active and do
 *                 not scale with channel count.
 *   boundaries  - fragmented block schedules, zero-length calls, clear/reuse
 *                 equivalence and internal output-pointer lifetime handling.
 *   realtime    - process() and clear() perform no heap allocation or
 *                 deallocation after construction.
 *
 * All signals are self-generated PCM; no audio files, third-party packages or
 * audio devices are used.
 */

#include "CDSPResamplerBank.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

using namespace r8b;

namespace {

const int MaxBlock = 256;
const int MaxChannels = 6;
const double ReqTransBand = 2.0;
const double ReqAtten = 120.0;
const long InputFrames = 4096;

struct Ratio
{
    double Src;
    double Dst;
    const char* Name;
};

const Ratio Ratios[] = {
    { 44100.0, 48000.0, "44100->48000" },
    { 48000.0, 16000.0, "48000->16000" },
    { 32000.0, 96000.0, "32000->96000" },
    { 44056.3, 47999.7, "44056.3->47999.7" },
    { 48000.0, 192000.0, "48000->192000" },
    { 192000.0, 48000.0, "192000->48000" }
};

const int RatioCount = (int) ( sizeof( Ratios ) / sizeof( Ratios[ 0 ]));
const int Blocks[] = { 1, 7, 31, 256 };
const int BlockCount = (int) ( sizeof( Blocks ) / sizeof( Blocks[ 0 ]));
const int ChannelOpts[] = { 1, 2, 6 };
const int ChannelOptCount =
    (int) ( sizeof( ChannelOpts ) / sizeof( ChannelOpts[ 0 ]));

/**
 * @brief Distinct per-channel test signal: independent sines, an impulse
 * train and a phase-inverted pair, so shared history misuse cannot hide
 * behind identical channels.
 */

double signal( const int ch, const long i )
{
    switch( ch % 6 )
    {
        case 0:
            return( 0.8 * std::sin( i * 0.0713 ));
        case 1:
            // Exact phase inversion of channel 0.
            return( -0.8 * std::sin( i * 0.0713 ));
        case 2:
            return( 0.5 * std::sin( i * 0.0177 + 1.3 ) + 0.25 );
        case 3:
            return( ( i % 97 == 13 ) ? 1.0 : 0.0 );
        case 4:
            return( 0.3 * std::sin( i * 0.113 ) -
                0.4 * std::sin( i * 0.031 ));
        default:
            return( 0.6 * std::sin( i * 0.0441 + 0.7 ) +
                0.2 * std::sin( i * 0.191 ));
    }
}

void fail( const char* const what )
{
    std::fprintf( stderr, "FAIL: %s\n", what );
    std::exit( 1 );
}

/**
 * @brief Drives one bank over InputFrames using the given block schedule and
 * records every channel's concatenated output. Returned internal pointers are
 * consumed before the next process() call.
 */

void renderBank( CDSPResamplerBank& bank, const int block,
    std::vector< std::vector< double > >& out )
{
    const int ch = bank.getChannelCount();
    out.assign( ch, std::vector< double >());

    std::vector< std::vector< double > > in( ch,
        std::vector< double >( MaxBlock ));
    double* ip[ MaxChannels ];
    long pos = 0;

    while( pos < InputFrames )
    {
        const int n = ( block < InputFrames - pos ) ?
            block : (int) ( InputFrames - pos );

        for( int c = 0; c < ch; c++ )
        {
            for( int i = 0; i < n; i++ )
            {
                in[ c ][ i ] = signal( c, pos + i );
            }

            ip[ c ] = in[ c ].data();
        }

        double* op[ MaxChannels ];
        const int bn = bank.process( ip, n, op );

        for( int c = 0; c < ch; c++ )
        {
            out[ c ].insert( out[ c ].end(), op[ c ], op[ c ] + bn );
        }

        pos += n;
    }
}

/**
 * @brief Per-call comparison of a bank against independent mono resamplers,
 * checking frame counts and the 1e-10 absolute error bound.
 */

void comparePerCall( const Ratio& r, const int ch, const int block,
    const EDSPFilterPhaseResponse phase, long& checks )
{
    CDSPResamplerBank bank( ch, r.Src, r.Dst, MaxBlock, ReqTransBand,
        ReqAtten, phase );
    CDSPResampler* mono[ MaxChannels ];

    for( int c = 0; c < ch; c++ )
    {
        mono[ c ] = new CDSPResampler( r.Src, r.Dst, MaxBlock, ReqTransBand,
            ReqAtten, phase );
    }

    std::vector< std::vector< double > > in( ch,
        std::vector< double >( MaxBlock ));
    double* ip[ MaxChannels ];
    long pos = 0;
    double maxerr = 0.0;

    while( pos < InputFrames )
    {
        const int n = ( block < InputFrames - pos ) ?
            block : (int) ( InputFrames - pos );

        for( int c = 0; c < ch; c++ )
        {
            for( int i = 0; i < n; i++ )
            {
                in[ c ][ i ] = signal( c, pos + i );
            }

            ip[ c ] = in[ c ].data();
        }

        double* bop[ MaxChannels ];
        const int bn = bank.process( ip, n, bop );

        for( int c = 0; c < ch; c++ )
        {
            double* mop = nullptr;
            const int mn = mono[ c ] -> process( in[ c ].data(), n, mop );

            if( mn != bn )
            {
                fail( "frame count mismatch" );
            }

            for( int i = 0; i < bn; i++ )
            {
                if( !std::isfinite( bop[ c ][ i ]) ||
                    !std::isfinite( mop[ i ]))
                {
                    fail( "non-finite output" );
                }

                const double e = std::fabs( bop[ c ][ i ] - mop[ i ]);

                if( e > maxerr ) maxerr = e;
            }
        }

        pos += n;
    }

    if( maxerr > 1e-10 )
    {
        std::fprintf( stderr, "ratio=%s ch=%d block=%d err=%.3g\n",
            r.Name, ch, block, maxerr );
        fail( "absolute error exceeds 1e-10" );
    }

    for( int c = 0; c < ch; c++ )
    {
        delete mono[ c ];
    }

    checks++;
}

int testEquivalence()
{
    long checks = 0;

    for( int ri = 0; ri < RatioCount; ri++ )
    {
        for( int pi = 0; pi < 2; pi++ )
        {
            const EDSPFilterPhaseResponse phase =
                pi ? fprMinPhase : fprLinearPhase;

            for( int bi = 0; bi < BlockCount; bi++ )
            {
                for( int ci = 0; ci < ChannelOptCount; ci++ )
                {
                    comparePerCall( Ratios[ ri ], ChannelOpts[ ci ],
                        Blocks[ bi ], phase, checks );
                }
            }
        }
    }

    // Equal sample rates must pass input through unchanged, for every
    // supported channel count and block schedule.

    for( int ci = 0; ci < ChannelOptCount; ci++ )
    {
        const int ch = ChannelOpts[ ci ];
        CDSPResamplerBank bank( ch, 48000.0, 48000.0, MaxBlock );

        for( int bi = 0; bi < BlockCount; bi++ )
        {
            const int block = Blocks[ bi ];
            std::vector< std::vector< double > > in( ch,
                std::vector< double >( block ));
            double* ip[ MaxChannels ];

            for( int c = 0; c < ch; c++ )
            {
                for( int i = 0; i < block; i++ )
                {
                    in[ c ][ i ] = signal( c, i + bi * 31 );
                }

                ip[ c ] = in[ c ].data();
            }

            double* op[ MaxChannels ];
            const int n = bank.process( ip, block, op );

            if( n != block ) fail( "passthrough frame count" );

            for( int c = 0; c < ch; c++ )
            {
                if( op[ c ] != ip[ c ]) fail( "passthrough must alias input" );

                for( int i = 0; i < block; i++ )
                {
                    if( op[ c ][ i ] != in[ c ][ i ])
                    {
                        fail( "passthrough altered samples" );
                    }
                }
            }

            checks++;
        }
    }

    std::printf( "PASS equivalence checks=%ld\n", checks );
    return( 0 );
}

/**
 * @brief Feeds an identical frame schedule and returns the resulting control
 * counters. Only the schedule matters, so the same input content is used for
 * every channel.
 */

CDSPResamplerBankCounters measureCounters( const double sr1,
    const double sr2, const int ch )
{
    CDSPResamplerBank bank( ch, sr1, sr2, MaxBlock, ReqTransBand, ReqAtten );
    static double sample[ MaxBlock ];

    for( int i = 0; i < MaxBlock; i++ ) sample[ i ] = 0.25 * std::sin( i * 0.05 );

    double* ip[ MaxChannels ];

    for( int c = 0; c < ch; c++ ) ip[ c ] = sample;

    double* op[ MaxChannels ];

    for( int k = 0; k < 12; k++ )
    {
        bank.process( ip, MaxBlock, op );
    }

    return( bank.getCounters());
}

int testSchedule()
{
    long checks = 0;

    // Half-band scheduling must be active for the 4x up/down conversions and
    // identical across 1, 2 and 6 channels.

    const Ratio hbRatios[] = {
        { 48000.0, 192000.0, "HB up" },
        { 192000.0, 48000.0, "HB down" }
    };

    for( int ri = 0; ri < 2; ri++ )
    {
        CDSPResamplerBankCounters ref;
        bool have = false;

        for( int ci = 0; ci < ChannelOptCount; ci++ )
        {
            const CDSPResamplerBankCounters cur = measureCounters(
                hbRatios[ ri ].Src, hbRatios[ ri ].Dst, ChannelOpts[ ci ]);

            if( cur.HBSchedules <= 0 )
            {
                fail( "half-band scheduling counter not positive" );
            }

            if( !have )
            {
                ref = cur;
                have = true;

                if( ref.HBSchedules <= 0 ) fail( "HB reference not active" );
            }
            else
            if( cur.HBSchedules != ref.HBSchedules ||
                cur.PhaseAdvances != ref.PhaseAdvances ||
                cur.FracCoeffEvals != ref.FracCoeffEvals )
            {
                fail( "half-band control work scales with channel count" );
            }
        }

        checks++;
    }

    // Polynomial fractional coefficient evaluation must be active only on the
    // genuinely fractional (non-whole-step) ratio, and be channel-count
    // independent.

    {
        CDSPResamplerBankCounters ref;
        bool have = false;

        for( int ci = 0; ci < ChannelOptCount; ci++ )
        {
            const CDSPResamplerBankCounters cur = measureCounters(
                44056.3, 47999.7, ChannelOpts[ ci ]);

            if( cur.FracCoeffEvals <= 0 )
            {
                fail( "fractional coefficient counter not positive on "
                    "polynomial path" );
            }

            if( cur.PhaseAdvances <= 0 )
            {
                fail( "phase advance counter not positive" );
            }

            if( !have )
            {
                ref = cur;
                have = true;
            }
            else
            if( cur.FracCoeffEvals != ref.FracCoeffEvals ||
                cur.PhaseAdvances != ref.PhaseAdvances ||
                cur.HBSchedules != ref.HBSchedules )
            {
                fail( "fractional control work scales with channel count" );
            }
        }

        checks++;
    }

    // Whole-numbered stepping still advances the phase, but must not perform
    // polynomial coefficient evaluation. A zero count here is expected and
    // must not be mistaken for shared scheduling evidence.

    {
        const CDSPResamplerBankCounters c1 =
            measureCounters( 44100.0, 48000.0, 1 );
        const CDSPResamplerBankCounters c6 =
            measureCounters( 44100.0, 48000.0, 6 );

        if( c1.PhaseAdvances <= 0 ) fail( "whole-step phase not advancing" );
        if( c1.FracCoeffEvals != 0 )
        {
            fail( "whole-step path must not evaluate polynomial coefficients" );
        }

        if( c1.PhaseAdvances != c6.PhaseAdvances ||
            c1.FracCoeffEvals != c6.FracCoeffEvals ||
            c1.HBSchedules != c6.HBSchedules )
        {
            fail( "whole-step control work scales with channel count" );
        }

        checks++;
    }

    // On a pipeline with no fractional and no half-band stage (exact 3x
    // conversion) the corresponding counters stay zero: the shared scheduling
    // claim is only made where the path exists.

    {
        const CDSPResamplerBankCounters c =
            measureCounters( 32000.0, 96000.0, 6 );

        if( c.PhaseAdvances != 0 || c.FracCoeffEvals != 0 ||
            c.HBSchedules != 0 )
        {
            fail( "unexpected control work on pure block-convolver pipeline" );
        }

        checks++;
    }

    std::printf( "PASS schedule checks=%ld\n", checks );
    return( 0 );
}

int testBoundaries()
{
    long checks = 0;

    // Fragmented schedules must produce output identical to a whole-block
    // schedule with no sample displacement, for every ratio and phase.

    for( int ri = 0; ri < RatioCount; ri++ )
    {
        for( int pi = 0; pi < 2; pi++ )
        {
            const EDSPFilterPhaseResponse phase =
                pi ? fprMinPhase : fprLinearPhase;

            std::vector< std::vector< double > > refOut;
            {
                CDSPResamplerBank whole( MaxChannels, Ratios[ ri ].Src,
                    Ratios[ ri ].Dst, MaxBlock, ReqTransBand, ReqAtten,
                    phase );
                renderBank( whole, 256, refOut );
            }

            for( int bi = 0; bi < BlockCount; bi++ )
            {
                CDSPResamplerBank frag( MaxChannels, Ratios[ ri ].Src,
                    Ratios[ ri ].Dst, MaxBlock, ReqTransBand, ReqAtten,
                    phase );
                std::vector< std::vector< double > > fragOut;
                renderBank( frag, Blocks[ bi ], fragOut );

                for( int c = 0; c < MaxChannels; c++ )
                {
                    if( fragOut[ c ].size() != refOut[ c ].size())
                    {
                        fail( "fragmented schedule changed frame count" );
                    }

                    double e = 0.0;

                    for( size_t i = 0; i < refOut[ c ].size(); i++ )
                    {
                        e = std::max( e,
                            std::fabs( fragOut[ c ][ i ] - refOut[ c ][ i ]));
                    }

                    // No sample displacement: identical frame count and the
                    // same values within the floating-point tolerance (the FFT
                    // overlap-save path accumulates rounding differently as
                    // block boundaries move, but never shifts samples).

                    if( e > 1e-10 )
                    {
                        std::fprintf( stderr, "frag ch=%d err=%.3g\n", c, e );
                        fail( "fragmented schedule displaced output" );
                    }
                }
            }

            checks++;
        }
    }

    // clear() must make a reused object behave exactly like a freshly
    // constructed one.

    for( int ri = 0; ri < RatioCount; ri++ )
    {
        CDSPResamplerBank reused( MaxChannels, Ratios[ ri ].Src,
            Ratios[ ri ].Dst, MaxBlock, ReqTransBand, ReqAtten );
        CDSPResamplerBank fresh( MaxChannels, Ratios[ ri ].Src,
            Ratios[ ri ].Dst, MaxBlock, ReqTransBand, ReqAtten );

        // Put some history into the reused object, then clear it.
        std::vector< std::vector< double > > warm( MaxChannels,
            std::vector< double >( MaxBlock ));
        double* wip[ MaxChannels ];

        for( int c = 0; c < MaxChannels; c++ ) wip[ c ] = warm[ c ].data();

        double* wop[ MaxChannels ];

        for( int k = 0; k < 8; k++ ) reused.process( wip, MaxBlock, wop );

        reused.clear();

        std::vector< std::vector< double > > reusedOut;
        std::vector< std::vector< double > > freshOut;
        renderBank( reused, 31, reusedOut );
        renderBank( fresh, 31, freshOut );

        for( int c = 0; c < MaxChannels; c++ )
        {
            if( reusedOut[ c ].size() != freshOut[ c ].size())
            {
                fail( "clear/reuse frame count differs" );
            }

            for( size_t i = 0; i < freshOut[ c ].size(); i++ )
            {
                if( reusedOut[ c ][ i ] != freshOut[ c ][ i ])
                {
                    fail( "clear/reuse output differs from fresh object" );
                }
            }
        }

        checks++;
    }

    // Zero-length calls must not advance audio and must not move counters.

    for( int ri = 0; ri < RatioCount; ri++ )
    {
        CDSPResamplerBank a( MaxChannels, Ratios[ ri ].Src, Ratios[ ri ].Dst,
            MaxBlock, ReqTransBand, ReqAtten );
        CDSPResamplerBank b( MaxChannels, Ratios[ ri ].Src, Ratios[ ri ].Dst,
            MaxBlock, ReqTransBand, ReqAtten );

        std::vector< std::vector< double > > in( MaxChannels,
            std::vector< double >( MaxBlock ));
        double* ip[ MaxChannels ];

        for( int c = 0; c < MaxChannels; c++ )
        {
            for( int i = 0; i < MaxBlock; i++ )
            {
                in[ c ][ i ] = signal( c, i + 100 );
            }

            ip[ c ] = in[ c ].data();
        }

        double* aop[ MaxChannels ];
        double* bop[ MaxChannels ];

        a.process( ip, 64, aop );
        b.process( ip, 64, bop );

        const CDSPResamplerBankCounters before = a.getCounters();
        double* zop[ MaxChannels ];
        const int zlen = a.process( ip, 0, zop );

        if( zlen != 0 ) fail( "zero-length call returned frames" );

        const CDSPResamplerBankCounters after = a.getCounters();

        if( after.PhaseAdvances != before.PhaseAdvances ||
            after.FracCoeffEvals != before.FracCoeffEvals ||
            after.HBSchedules != before.HBSchedules )
        {
            fail( "zero-length call advanced control state" );
        }

        // Subsequent output must stay aligned with the uninterrupted object.

        std::vector< std::vector< double > > aOut;
        std::vector< std::vector< double > > bOut;
        renderBank( a, 31, aOut );
        renderBank( b, 31, bOut );

        for( int c = 0; c < MaxChannels; c++ )
        {
            if( aOut[ c ].size() != bOut[ c ].size())
            {
                fail( "zero-length call altered subsequent frame count" );
            }

            for( size_t i = 0; i < bOut[ c ].size(); i++ )
            {
                if( aOut[ c ][ i ] != bOut[ c ][ i ])
                {
                    fail( "zero-length call displaced subsequent output" );
                }
            }
        }

        checks++;
    }

    std::printf( "PASS boundaries checks=%ld\n", checks );
    return( 0 );
}

// ---------------------------------------------------------------------------
// Allocation monitoring
// ---------------------------------------------------------------------------

volatile long g_newCount = 0;
volatile long g_deleteCount = 0;

} // namespace

void* operator new( std::size_t s )
{
    ++g_newCount;

    void* const p = std::malloc( s );

    if( !p ) throw std::bad_alloc();

    return( p );
}

void* operator new[]( std::size_t s )
{
    ++g_newCount;

    void* const p = std::malloc( s );

    if( !p ) throw std::bad_alloc();

    return( p );
}

void operator delete( void* p ) throw()
{
    ++g_deleteCount;
    std::free( p );
}

void operator delete[]( void* p ) throw()
{
    ++g_deleteCount;
    std::free( p );
}

#if __cplusplus >= 201402L
void operator delete( void* p, std::size_t ) throw()
{
    ++g_deleteCount;
    std::free( p );
}

void operator delete[]( void* p, std::size_t ) throw()
{
    ++g_deleteCount;
    std::free( p );
}
#endif // C++14 sized delete

namespace {

int testRealtime()
{
    long checks = 0;

    for( int ri = 0; ri < RatioCount; ri++ )
    {
        for( int pi = 0; pi < 2; pi++ )
        {
            const EDSPFilterPhaseResponse phase =
                pi ? fprMinPhase : fprLinearPhase;

            // Construction (including all workspace and FFT allocation)
            // happens before monitoring starts.
            CDSPResamplerBank* const bank = new CDSPResamplerBank(
                MaxChannels, Ratios[ ri ].Src, Ratios[ ri ].Dst, MaxBlock,
                ReqTransBand, ReqAtten, phase );

            static double in[ MaxChannels ][ MaxBlock ];

            for( int c = 0; c < MaxChannels; c++ )
            {
                for( int i = 0; i < MaxBlock; i++ )
                {
                    in[ c ][ i ] = signal( c, i );
                }
            }

            double* ip[ MaxChannels ];

            for( int c = 0; c < MaxChannels; c++ ) ip[ c ] = in[ c ];

            g_newCount = 0;
            g_deleteCount = 0;

            // Exercise block boundaries, ring wraparound and the direct
            // convolution paths via varied block schedules.
            for( int k = 0; k < 40; k++ )
            {
                double* op[ MaxChannels ];
                const int block = Blocks[ k % BlockCount ];
                bank -> process( ip, block, op );

                // Zero-length calls must also stay allocation-free.
                double* zop[ MaxChannels ];
                bank -> process( ip, 0, zop );
            }

            bank -> clear();

            // A second run after clear() must also allocate nothing.
            for( int k = 0; k < 16; k++ )
            {
                double* op[ MaxChannels ];
                bank -> process( ip, Blocks[ ( k + 1 ) % BlockCount ], op );
            }

            if( g_newCount != 0 || g_deleteCount != 0 )
            {
                std::fprintf( stderr,
                    "allocation during process/clear: new=%ld delete=%ld\n",
                    (long) g_newCount, (long) g_deleteCount );
                fail( "process/clear must not allocate or free heap memory" );
            }

            delete bank;
            checks++;
        }
    }

    std::printf( "PASS realtime checks=%ld\n", checks );
    return( 0 );
}

} // namespace

int main( int argc, char** argv )
{
    if( argc != 2 )
    {
        std::fprintf( stderr,
            "usage: %s equivalence|schedule|boundaries|realtime\n",
            argv[ 0 ] );
        return( 2 );
    }

    const char* const group = argv[ 1 ];

    if( std::strcmp( group, "equivalence" ) == 0 ) return( testEquivalence());
    if( std::strcmp( group, "schedule" ) == 0 ) return( testSchedule());
    if( std::strcmp( group, "boundaries" ) == 0 ) return( testBoundaries());
    if( std::strcmp( group, "realtime" ) == 0 ) return( testRealtime());

    std::fprintf( stderr, "unknown group: %s\n", group );
    return( 2 );
}
