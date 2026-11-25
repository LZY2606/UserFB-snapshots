// Game Boy APU independent NR50 left/right master volume and NR51 routing
// tests. The transition expectations are generated independently with Blip
// pulse synths/buffers rather than by comparing two Gb_Apu instances.

#include "task_support/check.h"

#include "Gb_Apu.h"
#include "Multi_Buffer.h"
#include "Blip_Buffer.h"
#include "gme.h"
#include "gbs_fixture.h"

#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>

namespace {

typedef std::vector<short> Samples;

const int kRate   = 48000;
const int kMsec   = 250;
const int kClock  = 4194304;
const double kUnit = 0.60 / 4 / 15.0 / 2.0 / 8.0; // Gb_Apu::volume(1.0) unit

struct Ev { int t; unsigned addr; int data; };

void reg( std::vector<Ev>& s, int t, int r, int v )
{
	Ev e = { t, (unsigned) (0xff00 + r), v };
	s.push_back( e );
}

void sort_ev( std::vector<Ev>& s )
{
	std::stable_sort( s.begin(), s.end(),
			[] ( const Ev& a, const Ev& b ) { return a.t < b.t; } );
}

void setup_stereo( Stereo_Buffer& b )
{
	b.set_sample_rate( kRate, kMsec );
	b.clock_rate( kClock );
	b.bass_freq( 0 );
}

Samples read_buf( Blip_Buffer* x )
{
	Samples v( (size_t) x->samples_avail() );
	if ( !v.empty() )
		x->read_samples( v.data(), (long) v.size() );
	return v;
}

struct Raw { Samples c, l, r; };

Raw read_raw( Stereo_Buffer& b )
{
	Raw o;
	o.c = read_buf( b.center() );
	o.l = read_buf( b.left() );
	o.r = read_buf( b.right() );
	return o;
}

// f = 1 square with duty 1/2 and volume 15: its only event in a short
// window is +15 at time 0, so transition expectations stay explicit.
void held_square( std::vector<Ev>& s, int nr50, int nr51 )
{
	reg( s, 0, 0x26, 0x80 );
	reg( s, 0, 0x24, nr50 );
	reg( s, 0, 0x25, nr51 );
	reg( s, 0, 0x10, 0x00 );
	reg( s, 0, 0x11, 0x80 );
	reg( s, 0, 0x12, 0xf0 );
	reg( s, 0, 0x13, 0x01 );
	reg( s, 0, 0x14, 0x80 );
}

Raw run_whole( const std::vector<Ev>& evs, int T, bool mono = false )
{
	Stereo_Buffer b;
	setup_stereo( b );
	Gb_Apu a;
	if ( mono )
		a.output( b.center() );
	else
		a.output( b.center(), b.left(), b.right() );
	a.reset();
	for ( size_t i = 0; i < evs.size(); ++i )
		a.write_register( evs [i].t, evs [i].addr, evs [i].data );
	a.end_frame( T );
	b.end_frame( T );
	return read_raw( b );
}

Raw run_chunked( const std::vector<Ev>& evs, int T, int chunk, bool mono = false )
{
	Stereo_Buffer b;
	setup_stereo( b );
	Gb_Apu a;
	if ( mono )
		a.output( b.center() );
	else
		a.output( b.center(), b.left(), b.right() );
	a.reset();

	size_t i = 0;
	int base = 0;
	while ( base < T )
	{
		int c = std::min( chunk, T - base );
		int stop = base + c;
		while ( i < evs.size() && evs [i].t < stop )
		{
			int lt = std::max( 0, evs [i].t - base );
			a.write_register( lt, evs [i].addr, evs [i].data );
			++i;
		}
		a.end_frame( c );
		b.end_frame( c );
		base += c;
	}
	return read_raw( b );
}

struct Diff { int bad; int maxd; bool size_mismatch; };

Diff diff_samples( const Samples& a, const Samples& b )
{
	Diff d = { 0, 0, a.size() != b.size() };
	size_t n = std::min( a.size(), b.size() );
	for ( size_t i = 0; i < n; ++i )
	{
		int x = std::abs( (int) a [i] - (int) b [i] );
		d.maxd = std::max( d.maxd, x );
		if ( a [i] != b [i] )
			++d.bad;
	}
	return d;
}

bool exact( const Samples& a, const Samples& b, const char* label )
{
	Diff d = diff_samples( a, b );
	if ( d.size_mismatch || d.bad )
		std::printf( "  %s: size_match=%d differing=%d max_diff=%d\n",
				label, !d.size_mismatch, d.bad, d.maxd );
	return !d.size_mismatch && d.bad == 0;
}

// Independent Blip pulse reference using the same synth quality/range and the
// same per-field gain volumes as the production gain synths.
struct Ref
{
	Blip_Buffer buf [3]; // 0 center, 1 left, 2 right
	Blip_Synth<blip_good_quality,1> sq [8];

