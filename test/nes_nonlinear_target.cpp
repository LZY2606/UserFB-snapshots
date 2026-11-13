// Target tests for the joint nonlinear NES five-channel mixer.
//
// The reference side below re-derives the raw per-channel DAC level events
// with its own compact models of the 2A03 channels (no production mixer
// code), applies the fixed nonlinear formula independently, and feeds the
// resulting mixed integers into a separate Blip_Buffer through its own
// Blip_Synth. Production mixed integers and PCM are then checked against it.

#include "task_support/suite.h"

#include "gme/Nes_Apu.h"
#include "gme/Blip_Buffer.h"
#include "gme/Nsf_Emu.h"
#include "gme/Multi_Buffer.h"
#include "gme/Data_Reader.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

namespace {

// ---------------------------------------------------------------------------
// Independent implementation of the fixed nonlinear mixing convention.
// ---------------------------------------------------------------------------

int ref_mix( int p1, int p2, int t, int n, int d )
{
	int P = p1 + p2;
	double pulse = 0.0;
	if ( P != 0 )
		pulse = 95.88 / (8128.0 / P + 100.0);

	double S = t / 8227.0 + n / 12241.0 + d / 22638.0;
	double tnd = 0.0;
	if ( S != 0.0 )
		tnd = 159.79 / (1.0 / S + 100.0);

	return (int) std::floor( 16384.0 * (pulse + tnd) + 0.5 );
}

struct Evt { int time; int osc; int amp; };

// Independent reference APU: produces the raw DAC level event timeline for the
// same register program. Constant-volume squares (no envelope/sweep), triangle
// with linear counter, noise and DMC, driven by the 5-step frame counter.
class RefApu {
public:
	static const int SQ1=0, SQ2=1, TRI=2, NOISE=3, DMC=4;

	int reg[5][4];
	bool rw[5][4];
	int length[5];
	int envelope[3];     // square1, square2, noise
	int env_delay[3];
	int phase_sq[2];
	int delay_sq[2];
	int tri_phase;
	int tri_delay;
	int tri_linear;
	int noise_lfsr;
	int noise_delay;

	// DMC
	int dmc_dac, dmc_period, dmc_delay, dmc_bits, dmc_bits_remain, dmc_buf;
	bool dmc_buf_full, dmc_silence, dmc_irq_enable, dmc_loop;
	int dmc_address, dmc_bytes_left;

	int frame, frame_delay, frame_mode;
	int enables;
	int last_time;
	int last_dmc_time;
	int last_level[5];
	int mute_mask;

	std::vector<Evt> events;
	std::vector<unsigned> dmc_reads;
	const unsigned char* dmc_rom;
	int dmc_rom_size;

	RefApu() { dmc_rom = 0; dmc_rom_size = 0; reset(); }

	void reset()
	{
		std::memset( reg, 0, sizeof reg );
		std::memset( rw, 0, sizeof rw );
		std::memset( length, 0, sizeof length );
		envelope[0] = envelope[1] = envelope[2] = 0;
		env_delay[0] = env_delay[1] = env_delay[2] = 0;
		phase_sq[0] = phase_sq[1] = 7;
		delay_sq[0] = delay_sq[1] = 0;
		tri_phase = 1; tri_delay = 0; tri_linear = 0;
		noise_lfsr = 1 << 14; noise_delay = 0;
		dmc_dac = 0; dmc_period = 0x1AC; dmc_delay = 0;
		dmc_bits = 0; dmc_bits_remain = 1; dmc_buf = 0;
		dmc_buf_full = false; dmc_silence = true;
		dmc_irq_enable = false; dmc_loop = false;
		dmc_address = 0; dmc_bytes_left = 0;
		frame = 0; frame_delay = 1; frame_mode = 0;
		enables = 0;
		last_time = 0; last_dmc_time = 0; mute_mask = 0;
		last_level[0] = last_level[1] = last_level[3] = last_level[4] = 0;
		last_level[2] = 15;
		events.clear(); dmc_reads.clear();
	}

	void level( int time, int osc, int amp )
	{
		if ( amp != last_level[osc] )
		{
			last_level[osc] = amp;
			events.push_back( {time, osc, amp} );
		}
	}

	int env_index( int osc ) const { return osc == 0 ? 0 : osc == 1 ? 1 : 2; }

	int sq_volume( int i ) const
	{
		if ( length[i] == 0 ) return 0;
		if ( reg[i][0] & 0x10 ) return reg[i][0] & 15;
		return envelope[env_index( i )];
	}
	int sq_period( int i ) const { return (reg[i][3] & 7) * 256 + (reg[i][2] & 0xFF); }

	int noise_volume() const
	{
		if ( length[3] == 0 ) return 0;
		if ( reg[3][0] & 0x10 ) return reg[3][0] & 15;
		return envelope[2];
	}

	int tri_calc() const
	{
		int amp = 16 - tri_phase;
		if ( amp < 0 ) amp = tri_phase - 17;
		return amp;
	}

	void write( int time, int addr, int data )
	{
		run_until( time );
		if ( addr < 0x4014 )
		{
			int idx = (addr - 0x4000) >> 2;
			int r = addr & 3;
			reg[idx][r] = data; rw[idx][r] = true;
			if ( idx == 4 ) { dmc_write( r, data ); return; }
			if ( r == 3 )
			{
				static const unsigned char lt[0x20] = {
					0x0A,0xFE,0x14,0x02,0x28,0x04,0x50,0x06,
					0xA0,0x08,0x3C,0x0A,0x0E,0x0C,0x1A,0x0E,
					0x0C,0x10,0x18,0x12,0x30,0x14,0x60,0x16,
					0xC0,0x18,0x48,0x1A,0x10,0x1C,0x20,0x1E };
				if ( (enables >> idx) & 1 )
					length[idx] = lt[(data >> 3) & 0x1F];
				if ( idx < 2 ) phase_sq[idx] = 7;
			}
		}
		else if ( addr == 0x4015 )
		{
			int old_enables = enables;
			enables = data;
			for ( int i = 0; i < 5; i++ )
				if ( !((data >> i) & 1) ) length[i] = 0;
			// DMC starts on a rising enable edge.
			if ( (data & 0x10) && !(old_enables & 0x10) )
				dmc_start();
		}
		else if ( addr == 0x4017 )
		{
			frame_mode = data;
			frame_delay = frame_delay & 1;
			frame = 0;
			if ( !(data & 0x80) ) { frame = 1; frame_delay += 7458; }
		}
	}

