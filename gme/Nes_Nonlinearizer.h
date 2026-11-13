// Joint nonlinear DAC mixer for the five NES 2A03 channels.
//
// In nonlinear mode the five oscillators keep running their ordinary state
// machines on the shared CPU cycle timeline, but instead of sending deltas to
// individual Blip_Buffers they report each raw DAC level change here. The
// mixer merges changes that happen during the same CPU cycle, evaluates the
// combined DAC curve once, and sends the difference of the resulting mixed
// integer to a single Blip_Buffer through one Blip_Synth.

// Nes_Snd_Emu 0.1.8
#ifndef NES_NONLINEARIZER_H
#define NES_NONLINEARIZER_H

#include "blargg_common.h"
#include "Blip_Buffer.h"

#ifndef NES_TIME_T_DEFINED
#define NES_TIME_T_DEFINED
typedef int32_t nes_time_t; // CPU clock cycle count
#endif

class Nes_Apu;

struct Nes_Nonlinear_Event
{
	nes_time_t time;
	int osc;
	int amp; // raw DAC level of the oscillator at this time
};

class Nes_Nonlinearizer
{
public:
	typedef void (*listener_t)( void* user_data, nes_time_t time, int mixed );

	Nes_Nonlinearizer();

	// Route the joint signal to buf. NULL detaches the mixer and restores the
	// regular per-oscillator routing.
	void attach( Nes_Apu* apu, Blip_Buffer* buf );
	void detach();

	bool attached() const { return buf != 0; }

	// Drop any pending events and use the oscillators' current raw levels as
	// the initial mixing reference, without emitting anything.
	void reset_reference();

	// Record a raw DAC level change of oscillator osc (0..4) at time t.
	// Called from the oscillator run loops.
	void level( int osc, nes_time_t time, int amp )
	{
		if ( event_count == (int) events.size() )
			events.resize( event_count + 64 );
		events [event_count].time = time;
		events [event_count].osc  = osc;
		events [event_count].amp  = amp;
		++event_count;
	}

	// Merge every recorded event up to end_time and emit the resulting mixed
	// deltas. Events belonging to later cycles (e.g. DMC run ahead of the other
	// four oscillators) stay queued.
	void flush( nes_time_t end_time );

	// Flush the final segment, then shift any events still queued (DMC run
	// ahead) to the new frame's time origin.
	void end_frame( nes_time_t frame_time );

	// Apply a new mute mask at time t. Muted oscillators contribute zero to
	// the mixing formula while their internal state keeps running.
	void set_mute_mask( nes_time_t time, int mask );

	int mute() const { return mute_mask; }

	void volume( double v )       { synth.volume( v ); }
	void treble_eq( blip_eq_t const& eq ) { synth.treble_eq( eq ); }

	void set_listener( listener_t fn, void* data )
	{
		listener = fn;
		listener_data = data;
	}

	// Mixed integer for the five raw DAC levels using the fixed nonlinear
	// convention. Exposed so tests can derive expectations independently.
	static int mix_levels( int p1, int p2, int t, int n, int d );

private:
	Nes_Apu* apu;
	Blip_Buffer* buf;
	Blip_Synth<blip_good_quality, 16384> synth;

	blargg_vector<Nes_Nonlinear_Event> events;
	int event_count;

	int raw [5];      // latest raw DAC level of each oscillator
	int mute_mask;
	int last_mixed;

	listener_t listener;
	void* listener_data;

	int current_mixed() const;
};

#endif