	Ref()
	{
		for ( int i = 0; i < 3; ++i )
		{
			buf [i].set_sample_rate( kRate, kMsec );
			buf [i].clock_rate( kClock );
			buf [i].bass_freq( 0 );
		}
		for ( int g = 0; g < 8; ++g )
			sq [g].volume( (g + 1) * kUnit );
	}

	void ev( int which, int gain, int t, int delta )
	{
		sq [gain].offset( t, delta, &buf [which] );
	}

	Samples get( int which, int T )
	{
		buf [which].end_frame( T );
		return read_buf( &buf [which] );
	}
};

// ---------- gain group ----------

void program_osc( Gb_Apu& a, int kind )
{
	if ( kind == 0 )
	{
		a.write_register( 0, 0xff11, 0x80 );
		a.write_register( 0, 0xff12, 0xf0 );
		a.write_register( 0, 0xff13, 0x00 );
		a.write_register( 0, 0xff14, 0x87 );
	}
	else if ( kind == 1 )
	{
		a.write_register( 0, 0xff16, 0x80 );
		a.write_register( 0, 0xff17, 0xf0 );
		a.write_register( 0, 0xff18, 0x00 );
		a.write_register( 0, 0xff19, 0x87 );
	}
	else if ( kind == 2 )
	{
		static const unsigned char wave [16] = {
			0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF,
			0xFE,0xDC,0xBA,0x98,0x76,0x54,0x32,0x10
		};
		for ( int i = 0; i < 16; ++i )
			a.write_register( 0, 0xff30 + i, wave [i] );
		a.write_register( 0, 0xff1a, 0x80 );
		a.write_register( 0, 0xff1b, 0x00 );
		a.write_register( 0, 0xff1c, 0x20 ); // 100% wave level
		a.write_register( 0, 0xff1d, 0x00 );
		a.write_register( 0, 0xff1e, 0x87 );
	}
	else
	{
		a.write_register( 0, 0xff20, 0x00 );
		a.write_register( 0, 0xff21, 0xf0 );
		a.write_register( 0, 0xff22, 0x08 );
		a.write_register( 0, 0xff23, 0x87 );
	}
}

int route_nr51( int kind )
{
	static const int bits [4] = { 0x11, 0x22, 0x44, 0x88 };
	return bits [kind];
}

Samples render_osc( int kind, int nr50, int nr51 )
{
	Stereo_Buffer b;
	setup_stereo( b );
	Gb_Apu a;
	a.output( b.center(), b.left(), b.right() );
	a.reset();
	a.write_register( 0, 0xff26, 0x80 );
	a.write_register( 0, 0xff24, nr50 );
	a.write_register( 0, 0xff25, nr51 );
	program_osc( a, kind );
	const int T = 120000;
	a.end_frame( T );
	b.end_frame( T );
	Samples out( (size_t) b.samples_avail() );
	b.read_samples( out.data(), (long) out.size() );
	return out;
}

double rms_channel( const Samples& o, int chan )
{
	double sum = 0;
	long n = 0;
	for ( size_t i = 400 + chan; i + 1 < o.size(); i += 2 )
	{
		sum += (double) o [i] * o [i];
		++n;
	}
	return n ? std::sqrt( sum / n ) : 0.0;
}

int peak_abs( const Samples& o )
{
	int p = 0;
	for ( size_t i = 0; i < o.size(); ++i )
		p = std::max( p, std::abs( (int) o [i] ) );
	return p;
}

int gain_ratios_case()
{
	const char* names [4] = { "square1", "square2", "wave", "noise" };
	for ( int k = 0; k < 4; ++k )
	{
		Samples hi = render_osc( k, 0x70, route_nr51( k ) );
		double l = rms_channel( hi, 0 );
		double r = rms_channel( hi, 1 );
		double ratio = r > 1e-9 ? l / r : 0.0;
		std::printf( "  %s 0x70 L/R=%.3f (L=%.0f R=%.0f)\n",
				names [k], ratio, l, r );
		TASK_CHECK( peak_abs( hi ) > 100 );
		TASK_CHECK( ratio > 6.5 );
		TASK_CHECK( ratio < 10.0 );

		Samples lo = render_osc( k, 0x07, route_nr51( k ) );
		double l2 = rms_channel( lo, 0 );
		double r2 = rms_channel( lo, 1 );
		double inv = l2 > 1e-9 ? r2 / l2 : 0.0; // R/L, expect ~8
		std::printf( "  %s 0x07 R/L=%.3f\n", names [k], inv );
		TASK_CHECK( inv > 6.5 );
		TASK_CHECK( inv < 10.0 );
	}
	return 0;
}

int gain_minimum_case()
{
	for ( int k = 0; k < 4; ++k )
	{
		// Right field is zero in 0x70, left field zero in 0x07.
		Samples a = render_osc( k, 0x70, route_nr51( k ) );
		Samples b = render_osc( k, 0x07, route_nr51( k ) );
		double right_min = rms_channel( a, 1 );
		double left_min  = rms_channel( b, 0 );
		std::printf( "  osc%d min right=%.1f min left=%.1f\n",
				k, right_min, left_min );
		TASK_CHECK( right_min > 40.0 ); // field 0 is gain 1/8, not mute
		TASK_CHECK( left_min > 40.0 );

		Samples both = render_osc( k, 0x00, 0xff );
		TASK_CHECK( rms_channel( both, 0 ) > 40.0 );
		TASK_CHECK( rms_channel( both, 1 ) > 40.0 );
	}
	return 0;
}

// ---------- transitions group ----------

const int kTrTime = 6000;
const int kChange = 2000;

int trans_volume_oracle_case()
{
	// symmetric center 0x77 -> 0x00: retract +15 g7, re-add +15 g0 center
	{
		Ref ref;
		ref.ev( 0, 7, 0, 15 );
		ref.ev( 0, 7, kChange, -15 );
		ref.ev( 0, 0, kChange, 15 );
		Samples expect = ref.get( 0, kTrTime );

		std::vector<Ev> s;
		held_square( s, 0x77, 0x11 );
		reg( s, kChange, 0x24, 0x00 );
		Raw got = run_whole( s, kTrTime );
		TASK_CHECK( exact( expect, got.c, "vol center->min" ) );
	}
	// center 0x77 -> split 0x70: center removed, left g7 + right g0 added
	{
		Ref ref;
		ref.ev( 0, 7, 0, 15 );
		ref.ev( 0, 7, kChange, -15 );
		ref.ev( 1, 7, kChange, 15 );
		ref.ev( 2, 0, kChange, 15 );
		Samples ec = ref.get( 0, kTrTime );
		Samples el = ref.get( 1, kTrTime );
		Samples er = ref.get( 2, kTrTime );

		std::vector<Ev> s;
		held_square( s, 0x77, 0x11 );
		reg( s, kChange, 0x24, 0x70 );
		Raw got = run_whole( s, kTrTime );
		TASK_CHECK( exact( ec, got.c, "77->70 center" ) );
		TASK_CHECK( exact( el, got.l, "77->70 left" ) );
		TASK_CHECK( exact( er, got.r, "77->70 right" ) );
	}
	return 0;
}

int trans_routing_oracle_case()
{
	// center -> left only
	{
		Ref ref;
		ref.ev( 0, 7, 0, 15 );
		ref.ev( 0, 7, kChange, -15 );
		ref.ev( 1, 7, kChange, 15 );
		Samples ec = ref.get( 0, kTrTime );
		Samples el = ref.get( 1, kTrTime );

		std::vector<Ev> s;
		held_square( s, 0x77, 0x11 );
		reg( s, kChange, 0x25, 0x10 );
		Raw got = run_whole( s, kTrTime );
		TASK_CHECK( exact( ec, got.c, "c->L center" ) );
		TASK_CHECK( exact( el, got.l, "c->L left" ) );
	}
	// center -> right only
	{
		Ref ref;
		ref.ev( 0, 7, 0, 15 );
		ref.ev( 0, 7, kChange, -15 );
		ref.ev( 2, 7, kChange, 15 );
		Samples ec = ref.get( 0, kTrTime );
		Samples er = ref.get( 2, kTrTime );

		std::vector<Ev> s;
		held_square( s, 0x77, 0x11 );
		reg( s, kChange, 0x25, 0x01 );
		Raw got = run_whole( s, kTrTime );
		TASK_CHECK( exact( ec, got.c, "c->R center" ) );
		TASK_CHECK( exact( er, got.r, "c->R right" ) );
	}
	// asymmetric split 0x70 -> symmetric 0x77 returns to shared center
	{
		Ref ref;
		ref.ev( 1, 7, 0, 15 );
		ref.ev( 2, 0, 0, 15 );
		ref.ev( 1, 7, kChange, -15 );
		ref.ev( 2, 0, kChange, -15 );
		ref.ev( 0, 7, kChange, 15 );
		Samples ec = ref.get( 0, kTrTime );
		Samples el = ref.get( 1, kTrTime );
		Samples er = ref.get( 2, kTrTime );

		std::vector<Ev> s;
		held_square( s, 0x70, 0x11 );
		reg( s, kChange, 0x24, 0x77 );
		Raw got = run_whole( s, kTrTime );
		TASK_CHECK( exact( ec, got.c, "split->c center" ) );
		TASK_CHECK( exact( el, got.l, "split->c left" ) );
		TASK_CHECK( exact( er, got.r, "split->c right" ) );
	}
	return 0;
}

int trans_same_clock_case()
{
	// NR50 then NR51 at the same clock: call order kept, no retrigger.
	// Only the final left-only g7 endpoint re-adds the held level.
	Ref ref;
	ref.ev( 0, 7, 0, 15 );
	ref.ev( 0, 7, kChange, -15 );
	ref.ev( 1, 7, kChange, 15 );
	Samples ec = ref.get( 0, kTrTime );
	Samples el = ref.get( 1, kTrTime );

	std::vector<Ev> s;
	held_square( s, 0x77, 0x11 );
	reg( s, kChange, 0x24, 0x70 );
	reg( s, kChange, 0x25, 0x10 );
	Raw got = run_whole( s, kTrTime );
	TASK_CHECK( exact( ec, got.c, "same-clock center" ) );
	TASK_CHECK( exact( el, got.l, "same-clock left" ) );
	return 0;
}

int trans_nr52_case()
{
	Ref ref;
	ref.ev( 0, 7, 0, 15 );
	ref.ev( 0, 7, kChange, -15 );
	Samples expect = ref.get( 0, kTrTime );

	std::vector<Ev> s;
	held_square( s, 0x77, 0x11 );
	reg( s, kChange, 0x26, 0x00 );
	Raw got = run_whole( s, kTrTime );
	TASK_CHECK( exact( expect, got.c, "nr52 retract" ) );

	bool tail_zero = true;
	for ( int i = (int) got.c.size() - 12; i < (int) got.c.size(); ++i )
		if ( i >= 0 && got.c [i] != 0 )
			tail_zero = false;
	TASK_CHECK( tail_zero ); // no lingering old level after power off
	return 0;
}

int trans_mono_case()
{
	// output(b) aliases center/left/right: both sides routed with 0x70 must
	// contribute exactly once at max gain (7), not twice.
	Ref ref;
	ref.ev( 0, 7, 0, 15 );
	Samples expect = ref.get( 0, kTrTime );

	std::vector<Ev> s;
	held_square( s, 0x70, 0x11 );
	Raw got = run_whole( s, kTrTime, true );
	TASK_CHECK( exact( expect, got.c, "mono max gain once" ) );

	// A second contribution would use delta 30 instead of 15.
	Ref ref_double;
	ref_double.ev( 0, 7, 0, 30 );
	Samples doubled = ref_double.get( 0, kTrTime );
	TASK_CHECK( diff_samples( doubled, got.c ).bad > 0 );
	return 0;
}

int trans_segmentation_case()
{
	// Event-rich square script crossing several gain/route changes.
	std::vector<Ev> sq;
	reg( sq, 0, 0x26, 0x80 );
	reg( sq, 0, 0x24, 0x77 );
	reg( sq, 0, 0x25, 0x11 );
	reg( sq, 0, 0x11, 0x80 );
	reg( sq, 0, 0x12, 0xf0 );
	reg( sq, 0, 0x13, 0x00 );
	reg( sq, 0, 0x14, 0x87 );
	reg( sq, 7000, 0x24, 0x70 );
	reg( sq, 12000, 0x25, 0x01 );
	reg( sq, 17000, 0x24, 0x77 );
	sort_ev( sq );
	const int T = 22000;
	Raw whole = run_whole( sq, T );
	for ( int idx = 0; idx < 3; ++idx )
	{
		int chunks [3] = { 1, 7, 257 };
		int c = chunks [idx];
		Raw part = run_chunked( sq, T, c );
		char label [32];
		std::snprintf( label, sizeof label, "square C=%d c", c );
		TASK_CHECK( exact( whole.c, part.c, label ) );
		std::snprintf( label, sizeof label, "square C=%d l", c );
		TASK_CHECK( exact( whole.l, part.l, label ) );
		std::snprintf( label, sizeof label, "square C=%d r", c );
		TASK_CHECK( exact( whole.r, part.r, label ) );
	}

	// Wave and noise use the medium-quality synth and resampled events.
	for ( int kind = 2; kind <= 3; ++kind )
	{
		std::vector<Ev> e;
		reg( e, 0, 0x26, 0x80 );
		reg( e, 0, 0x24, 0x77 );
		reg( e, 0, 0x25, route_nr51( kind ) );
		if ( kind == 2 )
		{
			for ( int i = 0; i < 16; ++i )
				reg( e, 0, 0x30 + i, 0x10 + i );
			reg( e, 0, 0x1a, 0x80 );
			reg( e, 0, 0x1c, 0x20 );
			reg( e, 0, 0x1d, 0x00 );
			reg( e, 0, 0x1e, 0x87 );
		}
		else
		{
			reg( e, 0, 0x20, 0x00 );
			reg( e, 0, 0x21, 0xf0 );
			reg( e, 0, 0x22, 0x08 );
			reg( e, 0, 0x23, 0x87 );
		}
		reg( e, 10000, 0x24, 0x70 );
		sort_ev( e );
		const int WT = 30000;
		Raw wwhole = run_whole( e, WT );
		int chunks [3] = { 1, 7, 257 };
		for ( int ci = 0; ci < 3; ++ci )
		{
			Raw p = run_chunked( e, WT, chunks [ci] );
			char label [40];
			std::snprintf( label, sizeof label, "osc%d C=%d l", kind, chunks [ci] );
			TASK_CHECK( exact( wwhole.l, p.l, label ) );
			std::snprintf( label, sizeof label, "osc%d C=%d r", kind, chunks [ci] );
			TASK_CHECK( exact( wwhole.r, p.r, label ) );
		}
	}
	return 0;
}

// ---------- playback group ----------

gme_err_t open_fixture( int nr50, Music_Emu** e )
{
	std::vector<unsigned char> d = task_gbs( (unsigned char) nr50 );
	return gme_open_data( &d [0], (long) d.size(), e, kRate );
}

Samples gbs_play( int nr50, long n )
{
	Music_Emu* e = 0;
	if ( open_fixture( nr50, &e ) )
		return Samples();
	gme_ignore_silence( e, 1 );
	if ( gme_start_track( e, 0 ) )
	{
		gme_delete( e );
		return Samples();
	}
	Samples out( (size_t) n );
	if ( gme_play( e, (int) n, out.data() ) )
		out.clear();
	gme_delete( e );
	return out;
}

Samples gbs_play_chunks( int nr50, const int* chunks, int count )
{
	Music_Emu* e = 0;
	if ( open_fixture( nr50, &e ) )
		return Samples();
	gme_ignore_silence( e, 1 );
	gme_start_track( e, 0 );
	Samples out;
	for ( int i = 0; i < count; ++i )
	{
		size_t pos = out.size();
		out.resize( pos + chunks [i] );
		if ( gme_play( e, chunks [i], out.data() + pos ) )
		{
			out.clear();
			break;
		}
	}
	gme_delete( e );
	return out;
}

Samples gbs_restart( int nr50, int before, long n )
{
	Music_Emu* e = 0;
	if ( open_fixture( nr50, &e ) )
		return Samples();
	gme_ignore_silence( e, 1 );
	gme_start_track( e, 0 );
	Samples tmp( before );
	gme_play( e, before, tmp.data() );
	gme_start_track( e, 0 ); // must clear previous track output state
	Samples out( (size_t) n );
	gme_play( e, (int) n, out.data() );
	gme_delete( e );
	return out;
}

int playback_direction_case()
{
	Music_Emu* e = 0;
	TASK_CHECK( open_fixture( 0x70, &e ) == 0 );
	TASK_CHECK( e != 0 );
	TASK_CHECK( gme_voice_count( e ) == 4 );
	gme_delete( e );

	Samples right_quiet = gbs_play( 0x70, 19200 );
	Samples left_quiet  = gbs_play( 0x07, 19200 );
	TASK_CHECK( right_quiet.size() == 19200 );
	TASK_CHECK( peak_abs( right_quiet ) > 100 );

	double l = rms_channel( right_quiet, 0 );
	double r = rms_channel( right_quiet, 1 );
	double ratio = r > 1e-9 ? l / r : 0;
	std::printf( "  GBS 0x70 L/R=%.3f\n", ratio );
	TASK_CHECK( ratio > 4.0 ); // audibly/measurably left-heavy
	TASK_CHECK( ratio < 12.0 );

	double l2 = rms_channel( left_quiet, 0 );
	double r2 = rms_channel( left_quiet, 1 );
	double inv = l2 > 1e-9 ? r2 / l2 : 0;
	std::printf( "  GBS 0x07 R/L=%.3f\n", inv );
	TASK_CHECK( inv > 4.0 );
	TASK_CHECK( inv < 12.0 );
	return 0;
}

int playback_fragmented_case()
{
	static const int chunks [] = { 2, 4, 6, 100, 256, 2, 1000, 512, 8, 4096 };
	int total = 0;
	for ( size_t i = 0; i < sizeof chunks / sizeof chunks [0]; ++i )
		total += chunks [i];

	Samples chunked = gbs_play_chunks( 0x70, chunks,
			sizeof chunks / sizeof chunks [0] );
	Samples whole = gbs_play( 0x70, total );
	TASK_CHECK( chunked.size() == (size_t) total );
	TASK_CHECK( exact( whole, chunked, "gbs fragmented 0x70" ) );

	Samples chunked_s = gbs_play_chunks( 0x77, chunks,
			sizeof chunks / sizeof chunks [0] );
	Samples whole_s = gbs_play( 0x77, total );
	TASK_CHECK( exact( whole_s, chunked_s, "gbs fragmented 0x77" ) );
	return 0;
}

int playback_restart_case()
{
	Samples restarted = gbs_restart( 0x70, 9600, 19200 );
	Samples fresh = gbs_play( 0x70, 19200 );
	TASK_CHECK( restarted.size() == fresh.size() );
	TASK_CHECK( exact( fresh, restarted, "gbs restart 0x70" ) );

	Samples restarted_s = gbs_restart( 0x77, 5000, 19200 );
	Samples fresh_s = gbs_play( 0x77, 19200 );
	TASK_CHECK( exact( fresh_s, restarted_s, "gbs restart 0x77" ) );
	return 0;
}

int playback_symmetric_case()
{
	// Symmetric NR50 must keep bit-identical left/right samples.
	Samples s = gbs_play( 0x77, 19200 );
	TASK_CHECK( s.size() == 19200 );
	int mismatch = 0;
	for ( size_t i = 0; i + 1 < s.size(); i += 2 )
		if ( s [i] != s [i + 1] )
			++mismatch;
	TASK_CHECK( mismatch == 0 );
	TASK_CHECK( peak_abs( s ) > 100 );

	// Sample-for-sample reference of the unchanged symmetric fixed path:
	// the 9600-short stream has the same FNV-1a hash as the pre-change APU.
	Samples ref9600 = gbs_play( 0x77, 9600 );
	uint64_t hash = 1469598103934665603ull;
	for ( size_t i = 0; i < ref9600.size(); ++i )
	{
		hash ^= (uint16_t) ref9600 [i];
		hash *= 1099511628211ull;
	}
	TASK_CHECK( hash == 0x3cdf4fb340844173ull );

	// Three-buffer center output and output(mono) of the same symmetric
	// fixed script expose identical center samples (single contribution).
	std::vector<Ev> ev;
	held_square( ev, 0x77, 0x11 );
	const int T = 20000;
	Raw stereo = run_whole( ev, T, false );
	Raw mono = run_whole( ev, T, true );
	TASK_CHECK( exact( stereo.c, mono.c, "stereo center == mono alias" ) );
	return 0;
}

} // namespace

const struct task_case task_cases [] = {
	{ "gain", "ratios",  gain_ratios_case },
	{ "gain", "minimum", gain_minimum_case },

	{ "transitions", "volume_oracle",  trans_volume_oracle_case },
	{ "transitions", "routing_oracle", trans_routing_oracle_case },
	{ "transitions", "same_clock",     trans_same_clock_case },
	{ "transitions", "nr52",           trans_nr52_case },
	{ "transitions", "mono_alias",     trans_mono_case },
	{ "transitions", "segmentation",   trans_segmentation_case },

	{ "playback", "gbs_direction",         playback_direction_case },
	{ "playback", "fragmented_reads",      playback_fragmented_case },
	{ "playback", "restart_clears",        playback_restart_case },
	{ "playback", "symmetric_reference",   playback_symmetric_case },
};

const size_t task_case_count = sizeof task_cases / sizeof task_cases [0];