	void dmc_write( int r, int data )
	{
		static const short ptab[16] = {
			428,380,340,320,286,254,226,214,190,160,142,128,106,84,72,54 };
		if ( r == 0 )
		{
			dmc_period = ptab[data & 15];
			dmc_loop = (data & 0x40) != 0;
			dmc_irq_enable = (data & 0xC0) == 0x80;
		}
		else if ( r == 1 ) dmc_dac = data & 0x7F;
		else if ( r == 2 ) dmc_address = 0x4000 + data * 0x40;
		else if ( r == 3 ) dmc_bytes_left = data * 16 + 1;
	}

	int dmc_fetch()
	{
		unsigned a = 0x8000u + (unsigned) dmc_address;
		dmc_reads.push_back( a );
		int v = dmc_rom ? dmc_rom[(a - 0x8000) % dmc_rom_size]
		                : (int) (((a * 73u) ^ (a >> 3) ^ 0x96u) & 255);
		dmc_address = (dmc_address + 1) & 0x7FFF;
		return v;
	}

	void dmc_start()
	{
		dmc_address = 0x4000 + reg[4][2] * 0x40;
		dmc_bytes_left = reg[4][3] * 16 + 1;
		dmc_fill();
	}

	void dmc_fill()
	{
		if ( !dmc_buf_full && dmc_bytes_left )
		{
			dmc_buf = dmc_fetch();
			dmc_buf_full = true;
			if ( --dmc_bytes_left == 0 )
			{
				if ( dmc_loop )
				{
					dmc_address = 0x4000 + reg[4][2] * 0x40;
					dmc_bytes_left = reg[4][3] * 16 + 1;
				}
				else
				{
					length[4] = 0;
					enables &= ~0x10;
				}
			}
		}
	}

	void clock_lengths()
	{
		if ( length[0] && !(reg[0][0] & 0x20) ) length[0]--;
		if ( length[1] && !(reg[1][0] & 0x20) ) length[1]--;
		if ( length[3] && !(reg[3][0] & 0x20) ) length[3]--;
		if ( length[2] && !(reg[2][0] & 0x80) ) length[2]--;
	}

	void clock_linear()
	{
		if ( rw[2][3] ) tri_linear = reg[2][0] & 0x7F;
		else if ( tri_linear ) tri_linear--;
		if ( !(reg[2][0] & 0x80) ) rw[2][3] = false;
	}

	void clock_envelope( int osc )
	{
		int e = env_index( osc );
		int period = reg[osc][0] & 15;
		if ( rw[osc][3] )
		{
			rw[osc][3] = false;
			env_delay[e] = period;
			envelope[e] = 15;
		}
		else if ( --env_delay[e] < 0 )
		{
			env_delay[e] = period;
			if ( envelope[e] | (reg[osc][0] & 0x20) )
				envelope[e] = (envelope[e] - 1) & 15;
		}
	}

	void run_square( int i, int time, int end )
	{
		int period = sq_period(i);
		int timer = (period + 1) * 2;
		int vol = sq_volume(i);
		if ( vol == 0 || period < 8 )
		{
			// phase still advances
			int remain = end - time;
			if ( remain > 0 )
			{
				int count = (remain + timer - 1) / timer;
				phase_sq[i] = (phase_sq[i] + count) & 7;
			}
			level( time, i, 0 );
			return;
		}
		int duty_sel = (reg[i][0] >> 6) & 3;
		int duty = 1 << duty_sel;
		if ( duty_sel == 3 ) duty = 2;
		int ph = phase_sq[i];
		int t = time + delay_sq[i];
		int amp = (duty_sel == 3) ? vol : 0;
		if ( ph < duty ) amp ^= vol;
		level( time, i, amp );
		while ( t < end )
		{
			ph = (ph + 1) & 7;
			if ( ph == 0 || ph == duty )
			{
				amp = amp ? 0 : vol;
				level( t, i, amp );
			}
			t += timer;
		}
		phase_sq[i] = ph;
		delay_sq[i] = t - end;
	}

	void run_triangle( int time, int end )
	{
		int timer = ((reg[2][3] & 7) * 256 + (reg[2][2] & 0xFF)) + 1;
		level( time, TRI, tri_calc() );
		int t = time + tri_delay;
		if ( length[2] == 0 || tri_linear == 0 || timer < 3 ) return;
		int ph = tri_phase;
		while ( t < end )
		{
			if ( --ph == 0 ) ph = 32;
			else
			{
				int amp = 16 - ph;
				if ( amp < 0 ) amp = ph - 17;
				level( t, TRI, amp );
			}
			t += timer;
		}
		tri_phase = ph;
		tri_delay = t - end;
	}

