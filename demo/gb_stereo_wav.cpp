// Example: render Game Boy audio through gme and write a 16-bit stereo WAV.
//
// This demonstrates the independent NR50 left/right master volumes. It builds
// a tiny GBS in memory that plays a continuous pulse wave on square 1 routed
// to both NR51 outputs, renders a left-heavy segment (NR50 = 0x70) followed
// by a right-heavy segment (NR50 = 0x07), and writes both as one stereo WAV.
//
// NR50 (0xFF24): bits 6..4 set left gain, bits 2..0 set right gain.
// Each 3-bit field gives gain (field + 1) / 8; zero is 1/8 gain, not mute.
// The VIN bits (7 and 3) are ignored by this emulator.
// NR51 (0xFF25): low nibble routes channels 1..4 right, high nibble left.
//
// Build against the task build:
//   c++ -std=c++11 -O2 -I. -Igme \
//       demo/gb_stereo_wav.cpp .task-build/libtaskgme.a \
//       -o .task-build/gb_stereo_wav
//   .task-build/gb_stereo_wav gb_stereo.wav

#include "gme.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <algorithm>

// Build a minimal GBS whose init code writes the Game Boy sound registers.
// The play routine is just RET, so the triggered note sustains.
static std::vector<unsigned char> make_gbs( unsigned char nr50 )
{
	std::vector<unsigned char> b( 112 + 256, 0 );
	b [0] = 'G'; b [1] = 'B'; b [2] = 'S';
	b [3] = 1;   // version
	b [4] = 1;   // one track
	b [5] = 1;
	b [6] = 0; b [7] = 0x40;   // load address 0x4000
	b [8] = 0; b [9] = 0x40;   // init address 0x4000
	b [10] = 0x80; b [11] = 0x40; // play address 0x4080
	b [12] = 0xfe; b [13] = 0xff; // stack pointer

	// Register init table: NR52 on, NR50, NR51 square1 both, duty/envelope,
	// frequency low/high with trigger.
	const unsigned char pairs [] = {
		0x26, 0x80,
		0x24, nr50,
		0x25, 0x11,
		0x11, 0x80,
		0x12, 0xf0,
		0x13, 0x00,
		0x14, 0x87
	};
	size_t p = 112;
	for ( size_t i = 0; i < sizeof pairs; i += 2 )
	{
		b [p++] = 0x3e;             // LD A, imm8
		b [p++] = pairs [i + 1];
		b [p++] = 0xe0;             // LDH (imm8), A
		b [p++] = pairs [i];
	}
	b [p] = 0xc9;                   // RET
	b [112 + 0x80] = 0xc9;          // play routine: RET
	return b;
}

static std::vector<short> render( unsigned char nr50, long samples )
{
	std::vector<unsigned char> data = make_gbs( nr50 );
	Music_Emu* emu = 0;
	if ( gme_open_data( &data [0], (long) data.size(), &emu, 48000 ) )
		return std::vector<short>();
	gme_ignore_silence( emu, 1 );
	if ( gme_start_track( emu, 0 ) )
	{
		gme_delete( emu );
		return std::vector<short>();
	}
	std::vector<short> out( samples );
	gme_err_t err = gme_play( emu, (int) samples, out.data() );
	gme_delete( emu );
	if ( err )
		return std::vector<short>();
	return out;
}

static void write_le32( FILE* f, uint32_t v )
{
	unsigned char d [4] = { (unsigned char) v, (unsigned char) (v >> 8),
			(unsigned char) (v >> 16), (unsigned char) (v >> 24) };
	std::fwrite( d, 1, 4, f );
}

static void write_le16( FILE* f, uint16_t v )
{
	unsigned char d [2] = { (unsigned char) v, (unsigned char) (v >> 8) };
	std::fwrite( d, 1, 2, f );
}

static bool write_stereo_wav( const char* path, const std::vector<short>& pcm,
		int sample_rate )
{
	FILE* f = std::fopen( path, "wb" );
	if ( !f )
		return false;

	uint32_t data_bytes = (uint32_t) (pcm.size() * sizeof(short));
	std::fwrite( "RIFF", 1, 4, f );
	write_le32( f, 36 + data_bytes );
	std::fwrite( "WAVE", 1, 4, f );
	std::fwrite( "fmt ", 1, 4, f );
	write_le32( f, 16 );             // PCM chunk size
	write_le16( f, 1 );              // PCM format
	write_le16( f, 2 );              // stereo
	write_le32( f, sample_rate );
	write_le32( f, sample_rate * 2 * (int) sizeof(short) ); // byte rate
	write_le16( f, 2 * (int) sizeof(short) );                // block align
	write_le16( f, 16 );             // bits per sample
	std::fwrite( "data", 1, 4, f );
	write_le32( f, data_bytes );
	std::fwrite( pcm.data(), sizeof(short), pcm.size(), f );
	std::fclose( f );
	return true;
}

int main( int argc, char** argv )
{
	const char* path = argc > 1 ? argv [1] : "gb_stereo.wav";
	const long segment = 48000; // shorts (~0.5 s, stereo interleaved)

	std::vector<short> left_heavy  = render( 0x70, segment );
	std::vector<short> right_heavy = render( 0x07, segment );
	if ( left_heavy.empty() || right_heavy.empty() )
	{
		std::fprintf( stderr, "rendering failed\n" );
		return 1;
	}

	std::vector<short> pcm;
	pcm.reserve( left_heavy.size() + right_heavy.size() );
	pcm.insert( pcm.end(), left_heavy.begin(), left_heavy.end() );
	pcm.insert( pcm.end(), right_heavy.begin(), right_heavy.end() );

	if ( !write_stereo_wav( path, pcm, 48000 ) )
	{
		std::fprintf( stderr, "cannot write %s\n", path );
		return 1;
	}

	long peak_l = 0, peak_r = 0;
	for ( size_t i = 0; i + 1 < left_heavy.size(); i += 2 )
	{
		peak_l = std::max<long>( peak_l, std::abs( left_heavy [i] ) );
		peak_r = std::max<long>( peak_r, std::abs( left_heavy [i + 1] ) );
	}
	std::printf( "wrote %s (%zu stereo frames); 0x70 segment peak L=%ld R=%ld (ratio ~8)\n",
			path, pcm.size() / 2, peak_l, peak_r );
	return 0;
}
