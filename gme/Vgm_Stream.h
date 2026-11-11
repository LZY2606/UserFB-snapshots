// VGM 1.71 DAC stream control (commands 0x90-0x95)
// Specification: https://vgmrips.net/wiki/VGM_Specification
//
// Supported scope (see doc/vgm-stream-control.md):
//  - first YM2612, port 0, register 0x2A
//  - uncompressed bank 0, stream IDs 0..254
//  - 0x93 length mode 1 (length counted in sample writes), loop bit 7
//  - 0x95 block start, loop bit 0
// Anything outside that scope is read past completely, reported through the
// warning string, and stops the affected stream. Its parameters are never
// reinterpreted as following commands.

// Game_Music_Emu https://bitbucket.org/mpyne/game-music-emu/
#ifndef VGM_STREAM_H
#define VGM_STREAM_H

#include "blargg_common.h"

class Vgm_Emu_Impl;

class Vgm_Stream_Ctrl {
public:
	enum {
		ticks_per_sec = 44100,
		stream_count  = 255,  // valid IDs are 0..254; 0xFF is reserved (stop-all in 0x94)
		invalid_id    = 0xFF
	};

	// A type-0 0x67 data block. Blocks are concatenated, in first-seen order,
	// into one bank; the physical pointer deduplicates blocks revisited by a
	// VGM loop.
	struct Block {
		unsigned char const* phys;
		long        start; // first bank byte position of this block
		long        size;
	};

	Vgm_Stream_Ctrl() { reset(); }

	// Clear every stream, the bank and the legacy PCM cursor.
	void reset();

	// Register a type-0 data block. A physical block seen again (VGM loop)
	// is neither appended nor numbered a second time.
	blargg_err_t add_block( unsigned char const* phys, long size );

	// Read one byte from the concatenated bank. Returns false if offset is
	// outside every known block; no out-of-bounds pointer is ever formed.
	bool read_bank( long offset, unsigned char* out ) const;

	// 0x90 DAC stream setup: chip type, chip target/port, register.
	void setup( Vgm_Emu_Impl& owner, int id, int chip_type, int target, int reg );

	// 0x91 set data bank/step/base. Reconfiguring stops the stream first.
	void set_data( Vgm_Emu_Impl& owner, int id, int bank_id, int step, int base );

	// 0x92 set frequency. Position and phase remainder are preserved.
	void set_frequency( Vgm_Emu_Impl& owner, int id, uint32_t frequency );

	// 0x93 start. bank_offset is a byte position within the concatenated
	// bank; flags bit 7 requests looping, bits 0..1 select the length mode.
	void start( Vgm_Emu_Impl& owner, int id, uint32_t bank_offset,
			int flags, uint32_t length, int64_t tick );

	// 0x95 fast start: play one whole block. flags bit 0 requests looping.
	void fast_start( Vgm_Emu_Impl& owner, int id, int block_id,
			int flags, int64_t tick );

	// 0x94 stop. id 0xFF stops every stream; other IDs stop just one.
	void stop( int id );

	// Write every value whose phase comes due at ticks in (current, target].
	// Due streams are handled in ascending ID order.
	void advance( Vgm_Emu_Impl& owner, int64_t target );

	// Absolute 44100 Hz tick up to which phases have been pumped.
	int64_t tick() const { return sched_tick; }

	// Legacy 0xE0/0x8n cursor, addressed through the same concatenated bank
	// but independent of every stream.
	bool legacy_seek( Vgm_Emu_Impl& owner, long offset );
	bool legacy_read( Vgm_Emu_Impl& owner, unsigned char* out );

private:
	struct Stream {
		// configuration
		bool     configured; // accepted 0x90 seen
		uint8_t  bank_id;
		uint16_t step;
		uint16_t base;
		uint32_t frequency;

		// playback state
		bool     active;
		bool     loop_flag;
		uint32_t phase;       // integer phase remainder, always < ticks_per_sec
		long     start_pos;   // bank byte of the first value of a pass
		long     cur_pos;     // bank byte of the next value
		uint32_t writes_left;
		uint32_t writes_total;

		void clear();
	};

	Stream stream [stream_count];
	blargg_vector<Block> blocks;
	int    block_count_;
	long   bank_bytes;
	int64_t sched_tick; // absolute tick up to which phases have been pumped
	long   legacy_pos;
	bool   legacy_warned;

	const Block* find_block( long offset ) const;
	void fail( Vgm_Emu_Impl& owner, int id, const char* what );
	bool config_usable( Stream const& s ) const;
	void begin_playback( Vgm_Emu_Impl& owner, Stream& s, int id,
			int64_t start_pos, uint32_t total, bool loop, int64_t tick );
};

#endif