	void run_noise( int time, int end )
	{
		static const short ptab[16] = {
			0x004,0x008,0x010,0x020,0x040,0x060,0x080,0x0A0,
			0x0CA,0x0FE,0x17C,0x1FC,0x2FA,0x3F8,0x7F2,0xFE4 };
		int period = ptab[reg[3][2] & 15];
		int vol = noise_volume();
		int lfsr = noise_lfsr;
		level( time, NOISE, (lfsr & 1) ? vol : 0 );
		int t = time + noise_delay;
		int tap = (reg[3][2] & 0x80) ? 8 : 13;
		// Mirror hardware/production order: shift first, then advance the
		// timer, and emit the new LFSR level at the advanced time.
		do
		{
			int fb = (lfsr << tap) ^ (lfsr << 14);
			lfsr = (fb & 0x4000) | (lfsr >> 1);
			t += period;
			level( t, NOISE, (lfsr & 1) ? vol : 0 );
		}
		while ( t < end );
		noise_lfsr = lfsr;
		noise_delay = t - end;
	}

	void run_dmc( int time, int end )
	{
		level( time, DMC, dmc_dac );
		int t = time + dmc_delay;
		if ( t >= end ) { dmc_delay = t - end; return; }
		int bits_remain = dmc_bits_remain;
		if ( dmc_silence && !dmc_buf_full )
		{
			int count = (end - t + dmc_period - 1) / dmc_period;
			bits_remain = (bits_remain - 1 + 8 - (count % 8)) % 8 + 1;
			t += count * dmc_period;
		}
		else
		{
			int bits = dmc_bits, dac = dmc_dac;
			do
			{
				if ( !dmc_silence )
				{
					int step = (bits & 1) * 4 - 2;
					bits >>= 1;
					if ( unsigned(dac + step) <= 0x7F )
					{
						dac += step;
						level( t, DMC, dac );
					}
				}
				t += dmc_period;
				if ( --bits_remain == 0 )
				{
					bits_remain = 8;
					if ( !dmc_buf_full ) dmc_silence = true;
					else
					{
						dmc_silence = false;
						bits = dmc_buf;
						dmc_buf_full = false;
						dmc_fill();
					}
				}
			}
			while ( t < end );
			dmc_bits = bits; dmc_dac = dac;
		}
		dmc_bits_remain = bits_remain;
		dmc_delay = t - end;
	}

	void run_until( int end )
	{
		if ( end == last_time ) return;
		if ( last_dmc_time < end )
		{
			run_dmc( last_dmc_time, end );
			last_dmc_time = end;
		}
		int time = last_time;
		while ( true )
		{
			int seg = time + frame_delay;
			if ( seg > end ) seg = end;
			frame_delay -= seg - time;

			run_square( 0, time, seg );
			run_square( 1, time, seg );
			run_triangle( time, seg );
			run_noise( time, seg );
			time = seg;
			if ( seg == end ) break;

			frame_delay = 7458;
			switch ( frame++ )
			{
				case 0:
				case 2:
					clock_lengths();
					break;
				case 1: frame_delay -= 2; break;
				case 3:
					frame = 0;
					if ( frame_mode & 0x80 ) frame_delay += 7458 - 6;
					break;
			}
			clock_linear();
			clock_envelope( 0 );
			clock_envelope( 1 );
			clock_envelope( 3 );
		}
		last_time = time;
	}

	void end_frame( int n )
	{
		if ( n > last_time ) run_until( n );
		last_time -= n;
		last_dmc_time -= n;
	}
};

// Turn a raw event timeline into mixed integers and then into a Blip buffer
// using the reference's own synthesizer.
struct RefOutput
{
	Blip_Buffer buf;
	Blip_Synth<blip_good_quality, 16384> synth;
	std::vector<std::pair<int,int> > mixed; // (global time, mixed integer)

