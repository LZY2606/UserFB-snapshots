// Task tests for NSF2 IRQ playback support.
//
// Registers "timer", "cpu" and "audio" cases with task_support/check.h.
// No main() here: task_support/driver.c provides it.
//
// Most observations go through the public gme C interface and rendered PCM.
// A few exact register/stack checks use a test-only access hack to inspect
// the Nsf_Emu directly; this does not affect the production build.

#include "check.h"
#include "gme.h"
#include "fixture.h"

#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#define protected public
#define private public
#include "Nsf_Emu.h"
#undef private
#undef protected

typedef std::vector<unsigned char> Bytes;
typedef std::vector<short> Pcm;

static Music_Emu* open_emu( Bytes const& b, gme_err_t* err_out = 0 )
{
	Music_Emu* e = 0;
	gme_err_t err = gme_open_data( &b [0], (long) b.size(), &e, 44100 );
	if ( err_out )
		*err_out = err;
	if ( err )
		return 0;
	gme_ignore_silence( e, 1 );
	return e;
}

static Nsf_Emu* nsf( Music_Emu* e ) { return static_cast<Nsf_Emu*>( e ); }

static Pcm render( Bytes const& b, int shorts = 44100, bool split = false )
{
	Music_Emu* e = open_emu( b );
	if ( !e )
		return Pcm();
	if ( gme_start_track( e, 0 ) )
	{
		gme_delete( e );
		return Pcm();
	}
	Pcm p( shorts );
	size_t pos = 0;
	int i = 0;
	static int const pat [] = { 2, 14, 514 }; // 1/7/257 stereo frames
	while ( pos < p.size() )
	{
		size_t n = split ? size_t( pat [i++ % 3] ) : p.size();
		if ( n > p.size() - pos )
			n = p.size() - pos;
		if ( gme_play( e, (int) n, &p [pos] ) )
		{
			p.clear();
			break;
		}
		pos += n;
	}
	gme_delete( e );
	return p;
}

static long count_nonzero( Pcm const& p )
{
	long n = 0;
	for ( size_t i = 0; i < p.size(); ++i )
		if ( p [i] )
			++n;
	return n;
}

// Custom NSF2 image with hand-written INIT/IRQ/PLAY routines at $8000/$8100/$8080.
static Bytes make_nsf( Bytes const& init, Bytes const& irq, Bytes const& play,
		unsigned char features )
{
	Bytes b( 128 + 8192, 0xEA );
	std::memset( &b [0], 0, 128 );
	std::memcpy( &b [0], "NESM\x1A", 5 );
	b [5] = 2;
	b [6] = 1;
	b [7] = 1;
	b [9] = 0x80;  // load $8000
	b [11] = 0x80; // init $8000
	b [12] = 0x80;
	b [13] = 0x80; // play $8080
	b [124] = features;
	if ( init.size() )
		std::memcpy( &b [128], &init [0], init.size() );
	if ( play.size() )
		std::memcpy( &b [128 + 0x80], &play [0], play.size() );
	else
		b [128 + 0x80] = 0x60; // RTS
	if ( irq.size() )
		std::memcpy( &b [128 + 0x100], &irq [0], irq.size() );
	return b;
}

// ---------------------------------------------------------------------------
// timer group
// ---------------------------------------------------------------------------

