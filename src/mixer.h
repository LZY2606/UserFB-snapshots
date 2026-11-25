#ifndef LIBXMP_MIXER_H
#define LIBXMP_MIXER_H

#include "common.h"

#define C4_PERIOD	428.0

#define SMIX_NUMVOC	128	/* default number of softmixer voices */
#define SMIX_SHIFT	16
#define SMIX_MASK	0xffff

#define FILTER_SHIFT	22
#define ANTICLICK_SHIFT	3

#ifdef LIBXMP_PAULA_SIMULATOR
#include "paula.h"
#endif

/* Sample wraparound backup state, kept per voice so a tick can be
 * rendered in multiple fragments. */
struct loop_data
{
#define LOOP_PROLOGUE 1
#define LOOP_EPILOGUE 2
	void *sptr;
	int start;
	int end;
	int first_loop;
	int _16bit;
	int active;
	int prologue_num;
	int epilogue_num;
	uint8 prologue[LOOP_PROLOGUE * 2 /* 16-bit */ * 2 /* stereo */];
	uint8 epilogue[LOOP_EPILOGUE * 2 /* 16-bit */ * 2 /* stereo */];
};

struct mixer_voice {
	int chn;		/* channel number */
	int root;		/* */
	int note;		/* */
#define PAN_SURROUND 0x8000
	int pan;		/* */
	int vol;		/* */
	double period;		/* current period */
	double pos;		/* position in sample */
	int pos0;		/* position in sample before mixing */
	int fidx;		/* mixer function index */
	int ins;		/* instrument number */
	int smp;		/* sample number */
	int start;		/* loop start */
	int end;		/* loop end */
	int act;		/* nna info & status of voice */
	int key;		/* key for DCA note check */
	int old_vl;		/* previous volume, left channel */
	int old_vr;		/* previous volume, right channel */
	int sleft;		/* last left sample output, in 32bit */
	int sright;		/* last right sample output, in 32bit */
#define VOICE_RELEASE	(1 << 0)
#define ANTICLICK	(1 << 1)
#define SAMPLE_LOOP	(1 << 2)
#define VOICE_REVERSE	(1 << 3)
#define VOICE_BIDIR	(1 << 4)
#define SAMPLE_QUEUED	(1 << 5)
#define SAMPLE_PAUSED	(1 << 6)
#define VOICE_RAMP_ARM	(1 << 7) /* timed player: rearm attack ramp mid-tick */
	int flags;		/* flags */
	void *sptr;		/* sample pointer */
#ifdef LIBXMP_PAULA_SIMULATOR
	struct paula_state *paula; /* paula simulation state */
#endif

	struct {		/* Protracker queued instrument change */
		int smp;
	} queued;

	/* Timed player fragment state. Unused by the regular mixer. */
	int ramp_remain;	/* attack ramp frames left in this tick */
	int ramp_delta_l;
	int ramp_delta_r;
	int mix_pos;		/* persistent integer sample position */
	int mix_frac;		/* persistent fractional sample position */
	int mix_resync;		/* re-seed integer position from vi->pos */
	struct loop_data loop;	/* persistent sample wraparound state */

#ifndef LIBXMP_CORE_DISABLE_IT
	struct {
		int r1;		/* filter variables */
		int r2;
		int l1;
		int l2;
		int a0;
		int b0;
		int b1;
		int cutoff;
		int resonance;
	} filter;
#endif
};

LIBXMP_BEGIN_DECLS

int	libxmp_mixer_on		(struct context_data *, int, int, int);
void	libxmp_mixer_off	(struct context_data *);
void    libxmp_mixer_setvol	(struct context_data *, int, int);
void    libxmp_mixer_seteffect	(struct context_data *, int, int, int);
void    libxmp_mixer_setpan	(struct context_data *, int, int);
int	libxmp_mixer_numvoices	(struct context_data *, int);
void	libxmp_mixer_softmixer	(struct context_data *);
void	libxmp_mixer_reset	(struct context_data *);
void	libxmp_mixer_setpatch	(struct context_data *, int, int, int);
void	libxmp_mixer_queuepatch	(struct context_data *, int, int);
void	libxmp_mixer_voicepos	(struct context_data *, int, double, int);
double	libxmp_mixer_getvoicepos(struct context_data *, int);
void	libxmp_mixer_setnote	(struct context_data *, int, int);
void	libxmp_mixer_setperiod	(struct context_data *, int, double);
void	libxmp_mixer_release	(struct context_data *, int, int);
void	libxmp_mixer_reverse	(struct context_data *, int, int);
int	libxmp_mixer_get_ticksize(int freq, double time_factor, double rrate, int bpm);

/* Sample-accurate timed playback */
void	libxmp_mixer_timed_begin_tick	(struct context_data *);
int	libxmp_mixer_timed_render	(struct context_data *, int, int);
void	libxmp_mixer_timed_finish_tick	(struct context_data *);
void	libxmp_mixer_timed_arm_ramp	(struct context_data *, int);
void	libxmp_mixer_timed_start_decay	(struct context_data *, int);
void	libxmp_mixer_timed_downmix	(struct context_data *, int16 *, int, int);
void	libxmp_mixer_timed_reset	(struct context_data *);

LIBXMP_END_DECLS

#endif /* LIBXMP_MIXER_H */