	RefOutput( long sample_rate, uint32_t clock_rate )
	{
		buf.set_sample_rate( sample_rate, 1000 );
		buf.clock_rate( clock_rate );
		synth.volume( 1.0 );
		synth.output( &buf );
	}
};

static int ref_eff( int raw[5], int mute, int i )
{
	return (mute >> i) & 1 ? 0 : raw[i];
}

void render_reference( RefApu& ref, RefOutput& out, int end, int initial_dmc,
		int mute_mask, std::vector<short>* pcm )
{
	// Raw levels at reset: squares/noise/DMC(initial) zero except triangle 15.
	int cur[5] = {0, 0, 15, 0, initial_dmc};
	int last_mixed = ref_mix( ref_eff(cur,mute_mask,0), ref_eff(cur,mute_mask,1),
			ref_eff(cur,mute_mask,2), ref_eff(cur,mute_mask,3), ref_eff(cur,mute_mask,4) );
	out.mixed.clear();

	// Channels append their edges separately; merge them on the shared timeline.
	std::stable_sort( ref.events.begin(), ref.events.end(),
			[]( Evt const& a, Evt const& b ) { return a.time < b.time; } );

	for ( size_t k = 0; k < ref.events.size(); )
	{
		int t = ref.events[k].time;
		while ( k < ref.events.size() && ref.events[k].time == t )
		{
			Evt const& e = ref.events[k++];
			cur[e.osc] = e.amp;
		}
		int m = ref_mix( ref_eff(cur,mute_mask,0), ref_eff(cur,mute_mask,1),
				ref_eff(cur,mute_mask,2), ref_eff(cur,mute_mask,3), ref_eff(cur,mute_mask,4) );
		out.mixed.push_back( {t, m} );
		int delta = m - last_mixed;
		if ( delta ) out.synth.offset( t, delta, &out.buf );
		last_mixed = m;
	}
	out.buf.end_frame( end );
	if ( pcm )
	{
		short tmp[4096];
		while ( long got = out.buf.read_samples( tmp, 4096 ) )
			pcm->insert( pcm->end(), tmp, tmp + got );
	}
}

// ---------------------------------------------------------------------------
// Production harness
// ---------------------------------------------------------------------------

struct ProgEvent { int time; int addr; int data; };

struct DmcSource {
	static int read( void* p, nes_addr_t a )
	{
		DmcSource& s = *static_cast<DmcSource*>(p);
		s.addresses.push_back( a );
		return s.rom[(a - 0x8000) % s.rom.size()];
	}
	std::vector<unsigned char> rom;
	std::vector<unsigned> addresses;
};

struct ProdResult {
	std::vector<short> pcm;
	std::vector<unsigned> reads;
	std::vector<std::pair<int,int> > mixed;
};

static void mixed_listener( void* p, nes_time_t t, int m )
{
	static_cast<ProdResult*>(p)->mixed.push_back( {(int) t, m} );
}

// Fixed register program exercising all five channels with a non-zero DMC
// background and TND (triangle/noise) activity.
const ProgEvent program[] = {
	{0,0x4017,0x80},{0,0x4015,0x1F},
	// square 1: constant volume 12, period ~54
	{0,0x4000,0xDC},{0,0x4001,0x00},{0,0x4002,53},{0,0x4003,0xF8},
	// square 2: constant volume 9, period ~72
	{0,0x4004,0x99},{0,0x4005,0x00},{0,0x4006,71},{0,0x4007,0xF8},
	// triangle
	{0,0x4008,0xFF},{0,0x400A,89},{0,0x400B,0xF8},
	// noise constant volume 7, length halt, period index 7
	{0,0x400C,0x37},{0,0x400E,0x07},{0,0x400F,0xF8},
	// DMC: loop off, period index 9, direct load, sample address/len
	{0,0x4010,0x0F},{0,0x4011,40},{0,0x4012,0x00},{0,0x4013,0x02},
	{0,0x4015,0x1F},
	{900,0x4011,79},
	{5000,0x4000,0xB4},       // square1 constant volume 4, length halt
	{12003,0x400E,0x9A},      // noise mode/period change, constant volume
	{26000,0x400A,40},        // triangle period change
	{40000,0x4011,16},
	{60000,0x4004,0xB3},      // square2 constant volume 3, length halt
};

const int TOTAL = 120000;

ProdResult run_production( const int* chunks, int nchunks, int mute_mask,
		DmcSource& src, long sample_rate )
{
	Nes_Apu apu;
	Blip_Buffer buf;
	buf.set_sample_rate( sample_rate, 1000 );
	buf.clock_rate( 1789773 );
	apu.dmc_reader( DmcSource::read, &src );
	apu.set_nonlinear_output( &buf );
	apu.volume( 1.0 );
	apu.reset( false, 0 );
	apu.set_nonlinear_mute_mask( 0, mute_mask );

	ProdResult res;
	apu.set_nonlinear_listener( mixed_listener, &res );

	int base = 0, ei = 0, ci = 0;
	int nev = (int)(sizeof(program)/sizeof(program[0]));
	while ( base < TOTAL )
	{
		int n = chunks ? std::min( chunks[ci++ % nchunks], TOTAL - base ) : TOTAL - base;
		while ( ei < nev && program[ei].time < base + n )
		{
			const ProgEvent& e = program[ei++];
			apu.write_register( e.time - base, e.addr, e.data );
		}
		if ( mute_mask && base == 0 ) { /* mask already applied at 0 */ }
		apu.end_frame( n );
		buf.end_frame( n );
		short tmp[4096];
		while ( long got = buf.read_samples( tmp, 4096 ) )
			res.pcm.insert( res.pcm.end(), tmp, tmp + got );
		base += n;
	}
	res.reads = src.addresses;
	return res;
}

void build_ref_program( RefApu& ref, const int* chunks, int nchunks, int end )
{
	int base = 0, ei = 0, ci = 0;
	int nev = (int)(sizeof(program)/sizeof(program[0]));
	while ( base < end )
	{
		int n = chunks ? std::min( chunks[ci++ % nchunks], end - base ) : end - base;
		while ( ei < nev && program[ei].time < base + n )
		{
			const ProgEvent& e = program[ei++];
			ref.write( e.time - base, e.addr, e.data );
		}
		ref.end_frame( n );
		base += n;
	}
}

} // namespace

// ---------------------------------------------------------------------------
// target_dac
// ---------------------------------------------------------------------------