static int case_header_validation()
{
	// accepted feature masks
	unsigned char const good [] = { 0x00, 0x10, 0x40, 0x50 };
	for ( unsigned i = 0; i < sizeof good; ++i )
	{
		Bytes b = nsf_fixture( true );
		b [124] = good [i];
		Music_Emu* e = 0;
		TASK_CHECK( gme_open_data( &b [0], (long) b.size(), &e, 44100 ) == 0 );
		if ( e )
			gme_delete( e );
	}

	// bit5, bit7, low reserved bits and any non-zero length byte are errors
	unsigned char bad_feat [] = { 0x20, 0x70, 0x80, 0xD0, 0x01, 0x0F, 0x52, 0x54, 0x58 };
	for ( unsigned i = 0; i < sizeof bad_feat; ++i )
	{
		Bytes b = nsf_fixture( true );
		b [124] = bad_feat [i];
		Music_Emu* e = 0;
		TASK_CHECK( gme_open_data( &b [0], (long) b.size(), &e, 44100 ) != 0 );
	}
	unsigned length_off [] = { 125, 126, 127 };
	for ( unsigned i = 0; i < 3; ++i )
	{
		Bytes b = nsf_fixture( true );
		b [length_off [i]] = 0x12;
		Music_Emu* e = 0;
		TASK_CHECK( gme_open_data( &b [0], (long) b.size(), &e, 44100 ) != 0 );
	}

	// expansion sound is outside the supported NSF2 subset
	{
		Bytes b = nsf_fixture( true );
		b [123] = 0x01; // chip_flags
		Music_Emu* e = 0;
		TASK_CHECK( gme_open_data( &b [0], (long) b.size(), &e, 44100 ) != 0 );
	}

	// NSF1 ignores the feature/length bytes entirely
	{
		Bytes b = nsf_fixture( false );
		b [124] = 0xFF;
		b [125] = 0xAB;
		b [126] = 0xCD;
		b [127] = 0xEF;
		Music_Emu* e = 0;
		TASK_CHECK( gme_open_data( &b [0], (long) b.size(), &e, 44100 ) == 0 );
		if ( e )
			gme_delete( e );
	}
	return 0;
}

static int case_timer_registers()
{
	Bytes b = nsf_fixture( true );
	Music_Emu* me = open_emu( b );
	TASK_CHECK( me != 0 );
	Nsf_Emu* e = nsf( me );
	TASK_CHECK( gme_start_track( me, 0 ) == 0 );
	TASK_CHECK( e->nsf2_irq_supported() );
	TASK_CHECK( e->nsf2_play_disabled() );
	TASK_CHECK( (e->r.status & Nes_Cpu::irq_inhibit) != 0 ); // first INIT: I set
	TASK_CHECK( !e->nsf2_timer_running );

	e->set_time( 100 );

	// $401B/$401C read/write the 16-bit reload
	e->cpu_write( 0x401B, 0x34 );
	e->cpu_write( 0x401C, 0x12 );
	TASK_CHECK( e->cpu_read( 0x401B ) == 0x34 );
	TASK_CHECK( e->cpu_read( 0x401C ) == 0x12 );
	TASK_CHECK( e->nsf2_timer_reload == 0x1234 );

	// stopped: $401D reads neither flag nor run bit
	TASK_CHECK( (e->cpu_read( 0x401D ) & 0x81) == 0x00 );

	// period is reload+1 from the enable write
	e->cpu_write( 0x401D, 0x01 );
	TASK_CHECK( e->nsf2_timer_running );
	nes_time_t first = 100 + 0x1234 + 1;
	TASK_CHECK( e->nsf2_timer_deadline == first );

	// changing reload while running does not move current countdown
	e->cpu_write( 0x401B, 0x00 );
	e->cpu_write( 0x401C, 0x00 );
	TASK_CHECK( e->nsf2_timer_deadline == first );
	TASK_CHECK( e->cpu_read( 0x401B ) == 0 );
	TASK_CHECK( e->cpu_read( 0x401C ) == 0 );

	// rewriting $01 while already running also leaves countdown untouched
	e->cpu_write( 0x401D, 0x01 );
	TASK_CHECK( e->nsf2_timer_deadline == first );

	// no flag one cycle before the deadline
	e->set_time( first - 1 );
	TASK_CHECK( (e->cpu_read( 0x401D ) & 0x80) == 0 );

	// at deadline read returns pre-clear bit7 + running bit0, clears flag only
	e->set_time( first );
	int st = e->cpu_read( 0x401D );
	TASK_CHECK( (st & 0x81) == 0x81 );
	TASK_CHECK( !e->nsf2_timer_flag );
	TASK_CHECK( (e->cpu_read( 0x401D ) & 0x80) == 0 );

	// disabling keeps a pending unacknowledged flag
	e->nsf2_timer_flag = true;
	e->cpu_write( 0x401D, 0x00 );
	TASK_CHECK( !e->nsf2_timer_running );
	TASK_CHECK( e->nsf2_timer_flag );
	int st2 = e->cpu_read( 0x401D );
	TASK_CHECK( (st2 & 0x81) == 0x80 ); // bit7 set, bit0 clear

	gme_delete( me );
	return 0;
}

