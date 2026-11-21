// Pausable, budget-driven NSF playback entry point

// Game_Music_Emu https://bitbucket.org/mpyne/game-music-emu/
#ifndef NSF_PUMP_H
#define NSF_PUMP_H

#include "blargg_common.h"
#include <stdint.h>

class Nsf_Emu;

// Drives NSF playback in small, caller-bounded amounts of real emulation
// work, without threads. The caller may regain control while an internal
// audio frame is only partially emulated; the next pump() call resumes at
// the next 6502 instruction. See doc/nsf-pump.md for details.
class Nsf_Pump {
public:
	Nsf_Pump();
	~Nsf_Pump();

	// Accounting for one pump() call. work always equals
	// instructions + frames and never exceeds the call's budget.
	struct Result {
		unsigned work;         // total budget units consumed
		unsigned instructions; // real 6502 instructions completed
		long     frames;       // stereo frames (interleaved sample pairs) delivered
	};

	// Cumulative counters since the last successful begin() or reset().
	struct Progress {
		uint64_t instructions;     // real 6502 instructions completed
		uint64_t delivered_frames; // stereo frames handed to the caller
		int64_t  emulated_clocks;  // NES clocks executed or idle-advanced
	};

	// Copies and loads a complete NSF1 file (NTSC/PAL, no expansion audio,
	// rate 44100 or 48000, valid zero-based track) and performs the usual
	// load and track-start setup without executing INIT or PLAY. On success
	// replaces any previous playback and clears progress; on failure the
	// previous playback state is kept.
	blargg_err_t begin( const void* data, long size, long rate, int track );

	// Emulates until the budget (1 unit per 6502 instruction plus 1 unit per
	// delivered PCM frame) or capacity_frames is exhausted, writing at most
	// capacity_frames stereo frames to out. Zero capacity or zero budget
	// succeeds without making any progress.
	blargg_err_t pump( short* out, long capacity_frames, unsigned budget, Result* result );

	// Cumulative progress. Has no side effects.
	Progress progress() const;

	// Stops playback and clears progress. Safe to call any number of times.
	void reset();

private:
	Nsf_Emu* emu_;
	bool frame_active;
	uint64_t total_instructions;
	uint64_t total_frames;
	int64_t total_clocks;

	// noncopyable
	Nsf_Pump( const Nsf_Pump& );
	Nsf_Pump& operator = ( const Nsf_Pump& );
};

#endif