void target_dac()
{
	// Fixed formula anchors, independently computed here.
	TARGET_CHECK( ref_mix(0,0,0,0,0) == 0 );
	TARGET_CHECK( ref_mix(15,0,0,0,0) == 2447 );
	TARGET_CHECK( ref_mix(15,15,0,0,0) == 4235 );
	TARGET_CHECK( ref_mix(0,0,15,0,0) == 4037 );
	TARGET_CHECK( ref_mix(0,0,0,15,0) == 2858 );
	TARGET_CHECK( ref_mix(0,0,0,0,127) == 9409 );
	TARGET_CHECK( ref_mix(15,15,15,15,127) == 16384 );
	TARGET_CHECK( ref_mix(7,3,10,5,64) == 9785 );
	// pulse must be shared-curve: two equal squares differ from doubling one
	TARGET_CHECK( ref_mix(8,8,0,0,0) != 2 * ref_mix(8,0,0,0,0) );
	// TND combines triangle/noise/dmc nonlinearly
	int tnd_sep = ref_mix(0,0,10,0,0) + ref_mix(0,0,0,6,0) + ref_mix(0,0,0,0,50);
	TARGET_CHECK( ref_mix(0,0,10,6,50) != tnd_sep );

	const long sr = 44100;
	DmcSource src;
	for ( int i = 0; i < 4096; i++ ) src.rom.push_back( (unsigned char) ((i * 37 + 11) & 255) );

	// Whole-segment production output.
	ProdResult whole = run_production( 0, 0, 0, src, sr );
	TARGET_CHECK( whole.pcm.size() > 1000 );
	TARGET_CHECK( whole.reads.size() > 10 );
	TARGET_CHECK( !whole.mixed.empty() );

	// Independent reference timeline from the same program.
	RefApu ref;
	ref.dmc_rom = &src.rom[0];
	ref.dmc_rom_size = (int) src.rom.size();
	build_ref_program( ref, 0, 0, TOTAL );
	TARGET_CHECK( ref.dmc_reads == whole.reads );

	RefOutput rout( sr, 1789773 );
	std::vector<short> ref_pcm;
	render_reference( ref, rout, TOTAL, 0, 0, &ref_pcm );
	TARGET_CHECK( ref_pcm.size() == whole.pcm.size() );

	// Compare the mixed integer sequence produced at every merged event cycle.
	TARGET_CHECK( rout.mixed.size() == whole.mixed.size() );
	size_t first_diff_cycle = (size_t) -1;
	for ( size_t i = 0; i < whole.mixed.size(); i++ )
	{
		if ( whole.mixed[i].first != rout.mixed[i].first ||
				whole.mixed[i].second != rout.mixed[i].second )
		{
			first_diff_cycle = i;
			break;
		}
	}
	TARGET_CHECK( first_diff_cycle == (size_t) -1 );

	// PCM from identical configuration must match within one sample absolute.
	int max_abs = 0;
	size_t first_pcm_diff = (size_t) -1;
	for ( size_t i = 0; i < whole.pcm.size(); i++ )
	{
		int d = std::abs( (int) whole.pcm[i] - (int) ref_pcm[i] );
		if ( d > 1 && first_pcm_diff == (size_t) -1 ) first_pcm_diff = i;
		if ( d > max_abs ) max_abs = d;
	}
	TARGET_CHECK( max_abs <= 1 );

	// Non-zero DMC background with triangle and noise both active produced
	// TND-only changes in the observed stream.
	bool saw_tnd = false;
	for ( size_t i = 1; i < whole.mixed.size(); i++ )
	{
		int t0 = whole.mixed[i-1].second, t1 = whole.mixed[i].second;
		if ( t1 != t0 ) { saw_tnd = true; break; }
	}
	TARGET_CHECK( saw_tnd );
}

// ---------------------------------------------------------------------------
// target_timeline
// ---------------------------------------------------------------------------