static int case_apu_overlap()
{
	Bytes b = nsf_fixture( true );
	Music_Emu* me = open_emu( b );
	TASK_CHECK( me != 0 );
	Nsf_Emu* e = nsf( me );
	TASK_CHECK( gme_start_track( me, 0 ) == 0 );

	e->r.status &= ~Nes_Cpu::irq_inhibit;
	e->set_time( 5000 );

	// both sources asserted: line requests
	e->nsf2_timer_flag = true;
	e->apu.irq_flag = true;
	e->apu.earliest_irq_ = 0;
	TASK_CHECK( e->cpu_done() == 4 );

	// reading the timer acks the timer only
	TASK_CHECK( e->cpu_read( 0x401D ) & 0x80 );
	TASK_CHECK( !e->nsf2_timer_flag );
	TASK_CHECK( e->apu.irq_flag );
	TASK_CHECK( e->cpu_done() == 4 ); // APU still requests

	// reading $4015 acknowledges the APU frame request
	int status = e->cpu_read( Nes_Apu::status_addr );
	(void) status;
	TASK_CHECK( !e->apu.irq_flag );
	TASK_CHECK( e->cpu_done() == -1 );

	// SEI masks the combined line
	e->nsf2_timer_flag = true;
	e->r.status |= Nes_Cpu::irq_inhibit;
	TASK_CHECK( e->cpu_done() == -1 );
	e->r.status &= ~Nes_Cpu::irq_inhibit;
	TASK_CHECK( e->cpu_done() == 4 );

	gme_delete( me );
	return 0;
}

// ---------------------------------------------------------------------------
// cpu group
// ---------------------------------------------------------------------------

// IRQ handler records the stacked status/PC and counts, acks timer, toggles DAC.
static Bytes stack_irq_code()
{
	return Bytes {
		0xBA,             // TSX
		0xBD,0x01,0x01,   // LDA $0101,X  stacked SR
		0x85,0x20,        // STA $20
		0xBD,0x02,0x01,   // LDA $0102,X  stacked PCL
		0x85,0x23,
		0xBD,0x03,0x01,   // LDA $0103,X  stacked PCH
		0x85,0x24,
		0xE6,0x21,        // INC $21
		0xAD,0x1D,0x40,   // LDA $401D (ack timer)
		0xA5,0x22,0x49,0x40,0x85,0x22,0x8D,0x11,0x40, // toggle DAC
		0x40              // RTI
	};
}

static Bytes stack_init_code()
{
	return Bytes {
		0xA9,0x40,0x8D,0x17,0x40,       // inhibit APU frame IRQ
		0xA9,0x00,0x8D,0xFE,0xFF,
		0xA9,0x81,0x8D,0xFF,0xFF,       // IRQ vector $8100
		0xA9,0xD0,0x8D,0x1B,0x40,
		0xA9,0x07,0x8D,0x1C,0x40,       // reload $07D0
		0xA9,0x01,0x8D,0x1D,0x40,       // timer on
		0x58,                           // CLI
		0x4C,0x1F,0x80                  // JMP $801F (self)
	};
}

