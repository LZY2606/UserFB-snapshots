// Nintendo NES/Famicom NSF music file emulator

// Game_Music_Emu https://bitbucket.org/mpyne/game-music-emu/
#ifndef NSF_EMU_H
#define NSF_EMU_H

#include "Classic_Emu.h"
#include "Nes_Apu.h"
#include "Nes_Cpu.h"

class Nsf_Emu : private Nes_Cpu, public Classic_Emu {
	typedef Nes_Cpu cpu;
public:
	// Equalizer profiles for US NES and Japanese Famicom
	static equalizer_t const nes_eq;
	static equalizer_t const famicom_eq;

	// NSF file header
	enum { header_size = 0x80 };
	struct header_t
	{
		char tag [5];
		byte vers;
		byte track_count;
		byte first_track;
		byte load_addr [2];
		byte init_addr [2];
		byte play_addr [2];
		char game [32];
		char author [32];
		char copyright [32];
		byte ntsc_speed [2];
		byte banks [8];
		byte pal_speed [2];
		byte speed_flags;
		byte chip_flags;
		byte unused [4];
	};

	// Header for currently loaded file
	header_t const& header() const { return header_; }

	static gme_type_t static_type() { return gme_nsf_type; }

public:
	// deprecated
	using Music_Emu::load;
	blargg_err_t load( header_t const& h, Data_Reader& in ) // use Remaining_Reader
			{ return load_remaining_( &h, sizeof h, in ); }

public:
	Nsf_Emu();
	~Nsf_Emu();
	Nes_Apu* apu_() { return &apu; }

	// Resumable frame-advance core, also used by run_clocks(). frame_begin()
	// starts an internal frame of the given length in NES clocks. frame_step()
	// executes at most instr_budget real 6502 instructions (unlimited if
	// negative), reports how many it executed, and sets *frame_done once the
	// frame's clocks are fully emulated; when the budget runs out it stops
	// cleanly at an instruction boundary within the unfinished frame.
	// frame_end() finishes a completed frame and returns the emulated clocks.
	void        frame_begin( blip_time_t clocks );
	long        frame_step( long instr_budget, bool* frame_done );
	blip_time_t frame_end();

	// Frame sizing and sample buffer services for resumable drivers
	blip_time_t frame_length() const   { return frame_clocks(); }
	nes_time_t  frame_time() const     { return time(); }
	void        end_frame_samples( blip_time_t d ) { end_buf_frame( d ); }
	long        read_samples( short* out, long count ) { return read_buf_samples( out, count ); }
	long        samples_avail() const  { return buf_samples_avail(); }

protected:
	blargg_err_t track_info_( track_info_t*, int track ) const;
	blargg_err_t load_( Data_Reader& );
	blargg_err_t start_track_( int );
	blargg_err_t run_clocks( blip_time_t&, int );
	void set_tempo_( double );
	void set_voice( int, Blip_Buffer*, Blip_Buffer*, Blip_Buffer* );
	void update_eq( blip_eq_t const& );
	void unload();
protected:
	enum { bank_count = 8 };
	byte initial_banks [bank_count];
	nes_addr_t init_addr;
	nes_addr_t play_addr;
	double clock_rate_;
	bool pal_only;

	// timing
	Nes_Cpu::registers_t saved_state;
	nes_time_t next_play;
	nes_time_t play_period;
	blip_time_t frame_duration_;
	int play_extra;
	int play_ready;

	enum { rom_begin = 0x8000 };
	enum { bank_select_addr = 0x5FF8 };
	enum { bank_size = 0x1000 };
	Rom_Data<bank_size> rom;

public: private: friend class Nes_Cpu;
	void cpu_jsr( nes_addr_t );
	int cpu_read( nes_addr_t );
	void cpu_write( nes_addr_t, int );
	void cpu_write_misc( nes_addr_t, int );
	enum { badop_addr = bank_select_addr };

private:
	byte mmc5_mul [2];

	class Nes_Namco_Apu* namco;
	class Nes_Vrc6_Apu*  vrc6;
	class Nes_Fme7_Apu*  fme7;
	class Nes_Fds_Apu*   fds;
	class Nes_Mmc5_Apu*  mmc5;
	class Nes_Vrc7_Apu*  vrc7;
	Nes_Apu apu;
	blargg_vector<const char*> apu_names;
	static int pcm_read( void*, nes_addr_t );
	blargg_err_t init_sound();

	header_t header_;

	enum { sram_addr = 0x6000 };
	byte sram [0x2000];
	byte unmapped_code [Nes_Cpu::page_size + 8];
};

#endif