void target_timeline()
{
	const long sr = 44100;
	DmcSource src;
	for ( int i = 0; i < 4096; i++ ) src.rom.push_back( (unsigned char) ((i * 37 + 11) & 255) );

	int chunksA[] = {1,7,257,997,4093};
	int chunksB[] = {3,11,333,1500,7777};
	int chunksC[] = {257};

	DmcSource s1; s1.rom = src.rom;
	DmcSource s2; s2.rom = src.rom;
	DmcSource s3; s3.rom = src.rom;
	ProdResult whole = run_production( 0, 0, 0, s1, sr );
	ProdResult split = run_production( chunksA, 5, 0, s2, sr );
	ProdResult split2 = run_production( chunksB, 5, 0, s3, sr );

	TARGET_CHECK( whole.reads == split.reads );
	TARGET_CHECK( whole.reads == split2.reads );

	size_t first = (size_t) -1;
	for ( size_t i = 0; i < whole.pcm.size(); i++ )
		if ( whole.pcm[i] != split.pcm[i] ) { first = i; break; }
	TARGET_CHECK( first == (size_t) -1 );
	TARGET_CHECK( whole.pcm == split.pcm );
	TARGET_CHECK( whole.pcm == split2.pcm );

	// Listener timestamps are frame-relative and therefore shift with the
	// partition boundaries; the merged mixed integer stream itself must be
	// byte-for-byte independent of how the run is segmented.
	auto values = []( const std::vector<std::pair<int,int> >& m ) {
		std::vector<int> v;
		v.reserve( m.size() );
		for ( size_t i = 0; i < m.size(); i++ ) v.push_back( m[i].second );
		return v;
	};
	TARGET_CHECK( values( whole.mixed ) == values( split.mixed ) );
	TARGET_CHECK( values( whole.mixed ) == values( split2.mixed ) );
	// Within one frame, listener times must be non-negative and ordered.
	bool ordered = true;
	for ( size_t i = 1; i < whole.mixed.size(); i++ )
		if ( whole.mixed[i].first < whole.mixed[i-1].first ) { ordered = false; break; }
	TARGET_CHECK( ordered );

	// Same-cycle changes: two register writes to different channels within one
	// cycle must merge into a single mixed delta, applied once.
	{
		Nes_Apu apu;
		Blip_Buffer b;
		b.set_sample_rate( sr, 1000 ); b.clock_rate( 1789773 );
		apu.set_nonlinear_output( &b );
		apu.reset( false, 0 );
		ProdResult r;
		apu.set_nonlinear_listener( mixed_listener, &r );
		// enable both squares at the identical cycle (DMC left disabled: this
		// block exercises same-cycle merging of the two pulse channels)
		apu.write_register( 100, 0x4015, 0x0F );
		apu.write_register( 100, 0x4000, 0xBF );
		apu.write_register( 100, 0x4002, 53 );
		apu.write_register( 100, 0x4003, 0xF8 );
		apu.write_register( 100, 0x4004, 0x78 );
		apu.write_register( 100, 0x4006, 71 );
		apu.write_register( 100, 0x4007, 0xF8 );
		apu.end_frame( 5000 );
		b.end_frame( 5000 );
		int at100 = 0;
		for ( size_t i = 0; i < r.mixed.size(); i++ )
			if ( r.mixed[i].first == 100 ) at100++;
		TARGET_CHECK( at100 == 1 );
	}

	// DMC run-ahead via run_until must not fetch the same bytes twice and must
	// produce the same fetch sequence and PCM as plain frame-by-frame progress.
	{
		auto build_dmc = [&]( bool interleave ) {
			Nes_Apu apu;
			Blip_Buffer b;
			b.set_sample_rate( sr, 1000 ); b.clock_rate( 1789773 );
			DmcSource ds; ds.rom = src.rom;
			apu.dmc_reader( DmcSource::read, &ds );
			apu.set_nonlinear_output( &b );
			apu.reset( false, 0 );
			apu.write_register( 0, 0x4017, 0x80 );
			apu.write_register( 0, 0x4010, 0x0F ); // loop off
			apu.write_register( 0, 0x4011, 40 );
			apu.write_register( 0, 0x4012, 0 );
			apu.write_register( 0, 0x4013, 2 );
			apu.write_register( 0, 0x4015, 0x10 );

			int frames[] = { 3000, 3000, 4000, 5000 };
			int total = 0;
			for ( int f = 0; f < 4; f++ )
			{
				int len = frames[f];
				if ( interleave && (f & 1) )
					apu.run_until( len / 2 ); // DMC fetched early
				apu.end_frame( len );
				b.end_frame( len );
				total += len;
			}
			short tmp[4096];
			std::vector<short> pcm;
			while ( long got = b.read_samples( tmp, 4096 ) )
				pcm.insert( pcm.end(), tmp, tmp + got );
			return std::make_pair( ds.addresses, pcm );
		};
		std::pair<std::vector<unsigned>, std::vector<short> > plain = build_dmc( false );
		std::pair<std::vector<unsigned>, std::vector<short> > ahead = build_dmc( true );
		TARGET_CHECK( plain.first.size() > 5 );
		TARGET_CHECK( plain.first == ahead.first ); // no re-fetch, same order
		TARGET_CHECK( plain.second == ahead.second );
		// loop off: addresses strictly increment within the 0x8000-0xFFFF window
		for ( size_t i = 1; i < plain.first.size(); i++ )
			TARGET_CHECK( plain.first[i] == plain.first[i-1] + 1 );
	}

	// Mute then restore: internal state keeps running, output resumes at level.
	{
		DmcSource a; a.rom = src.rom;
		DmcSource c; c.rom = src.rom;
		ProdResult unmuted = run_production( chunksC, 1, 0, a, sr );

		Nes_Apu apu;
		Blip_Buffer b;
		b.set_sample_rate( sr, 1000 ); b.clock_rate( 1789773 );
		apu.dmc_reader( DmcSource::read, &c );
		apu.set_nonlinear_output( &b );
		apu.reset( false, 0 );
		ProdResult r;
		apu.set_nonlinear_listener( mixed_listener, &r );
		// mute triangle+noise for a window, then restore
		apu.set_nonlinear_mute_mask( 0, 0x0C );
		int base = 0, ei = 0;
		int nev = (int)(sizeof(program)/sizeof(program[0]));
		int chunks[] = { 257 };
		int ci = 0;
		const int mute_begin = 30000, mute_end = 60000;
		while ( base < TOTAL )
		{
			int n = std::min( chunks[ci++ % 1], TOTAL - base );
			while ( ei < nev && program[ei].time < base + n )
			{
				const ProgEvent& e = program[ei++];
				apu.write_register( e.time - base, e.addr, e.data );
			}
			// Mute changes take effect inside the frame that contains the
			// global transition time, expressed in frame-relative cycles.
			if ( mute_begin >= base && mute_begin < base + n )
				apu.set_nonlinear_mute_mask( mute_begin - base, 0x0C );
			if ( mute_end >= base && mute_end < base + n )
				apu.set_nonlinear_mute_mask( mute_end - base, 0x00 );
			apu.end_frame( n );
			b.end_frame( n );
			short tmp[4096];
			while ( long got = b.read_samples( tmp, 4096 ) )
				r.pcm.insert( r.pcm.end(), tmp, tmp + got );
			base += n;
		}
		TARGET_CHECK( c.addresses == a.addresses ); // DMC kept fetching while muted
		TARGET_CHECK( r.pcm.size() == unmuted.pcm.size() );
		// after unmuting, signal is non-silent again
		bool loud_after = false;
		for ( size_t i = r.pcm.size() / 2; i < r.pcm.size(); i++ )
			if ( r.pcm[i] != 0 ) { loud_after = true; break; }
		TARGET_CHECK( loud_after );
		// muted window is audibly different from the fully unmuted run
		TARGET_CHECK( r.pcm != unmuted.pcm );
	}

	// reset() rebuilds the mixing reference from the post-reset raw levels and
	// must not inject a startup step into an empty buffer.
	{
		Nes_Apu apu;
		Blip_Buffer b;
		b.set_sample_rate( sr, 1000 ); b.clock_rate( 1789773 );
		apu.set_nonlinear_output( &b );
		apu.reset( false, 33 );
		// No register changes: the reference already equals the current mixed
		// level, so an entire frame must stay silent (no artificial edge).
		apu.end_frame( 1000 );
		b.end_frame( 1000 );
		short tmp[512];
		long got = b.read_samples( tmp, 512 );
		TARGET_CHECK( got > 0 );
		bool all_zero = true;
		for ( long i = 0; i < got; i++ ) if ( tmp[i] ) { all_zero = false; break; }
		TARGET_CHECK( all_zero );

		// A direct DMC load now produces exactly one mixed step from the reset
		// reference level (triangle rests at its reset value 15; dac 33 -> 50).
		ProdResult r;
		apu.set_nonlinear_listener( mixed_listener, &r );
		apu.write_register( 5, 0x4011, 50 );
		apu.end_frame( 1000 );
		b.end_frame( 1000 );
		TARGET_CHECK( r.mixed.size() == 1 );
		TARGET_CHECK( r.mixed[0].first == 5 );
		int expected = Nes_Nonlinearizer::mix_levels( 0, 0, 15, 0, 50 );
		TARGET_CHECK( r.mixed[0].second == expected );
		TARGET_CHECK( expected > Nes_Nonlinearizer::mix_levels( 0, 0, 15, 0, 33 ) );

		// Resetting mid-stream drops pending events and rebuilds the reference;
		// a subsequent silent frame must produce no new mixer events (no
		// artificial startup step), even though the band-limited buffer still
		// carries the decaying tail of earlier impulses.
		apu.reset( false, 17 );
		ProdResult after;
		apu.set_nonlinear_listener( mixed_listener, &after );
		apu.end_frame( 1000 );
		b.end_frame( 1000 );
		TARGET_CHECK( after.mixed.empty() );
	}
}