static int case_irq_stack_b_flag()
{
	Bytes b = make_nsf( stack_init_code(), stack_irq_code(), Bytes(), 0x50 );
	Music_Emu* me = open_emu( b );
	TASK_CHECK( me != 0 );
	Nsf_Emu* e = nsf( me );
	TASK_CHECK( gme_start_track( me, 0 ) == 0 );

	Pcm pcm( 44100 );
	TASK_CHECK( gme_play( me, (int) pcm.size(), &pcm [0] ) == 0 );

	unsigned sr  = e->low_mem [0x20];
	unsigned cnt = e->low_mem [0x21];
	unsigned pcl = e->low_mem [0x23];
	unsigned pch = e->low_mem [0x24];

	TASK_CHECK( cnt > 50 );          // many interrupts serviced
	TASK_CHECK( (sr & 0x10) == 0 );  // B pushed clear for IRQ
	TASK_CHECK( (sr & 0x20) != 0 );  // reserved bit pushed set
	TASK_CHECK( (sr & 0x04) == 0 );  // interrupted code had I clear
	TASK_CHECK( pch == 0x80 );       // returned into main loop page
	TASK_CHECK( pcl == 0x1F );
	TASK_CHECK( count_nonzero( pcm ) > 1000 ); // DAC toggles audible

	gme_delete( me );
	return 0;
}

static int case_play_and_irq_nesting()
{
	Bytes play { 0xE6,0x30, 0x60 }; // INC $30; RTS
	Bytes b = make_nsf( stack_init_code(), stack_irq_code(), play, 0x10 );
	Music_Emu* me = open_emu( b );
	TASK_CHECK( me != 0 );
	Nsf_Emu* e = nsf( me );
	TASK_CHECK( gme_start_track( me, 0 ) == 0 );

	Pcm pcm( 44100 );
	TASK_CHECK( gme_play( me, (int) pcm.size(), &pcm [0] ) == 0 );

	unsigned irq_count  = e->low_mem [0x21];
	unsigned play_count = e->low_mem [0x30];
	TASK_CHECK( irq_count > 20 );
	TASK_CHECK( play_count > 10 ); // PLAY ran alongside IRQs
	TASK_CHECK( gme_warning( me ) == 0 );

	gme_delete( me );
	return 0;
}

static int case_vector_bank_and_restart()
{
	Bytes b = nsf_fixture( true );
	Music_Emu* me = open_emu( b );
	TASK_CHECK( me != 0 );
	Nsf_Emu* e = nsf( me );
	TASK_CHECK( gme_start_track( me, 0 ) == 0 );

	// initial override mirrors initial ROM vector (fixture fill is $EA)
	TASK_CHECK( e->cpu_read( 0xFFFE ) == 0xEA );
	TASK_CHECK( e->cpu_read( 0xFFFF ) == 0xEA );

	// independent writable override; normal read and vector fetch share it
	e->cpu_write( 0xFFFE, 0x00 );
	e->cpu_write( 0xFFFF, 0x81 );
	TASK_CHECK( e->cpu_read( 0xFFFE ) == 0x00 );
	TASK_CHECK( e->cpu_read( 0xFFFF ) == 0x81 );
	TASK_CHECK( *e->get_code( 0xFFFE ) == 0x00 );
	TASK_CHECK( *e->get_code( 0xFFFF ) == 0x81 );

	// swapping the top $F000 bank through $5FFF preserves the override
	e->cpu_write( 0x5FFF, 0x00 );
	TASK_CHECK( e->cpu_read( 0xFFFE ) == 0x00 );
	TASK_CHECK( e->cpu_read( 0xFFFF ) == 0x81 );
	TASK_CHECK( *e->get_code( 0xFFFE ) == 0x00 );

	// original input buffer is never modified
	TASK_CHECK( b [128 + 0xFFE] == 0xEA );
	TASK_CHECK( b [128 + 0xFFF] == 0xEA );

	// restart clears timer and vector override, then re-initializes from ROM
	e->nsf2_timer_running = true;
	e->nsf2_timer_flag = true;
	e->cpu_write( 0xFFFE, 0x12 );
	e->cpu_write( 0xFFFF, 0x34 );
	TASK_CHECK( gme_start_track( me, 0 ) == 0 );
	TASK_CHECK( !e->nsf2_timer_running );
	TASK_CHECK( !e->nsf2_timer_flag );
	TASK_CHECK( e->cpu_read( 0xFFFE ) == 0xEA );
	TASK_CHECK( e->cpu_read( 0xFFFF ) == 0xEA );

	gme_delete( me );
	return 0;
}

