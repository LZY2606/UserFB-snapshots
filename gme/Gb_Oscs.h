// Private oscillators used by Gb_Apu

// Gb_Snd_Emu 0.1.5
#ifndef GB_OSCS_H
#define GB_OSCS_H

#include "blargg_common.h"
#include "Blip_Buffer.h"

struct Gb_Osc
{
	enum { trigger = 0x80 };
	enum { len_enabled_mask = 0x40 };

	// One physical output contribution: a buffer and the NR50 field (0..7)
	// whose (field + 1) / 8 master gain the associated gain synth applies.
	struct out_t
	{
		Blip_Buffer* buf;
		int gain;
	};
	enum { max_outs = 2 };

	Blip_Buffer* outputs [4]; // NULL, right, left, center
	Blip_Buffer* output;
	int output_select;

	// Active routed endpoints for the current register/gain configuration.
	// A center endpoint is a single contribution shared by both sides when
	// their gains are equal; otherwise left and right are separate.
	out_t outs [max_outs];
	int out_count;
	uint8_t* regs; // osc's 5 registers

	int delay;
	int last_amp;
	int volume;
	int length;
	int enabled;

	void reset();
	void clock_length();
	int frequency() const { return (regs [4] & 7) * 0x100 + regs [3]; }
};

struct Gb_Env : Gb_Osc
{
	int env_delay;

	void reset();
	void clock_envelope();
	bool write_register( int, int );
};

struct Gb_Square : Gb_Env
{
	enum { period_mask = 0x70 };
	enum { shift_mask  = 0x07 };

	typedef Blip_Synth<blip_good_quality,1> Synth;
	Synth const* synth;
	int sweep_delay;
	int sweep_freq;
	int phase;

	void reset();
	void clock_sweep();
	void run( blip_time_t, blip_time_t, int playing );
};

struct Gb_Noise : Gb_Env
{
	typedef Blip_Synth<blip_med_quality,1> Synth;
	Synth const* synth;
	unsigned bits;

	void run( blip_time_t, blip_time_t, int playing );
};

struct Gb_Wave : Gb_Osc
{
	typedef Blip_Synth<blip_med_quality,1> Synth;
	Synth const* synth;
	int wave_pos;
	enum { wave_size = 32 };
	uint8_t wave [wave_size];

	void write_register( int, int );
	void run( blip_time_t, blip_time_t, int playing );
};

inline void Gb_Env::reset()
{
	env_delay = 0;
	Gb_Osc::reset();
}

#endif