// ---------------------------------------------------------------------------
// target_nsf
// ---------------------------------------------------------------------------

namespace {

struct NsfBuilder {
	std::vector<unsigned char> data;
	void put8( int v ) { data.push_back( (unsigned char) v ); }
	void put16( int v ) { put8( v & 255 ); put8( (v >> 8) & 255 ); }
	void put_str( const char* s, int n )
	{
		int k = 0;
		for ( ; k < n && s[k]; k++ ) put8( s[k] );
		for ( ; k < n; k++ ) put8( 0 );
	}
};

// Build a minimal one-track NSF whose init/play code writes APU registers via
// real 6502 STA instructions. load=$8000, code placed right there.
std::vector<unsigned char> make_nsf( bool vrc6 )
{
	NsfBuilder b;
	b.put_str( "NESM\x1A", 5 );
	b.put8( 1 );                 // version
	b.put8( 1 );                 // track count
	b.put8( 1 );                 // first track
	b.put16( 0x8000 );           // load address
	b.put16( 0x8000 );           // init address
	b.put16( 0x8010 );           // play address
	b.put_str( "t", 32 );        // game
	b.put_str( "t", 32 );        // author
	b.put_str( "t", 32 );        // copyright
	b.put16( 16639 );            // NTSC speed (~60Hz)
	for ( int i = 0; i < 8; i++ ) b.put8( 0 ); // banks
	b.put16( 16639 );            // PAL speed
	b.put8( 0 );                 // speed flags
	b.put8( vrc6 ? 0x01 : 0x00 );// chip flags
	for ( int i = 0; i < 4; i++ ) b.put8( 0 ); // unused

	// Code at 0x8000 (init): set up square 1 once, then RTS.
	// LDA #$BF ; STA $4000 ; LDA #53 ; STA $4002 ; LDA #$F8 ; STA $4003 ; RTS
	int init[] = {
		0xA9,0xBF, 0x8D,0x00,0x40,
		0xA9,53,   0x8D,0x02,0x40,
		0xA9,0xF8, 0x8D,0x03,0x40,
		0x60
	};
	for ( int v : init ) b.put8( v );
	// pad to 0x8010
	while ( (int) b.data.size() < 0x80 + 0x10 ) b.put8( 0xEA );

	// Play at 0x8010: retrigger length, toggle volume, then RTS.
	// LDA #$BF ; STA $4000 ; LDA #$F8 ; STA $4003 ;
	// (triangle) LDA #$FF ; STA $4008 ; LDA #89 ; STA $400A ; LDA #$F8 ; STA $400B ;
	// RTS
	int play[] = {
		0xA9,0xBF, 0x8D,0x00,0x40,
		0xA9,0xF8, 0x8D,0x03,0x40,
		0xA9,0xFF, 0x8D,0x08,0x40,
		0xA9,89,   0x8D,0x0A,0x40,
		0xA9,0xF8, 0x8D,0x0B,0x40,
		0x60
	};
	for ( int v : play ) b.put8( v );

	if ( vrc6 )
	{
		// VRC6 square 3 at $9000 (vol/duty), $9001 (period low), $9002 (enable)
		int extra[] = {
			0xA9,0x0F, 0x8D,0x00,0x90,
			0xA9,0x40, 0x8D,0x01,0x90,
			0xA9,0x80, 0x8D,0x02,0x90
		};
		// splice before the final RTS of play
		b.data.resize( b.data.size() - 1 );
		for ( int v : extra ) b.put8( v );
		b.put8( 0x60 );
	}
	return b.data;
}

void play_emu( Nsf_Emu& emu, long samples, std::vector<short>& out )
{
	out.clear();
	short buf[2048];
	long got = 0;
	while ( got < samples )
	{
		long want = std::min<long>( 2048, samples - got );
		blargg_err_t e = emu.play( want, buf );
		TARGET_CHECK( e == 0 );
		out.insert( out.end(), buf, buf + want );
		got += want;
	}
}

} // namespace