// ---------------------------------------------------------------------------
// audio group
// ---------------------------------------------------------------------------

static int case_fixture_dac_output()
{
	Pcm p = render( nsf_fixture( true ), 44100, false );
	TASK_CHECK( p.size() == 44100 );
	long nz = count_nonzero( p );
	TASK_CHECK( nz > 20000 ); // IRQ toggles $4011 continuously
	return 0;
}

static int case_chunk_equivalence()
{
	Pcm whole = render( nsf_fixture( true ), 44100, false );
	Pcm split = render( nsf_fixture( true ), 44100, true );
	TASK_CHECK( whole.size() == split.size() );
	TASK_CHECK( whole == split ); // no missed/duplicated events across chunks
	return 0;
}

static int case_nsf1_control()
{
	// NSF1 ignores the NSF2 feature interpretation and stays PLAY driven.
	Pcm nsf1 = render( nsf_fixture( false ), 44100, false );
	TASK_CHECK( nsf1.size() == 44100 );
	long nz = count_nonzero( nsf1 );
	TASK_CHECK( nz > 500 );
	TASK_CHECK( nz < 8000 ); // sparse legacy envelope pop pattern

	// even with all "NSF2" bytes set, version 1 must accept and render identically
	Bytes dirty = nsf_fixture( false );
	dirty [124] = 0xFF;
	dirty [125] = 0xFF;
	dirty [126] = 0xFF;
	dirty [127] = 0xFF;
	Pcm dirty_pcm = render( dirty, 44100, false );
	Pcm base_pcm = render( nsf_fixture( false ), 44100, false );
	TASK_CHECK( dirty_pcm == base_pcm );
	return 0;
}

static int case_restart_and_seek_audio()
{
	Pcm fresh = render( nsf_fixture( true ), 22050, false );

	Bytes b = nsf_fixture( true );
	Music_Emu* e = open_emu( b );
	TASK_CHECK( e != 0 );
	TASK_CHECK( gme_start_track( e, 0 ) == 0 );
	Pcm a( 11024 ); // even stereo sample count
	TASK_CHECK( gme_play( e, (int) a.size(), &a [0] ) == 0 );
	TASK_CHECK( gme_seek( e, 0 ) == 0 ); // backward seek restarts track
	Pcm after( 22050 );
	TASK_CHECK( gme_play( e, (int) after.size(), &after [0] ) == 0 );
	TASK_CHECK( after == fresh );       // timer/vector state fully reset
	gme_delete( e );
	return 0;
}

static int case_stack_dac_recognizable()
{
	// End-to-end: the routine that validates stacked state also drives DAC.
	Bytes b = make_nsf( stack_init_code(), stack_irq_code(), Bytes(), 0x50 );
	Pcm p = render( b, 44100, true ); // also exercises split delivery
	TASK_CHECK( p.size() == 44100 );
	TASK_CHECK( count_nonzero( p ) > 2000 );
	return 0;
}

// ---------------------------------------------------------------------------

struct task_case const task_cases [] = {
	{ "timer", "header_validation",  case_header_validation  },
	{ "timer", "timer_registers",    case_timer_registers    },
	{ "timer", "apu_overlap",        case_apu_overlap        },
	{ "cpu",   "irq_stack_b_flag",   case_irq_stack_b_flag   },
	{ "cpu",   "play_irq_nesting",   case_play_and_irq_nesting },
	{ "cpu",   "vector_bank_restart",case_vector_bank_and_restart },
	{ "audio", "fixture_dac",        case_fixture_dac_output },
	{ "audio", "chunk_equivalence",  case_chunk_equivalence  },
	{ "audio", "nsf1_control",       case_nsf1_control       },
	{ "audio", "restart_seek",       case_restart_and_seek_audio },
	{ "audio", "stack_dac",          case_stack_dac_recognizable },
};

size_t const task_case_count = sizeof task_cases / sizeof task_cases [0];