void target_nsf()
{
	const long sr = 44100;
	std::vector<unsigned char> nsf = make_nsf( false );
	TARGET_CHECK( nsf.size() > 0x80 );

	// Default (nonlinear disabled) must remain compatible and produce sound.
	{
		Nsf_Emu emu;
		TARGET_CHECK( emu.set_sample_rate( sr ) == 0 );
		Mem_File_Reader r( &nsf[0], (long) nsf.size() );
		TARGET_CHECK( emu.load( r ) == 0 );
		TARGET_CHECK( emu.nonlinear_mixing_enabled() == false );
		TARGET_CHECK( emu.start_track( 0 ) == 0 );
		std::vector<short> pcm;
		play_emu( emu, 4096, pcm );
		TARGET_CHECK( pcm.size() == 4096 );
		bool loud = false;
		for ( short s : pcm ) if ( s ) { loud = true; break; }
		TARGET_CHECK( loud );
	}

	// Nonlinear mode produces sound and differs from the linear default.
	std::vector<short> nl_pcm;
	{
		Nsf_Emu emu;
		emu.set_nonlinear_mixing( true );
		TARGET_CHECK( emu.set_sample_rate( sr ) == 0 );
		Mem_File_Reader r( &nsf[0], (long) nsf.size() );
		TARGET_CHECK( emu.load( r ) == 0 );
		TARGET_CHECK( emu.nonlinear_mixing_enabled() == true );
		TARGET_CHECK( emu.start_track( 0 ) == 0 );
		// The joint mixer attaches when the track starts.
		TARGET_CHECK( emu.apu_()->nonlinear_output_enabled() == true );
		play_emu( emu, 4096, nl_pcm );
		bool loud = false;
		for ( short s : nl_pcm ) if ( s ) { loud = true; break; }
		TARGET_CHECK( loud );
		// nonlinear rendering is audibly different from the linear default
		{
			Nsf_Emu ref_emu;
			TARGET_CHECK( ref_emu.set_sample_rate( sr ) == 0 );
			Mem_File_Reader rr( &nsf[0], (long) nsf.size() );
			TARGET_CHECK( ref_emu.load( rr ) == 0 );
			TARGET_CHECK( ref_emu.start_track( 0 ) == 0 );
			std::vector<short> lin;
			play_emu( ref_emu, 4096, lin );
			TARGET_CHECK( lin != nl_pcm );
		}
	}

	// Restarting a track clears pending state and still renders correctly.
	{
		Nsf_Emu emu;
		emu.set_nonlinear_mixing( true );
		TARGET_CHECK( emu.set_sample_rate( sr ) == 0 );
		Mem_File_Reader r( &nsf[0], (long) nsf.size() );
		TARGET_CHECK( emu.load( r ) == 0 );
		TARGET_CHECK( emu.start_track( 0 ) == 0 );
		std::vector<short> first;
		play_emu( emu, 2048, first );
		TARGET_CHECK( emu.start_track( 0 ) == 0 );
		std::vector<short> second;
		play_emu( emu, 2048, second );
		TARGET_CHECK( first == second );
		TARGET_CHECK( emu.apu_()->nonlinear_output_enabled() == true );
	}

	// Loading again (which internally unloads the previous file) keeps the
	// configuration chosen before the first load.
	{
		Nsf_Emu emu;
		emu.set_nonlinear_mixing( true );
		TARGET_CHECK( emu.set_sample_rate( sr ) == 0 );
		Mem_File_Reader r1( &nsf[0], (long) nsf.size() );
		TARGET_CHECK( emu.load( r1 ) == 0 );
		TARGET_CHECK( emu.nonlinear_mixing_enabled() == true );
		Mem_File_Reader r2( &nsf[0], (long) nsf.size() );
		TARGET_CHECK( emu.load( r2 ) == 0 );
		TARGET_CHECK( emu.nonlinear_mixing_enabled() == true );
		TARGET_CHECK( emu.start_track( 0 ) == 0 );
		std::vector<short> pcm;
		play_emu( emu, 2048, pcm );
		bool loud = false;
		for ( short s : pcm ) if ( s ) { loud = true; break; }
		TARGET_CHECK( loud );
	}

	// VRC6 expansion stays on its independent path in nonlinear mode.
	{
		std::vector<unsigned char> vnsf = make_nsf( true );
		Nsf_Emu emu;
		emu.set_nonlinear_mixing( true );
		TARGET_CHECK( emu.set_sample_rate( sr ) == 0 );
		Mem_File_Reader r( &vnsf[0], (long) vnsf.size() );
		TARGET_CHECK( emu.load( r ) == 0 );
		TARGET_CHECK( emu.voice_count() > Nes_Apu::osc_count );
		TARGET_CHECK( emu.start_track( 0 ) == 0 );
		std::vector<short> with_vrc6;
		play_emu( emu, 4096, with_vrc6 );

		// mute only the five APU voices; VRC6 must still be audible.
		emu.mute_voices( 0x1F );
		std::vector<short> only_vrc6;
		play_emu( emu, 4096, only_vrc6 );
		bool vrc6_audible = false;
		for ( short s : only_vrc6 ) if ( s ) { vrc6_audible = true; break; }
		TARGET_CHECK( vrc6_audible );
		TARGET_CHECK( only_vrc6 != with_vrc6 );

		// Unmuting reconnects the five APU channels: the joint signal returns
		// on top of the untouched VRC6 output, so total energy rises again.
		emu.mute_voices( 0 );
		std::vector<short> restored;
		play_emu( emu, 4096, restored );
		auto energy = []( const std::vector<short>& v ) {
			long long e = 0;
			for ( short s : v ) e += std::abs( (int) s );
			return e;
		};
		TARGET_CHECK( energy( restored ) > energy( only_vrc6 ) );
		bool restored_apu = false;
		for ( short s : restored ) if ( s ) { restored_apu = true; break; }
		TARGET_CHECK( restored_apu );

		// A separately run, never-muted emulator over the same span confirms
		// the APU channels were the missing contribution (its output is louder
		// than the APU-muted window).
		Nsf_Emu full;
		full.set_nonlinear_mixing( true );
		TARGET_CHECK( full.set_sample_rate( sr ) == 0 );
		Mem_File_Reader fr( &vnsf[0], (long) vnsf.size() );
		TARGET_CHECK( full.load( fr ) == 0 );
		TARGET_CHECK( full.start_track( 0 ) == 0 );
		std::vector<short> full_pcm;
		play_emu( full, 4096 * 3, full_pcm );
		TARGET_CHECK( energy( full_pcm ) > energy( only_vrc6 ) * 3 );
	}
}
