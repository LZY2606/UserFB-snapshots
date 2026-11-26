/* Stereo sample loading, playback, saving and state snapshots. CC0. */
#include "contract.h"

#include <xm.h>

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SF_CAP (1u << 16)
#define SF_MAX_SAMPLES 6
#define SF_MAX_EVENTS 16
#define MICROSTEPS 4096u
#define TICK_SUBSAMPLES 8192u
#define WARM_FRAMES 300u

struct sf_sample {
	uint32_t declared_length;
	uint32_t loop_start;
	uint32_t loop_end;
	uint8_t flags; /* S3M sample header flags */
	uint8_t volume;
	const int16_t* left;
	const int16_t* right; /* NULL for mono */
};

struct sf_event {
	uint8_t row;
	uint8_t note;
	uint8_t instrument;
	uint8_t effect;
	uint8_t param;
};

struct sf_file {
	uint8_t data[SF_CAP];
	struct sf_sample samples[SF_MAX_SAMPLES];
	struct sf_event events[SF_MAX_EVENTS];
	unsigned num_samples;
	unsigned num_events;
	unsigned used;
	bool signed_samples;
};

static void sf_u16(uint8_t* p, unsigned v) {
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static void sf_u32(uint8_t* p, uint32_t v) {
	sf_u16(p, v & 0xFFFFu);
	sf_u16(p + 2, v >> 16);
}

static void sf_init(struct sf_file* f, unsigned num_samples, bool signed_data) {
	memset(f, 0, sizeof(*f));
	f->num_samples = num_samples;
	f->signed_samples = signed_data;

	uint8_t* d = f->data;
	memcpy(d, "stereo contract fixture", 24);
	d[28] = 0x1A;
	d[29] = 0x10;
	sf_u16(d + 32, 2); /* POT length, including terminator */
	sf_u16(d + 34, num_samples);
	sf_u16(d + 36, 1); /* one pattern */
	sf_u16(d + 40, 0x5000);
	/* The loader treats header word 42 == 1 as signed PCM. */
	sf_u16(d + 42, signed_data ? 1 : 2);
	memcpy(d + 44, "SCRM", 4);
	d[48] = 64; /* global volume */
	d[49] = 6; /* tempo */
	d[50] = 255; /* BPM */
	d[51] = 0x80;
	d[53] = 0; /* use ST3 default channel pannings */
	memset(d + 64, 255, 32);
	d[64] = 0; /* physical channel 0 is a normal PCM channel */
	d[96] = 0;
	d[97] = 255;
}

static void sf_encode_point(uint8_t* out, int16_t v, bool bits16,
                            bool signed_data) {
	if(bits16) {
		uint16_t raw = signed_data ? (uint16_t)v
		                            : (uint16_t)((int)v + 32768);
		sf_u16(out, raw);
	} else {
		int8_t s8 = (int8_t)(v / 256);
		uint8_t raw = signed_data ? (uint8_t)s8
		                          : (uint8_t)((int)s8 + 128);
		*out = raw;
	}
}

static void sf_finalize(struct sf_file* f) {
	uint8_t* d = f->data;
	unsigned header_base = 256;
	unsigned header_stride = 96;
	unsigned sample_base = header_base + f->num_samples * header_stride;
	sample_base = (sample_base + 15u) & ~15u;

	for(unsigned i = 0; i < f->num_samples; ++i) {
		struct sf_sample* s = f->samples + i;
		unsigned h = header_base + i * header_stride;
		sf_u16(d + 98 + 2 * i, h / 16);

		d[h] = 1; /* PCM */
		sf_u32(d + h + 16, s->declared_length);
		sf_u32(d + h + 20, s->loop_start);
		sf_u32(d + h + 24, s->loop_end);
		d[h + 28] = s->volume ? s->volume : 64;
		d[h + 31] = s->flags;
		sf_u32(d + h + 32, 8363); /* C-4 reference frequency */
		memcpy(d + h + 76, "SCRS", 4);

		uint32_t para = sample_base / 16u;
		d[h + 13] = (uint8_t)(para >> 16);
		sf_u16(d + h + 14, para & 0xFFFFu);

		bool bits16 = (s->flags & 4) != 0;
		bool stereo = (s->flags & 2) != 0;
		unsigned bps = bits16 ? 2 : 1;
		uint32_t plane_bytes = s->declared_length * bps;

		for(uint32_t k = 0; k < s->declared_length; ++k) {
			sf_encode_point(d + sample_base + k * bps,
			                 s->left[k], bits16, f->signed_samples);
			if(stereo) {
				sf_encode_point(d + sample_base + plane_bytes + k * bps,
				                 s->right[k], bits16, f->signed_samples);
			}
		}

		sample_base += plane_bytes * (stereo ? 2u : 1u);
		sample_base = (sample_base + 15u) & ~15u;
	}

	unsigned pattern_offset = sample_base;
	pattern_offset = (pattern_offset + 15u) & ~15u;
	sf_u16(d + 98 + 2 * f->num_samples, pattern_offset / 16u);

	uint8_t packed[1024];
	unsigned pn = 0;
	for(unsigned row = 0; row < 64; ++row) {
		const struct sf_event* e = NULL;
		for(unsigned i = 0; i < f->num_events; ++i) {
			if(f->events[i].row == row) { e = f->events + i; break; }
		}
		if(e) {
			packed[pn++] = 0x20 | 0x80; /* note/instrument + effect */
			packed[pn++] = e->note;
			packed[pn++] = e->instrument;
			packed[pn++] = e->effect;
			packed[pn++] = e->param;
		}
		packed[pn++] = 0; /* end of row */
	}

	uint16_t pattern_size = (uint16_t)(2 + pn);
	sf_u16(d + pattern_offset, pattern_size);
	memcpy(d + pattern_offset + 2, packed, pn);
	f->used = pattern_offset + pattern_size;
}

static xm_context_t* sf_load(struct check_result* r, struct sf_file* f) {
	sf_finalize(f);
	xm_prescan_data_t* scan = malloc(XM_PRESCAN_DATA_SIZE);
	Z_CHECK(r, scan != NULL);
	if(!scan) return NULL;

	bool prescanned = xm_prescan_module((const char*)f->data, f->used, scan);
	Z_CHECK(r, prescanned);
	if(!prescanned) {
		free(scan);
		return NULL;
	}

	uint32_t pool_size = xm_size_for_context(scan);
	void* pool = malloc(pool_size);
	Z_CHECK(r, pool != NULL);
	xm_context_t* ctx = NULL;
	if(pool) {
		ctx = xm_create_context(pool, scan, (const char*)f->data, f->used);
		Z_CHECK(r, ctx != NULL);
		xm_set_sample_rate(ctx, 48000);
	}
	free(scan);
	return ctx;
}

static void sf_free(xm_context_t* ctx) {
	free(ctx);
}

static float point_to_float(xm_sample_point_t p) {
	return _Generic(p,
		int8_t: (float)p / 128.f,
		int16_t: (float)p / 32768.f,
		float: p);
}

static bool g_ramp_detected;
static bool g_ramp_known;

static bool ramp_is_enabled(void) {
	if(g_ramp_known) return g_ramp_detected;
	g_ramp_known = true;

	struct sf_file f;
	static int16_t wave[16];
	for(unsigned i = 0; i < 16; ++i) wave[i] = 12000;
	sf_init(&f, 1, false);
	f.samples[0] = (struct sf_sample){
		.declared_length = 16, .flags = 0, .volume = 64, .left = wave
	};
	f.events[0] = (struct sf_event){0, 0x40, 1, 24, 64};
	f.num_events = 1;

	struct check_result local = {0};
	xm_context_t* ctx = sf_load(&local, &f);
	if(!ctx) {
		/* The real checks report load failures; default to the public
		   source build's ramping configuration if this probe cannot run. */
		g_ramp_detected = true;
		return true;
	}
	float out[2] = {0, 0};
	xm_generate_samples(ctx, out, 1);
	g_ramp_detected = (out[0] == 0.f && out[1] == 0.f);
	sf_free(ctx);
	return g_ramp_detected;
}

/* ----- load group ----- */

void check_load(struct check_result* r) {
	static int16_t u8_l[20], u8_r[20], u16_mono[17];
	for(unsigned i = 0; i < 20; ++i) {
		u8_l[i] = (int16_t)(256 * (10 + (i % 5) * 7));
		u8_r[i] = (int16_t)(-256 * (12 + (i % 6) * 9));
	}
	for(unsigned i = 16; i < 20; ++i) {
		u8_l[i] = 32000;
		u8_r[i] = -32000;
	}
	for(unsigned i = 0; i < 17; ++i) {
		u16_mono[i] = (int16_t)(-14000 + 1700 * (int)i);
	}

	struct sf_file f;
	sf_init(&f, 2, false);
	f.samples[0] = (struct sf_sample){
		.declared_length = 20, .loop_start = 0, .loop_end = 16,
		.flags = 1 | 2, .volume = 64, .left = u8_l, .right = u8_r
	};
	f.samples[1] = (struct sf_sample){
		.declared_length = 17, .flags = 4, .volume = 64,
		.left = u16_mono
	};

	xm_context_t* ctx = sf_load(r, &f);
	Z_CHECK(r, ctx != NULL);
	if(!ctx) return;

	Z_CHECK(r, xm_get_number_of_samples(ctx) == 2);
	Z_CHECK(r, xm_get_sample_channels(ctx, 0) == 2);
	Z_CHECK(r, xm_get_sample_channels(ctx, 1) == 1);
	Z_CHECK(r, xm_get_sample_channels(ctx, 2) == 0);
	Z_CHECK(r, xm_get_sample_channels(ctx, 65535) == 0);

	uint32_t l0 = 999, r0_len = 999, m1 = 999;
	xm_sample_point_t* p0 = xm_get_sample_waveform(ctx, 0, &l0);
	xm_sample_point_t* q0 = xm_get_sample_waveform_channel(ctx, 0, 1, &r0_len);
	xm_sample_point_t* p1 = xm_get_sample_waveform(ctx, 1, &m1);
	Z_CHECK(r, l0 == 16);
	Z_CHECK(r, r0_len == 16);
	Z_CHECK(r, m1 == 17); /* odd 16-bit frame count is not divided by two */
	Z_CHECK(r, p0 != NULL && q0 != NULL && p1 != NULL);
	Z_CHECK(r, q0 == p0 + 16);
	Z_CHECK(r, p1 == p0 + 32); /* left and right planes, then next sample */

	for(unsigned i = 0; i < 16; ++i) {
		Z_CHECK(r, point_to_float(p0[i]) == point_to_float((xm_sample_point_t)u8_l[i]));
		Z_CHECK(r, point_to_float(q0[i]) == point_to_float((xm_sample_point_t)u8_r[i]));
	}
	for(unsigned i = 0; i < 17; ++i) {
		Z_CHECK(r, p1[i] == (xm_sample_point_t)u16_mono[i]);
	}

	/* The right plane starts after the original 20-frame left plane.
	   A loader that used the trimmed 16-frame length would read u8_l[16]. */
	Z_CHECK(r, q0[0] == (xm_sample_point_t)u8_r[0]);
	Z_CHECK(r, q0[0] != (xm_sample_point_t)u8_l[16]);

	uint32_t bad_len = 12345;
	Z_CHECK(r, xm_get_sample_waveform_channel(ctx, 7, 0, &bad_len) == NULL);
	Z_CHECK(r, bad_len == 0);
	bad_len = 12345;
	Z_CHECK(r, xm_get_sample_waveform_channel(ctx, 0, 2, &bad_len) == NULL);
	Z_CHECK(r, bad_len == 0);
	bad_len = 12345;
	Z_CHECK(r, xm_get_sample_waveform_channel(ctx, 1, 1, &bad_len) == NULL);
	Z_CHECK(r, bad_len == 0);
	Z_CHECK(r, xm_get_sample_waveform_channel(ctx, 0, 1, NULL) == q0);

	sf_free(ctx);

	/* Signed 16-bit stereo, also with an odd frame count. */
	static int16_t s16_l[17], s16_r[17], s8_mono[13];
	for(unsigned i = 0; i < 17; ++i) {
		s16_l[i] = (int16_t)(-30000 + 3500 * (int)i);
		s16_r[i] = (int16_t)(28000 - 3300 * (int)i);
	}
	for(unsigned i = 0; i < 13; ++i) s8_mono[i] = (int16_t)(256 * (i - 6));

	sf_init(&f, 2, true);
	f.samples[0] = (struct sf_sample){
		.declared_length = 17, .flags = 2 | 4, .volume = 64,
		.left = s16_l, .right = s16_r
	};
	f.samples[1] = (struct sf_sample){
		.declared_length = 13, .flags = 0, .volume = 64, .left = s8_mono
	};
	ctx = sf_load(r, &f);
	Z_CHECK(r, ctx != NULL);
	if(ctx) {
		uint32_t a = 0, b = 0, c = 0;
		xm_sample_point_t* l = xm_get_sample_waveform_channel(ctx, 0, 0, &a);
		xm_sample_point_t* rr = xm_get_sample_waveform_channel(ctx, 0, 1, &b);
		xm_sample_point_t* m = xm_get_sample_waveform(ctx, 1, &c);
		Z_CHECK(r, xm_get_sample_channels(ctx, 0) == 2);
		Z_CHECK(r, a == 17 && b == 17 && c == 13);
		Z_CHECK(r, rr == l + 17);
		for(unsigned i = 0; i < 17; ++i) {
			Z_CHECK(r, l[i] == (xm_sample_point_t)s16_l[i]);
			Z_CHECK(r, rr[i] == (xm_sample_point_t)s16_r[i]);
		}
		for(unsigned i = 0; i < 13; ++i) {
			Z_CHECK(r, point_to_float(m[i])
			        == point_to_float((xm_sample_point_t)s8_mono[i]));
		}
		sf_free(ctx);
	}
}

/* ----- playback reference ----- */

struct ref_plane {
	const xm_sample_point_t* data;
	uint32_t length;
	uint32_t loop_start;
	uint32_t loop_length;
};

static float ref_sample(const struct ref_plane* s, uint64_t position) {
	if(s->loop_length) {
		uint64_t off = (uint64_t)s->loop_start * MICROSTEPS;
		uint64_t loop_span = (uint64_t)s->loop_length * MICROSTEPS;
		if(position >= (uint64_t)s->length * MICROSTEPS) {
			position -= off;
			position %= loop_span;
			position += off;
		}
	}

	uint32_t a = (uint32_t)(position / MICROSTEPS);
	uint32_t fraction = (uint32_t)(position % MICROSTEPS);
	uint32_t b;
	if(!s->loop_length) {
		b = a + 1 < s->length ? a + 1 : a;
	} else {
		b = a + 1 == s->length ? s->loop_start : a + 1;
	}

	float t = (float)fraction / (float)MICROSTEPS;
	float u = point_to_float(s->data[a]);
	float v = point_to_float(s->data[b]);
	return u + t * (v - u);
}

/* The internal fadeout maximum is 32768; a fresh sustained voice uses
   MAX_FADEOUT_VOLUME-1, i.e. 32767. */
#define MAX_FADE_VOLUME 32767

static void pan_gains(uint8_t panning, float* left_gain, float* right_gain) {
	int32_t base = 64;
	base *= 64; /* volume envelope */
	base *= MAX_FADE_VOLUME;
	base /= 4;
	base *= 64; /* global volume */
	float volume = (float)base / 2147483647.f;
	*left_gain = volume * sqrtf((float)(256 - panning) / 256.f);
	*right_gain = volume * sqrtf((float)panning / 256.f);
}

static struct sf_file make_play_fixture(unsigned effect, uint8_t param) {
	static int16_t left[320], right[320];
	for(unsigned i = 0; i < 320; ++i) {
		int li = (int)(i % 17) - 8;
		int ri = (int)(i % 13) - 6;
		left[i] = (int16_t)(256 * li * 7);
		right[i] = (int16_t)(-256 * ri * 9);
	}

	struct sf_file f;
	sf_init(&f, 1, false);
	f.samples[0] = (struct sf_sample){
		.declared_length = 320, .loop_start = 64, .loop_end = 288,
		.flags = 1 | 2, .volume = 64, .left = left, .right = right
	};
	f.events[0] = (struct sf_event){0, 0x40, 1, (uint8_t)effect, param};
	f.num_events = 1;
	return f;
}

static uint32_t query_step(xm_context_t* ctx) {
	float hz = xm_get_frequency_of_channel(ctx, 1);
	uint32_t rate = xm_get_sample_rate(ctx);
	return (uint32_t)llround((double)hz * (double)MICROSTEPS / rate);
}

static void check_expected_frames(struct check_result* r, xm_context_t* ctx,
                                  unsigned sample_index, uint8_t panning,
                                  unsigned extra_offset_frames,
                                  unsigned capture_frames) {
	uint32_t length;
	xm_sample_point_t* l = xm_get_sample_waveform_channel(ctx, sample_index, 0, &length);
	xm_sample_point_t* rr = xm_get_sample_waveform_channel(ctx, sample_index, 1, NULL);
	Z_CHECK(r, l && rr && length == 288);

	struct ref_plane planes[2] = {
		{l, length, 64, 224},
		{rr, length, 64, 224}
	};
	uint32_t step = query_step(ctx);
	Z_CHECK(r, step > 0 && step < MICROSTEPS);

	float gl, gr;
	pan_gains(panning, &gl, &gr);

	unsigned start = ramp_is_enabled() ? WARM_FRAMES - 255 : WARM_FRAMES;
	uint64_t position = (uint64_t)extra_offset_frames * MICROSTEPS
	                    + (uint64_t)start * step;

	float* out_l = malloc(capture_frames * sizeof(float));
	float* out_r = malloc(capture_frames * sizeof(float));
	Z_CHECK(r, out_l && out_r);
	xm_generate_samples_noninterleaved(ctx, out_l, out_r, (uint16_t)capture_frames);

	for(unsigned i = 0; i < capture_frames; ++i) {
		float expected_l = ref_sample(&planes[0], position);
		float expected_r = ref_sample(&planes[1], position);
		expected_l *= 0.25f;
		expected_r *= 0.25f;
		expected_l *= gl;
		expected_r *= gr;
		Z_CHECK(r, fabsf(out_l[i] - expected_l) < 2.0e-6f);
		Z_CHECK(r, fabsf(out_r[i] - expected_r) < 2.0e-6f);
		position += step;
	}
	free(out_l);
	free(out_r);
}

static void gen_chunked_interleaved(xm_context_t* ctx, float* out,
                                    unsigned frames, const unsigned* chunks,
                                    unsigned num_chunks) {
	unsigned done = 0, ci = 0;
	while(done < frames) {
		unsigned n = chunks[ci++ % num_chunks];
		if(n > frames - done) n = frames - done;
		xm_generate_samples(ctx, out + 2 * done, (uint16_t)n);
		done += n;
	}
}

void check_play(struct check_result* r) {
	/* Identical planes must reproduce the old mono path exactly, with no
	   extra center-pan compensation. */
	static int16_t equal_wave[64];
	for(unsigned i = 0; i < 64; ++i) {
		equal_wave[i] = (int16_t)(256 * ((int)(i % 19) - 9) * 5);
	}
	struct sf_file stereo_equal, mono_equal;
	sf_init(&stereo_equal, 1, false);
	stereo_equal.samples[0] = (struct sf_sample){
		.declared_length = 64, .loop_start = 0, .loop_end = 64,
		.flags = 1 | 2, .left = equal_wave, .right = equal_wave
	};
	stereo_equal.events[0] = (struct sf_event){0, 0x40, 1, 24, 64};
	stereo_equal.num_events = 1;
	sf_init(&mono_equal, 1, false);
	mono_equal.samples[0] = (struct sf_sample){
		.declared_length = 64, .loop_start = 0, .loop_end = 64,
		.flags = 1, .left = equal_wave
	};
	mono_equal.events[0] = stereo_equal.events[0];
	mono_equal.num_events = 1;
	xm_context_t* equal_stereo_ctx = sf_load(r, &stereo_equal);
	xm_context_t* equal_mono_ctx = sf_load(r, &mono_equal);
	Z_CHECK(r, equal_stereo_ctx && equal_mono_ctx);
	if(equal_stereo_ctx && equal_mono_ctx) {
		float stereo_out[512], mono_out[512];
		xm_generate_samples(equal_stereo_ctx, stereo_out, 256);
		xm_generate_samples(equal_mono_ctx, mono_out, 256);
		Z_CHECK(r, memcmp(stereo_out, mono_out, sizeof(stereo_out)) == 0);
	}
	sf_free(equal_stereo_ctx);
	sf_free(equal_mono_ctx);

	/* Exact independent expansion, interpolation and forward loop seam. */
	struct sf_file f = make_play_fixture(24, 64); /* X40: center pan */
	xm_context_t* ctx = sf_load(r, &f);
	Z_CHECK(r, ctx != NULL);
	if(ctx) {
		float warm[2 * WARM_FRAMES];
		xm_generate_samples(ctx, warm, WARM_FRAMES);
		check_expected_frames(r, ctx, 0, 128, 0, 96);
		sf_free(ctx);
	}

	/* O01 starts at frame 256; enough captured frames cross the loop. */
	f = make_play_fixture(15, 1);
	ctx = sf_load(r, &f);
	Z_CHECK(r, ctx != NULL);
	if(ctx) {
		float warm[2 * WARM_FRAMES];
		xm_generate_samples(ctx, warm, WARM_FRAMES);
		/* Default PCM channel 0 panning is ST3 0x33 -> 51/256. */
		check_expected_frames(r, ctx, 0, 51, 256, 220);
		sf_free(ctx);
	}

	/* The three output layouts and arbitrary chunk boundaries must be the
	   same playback process. */
	const unsigned chunks[] = {1, 7, 113};
	const unsigned total = 512;
	float* canonical = malloc(2 * total * sizeof(float));
	float* chunked = malloc(2 * total * sizeof(float));
	float* ni_l = malloc(total * sizeof(float));
	float* ni_r = malloc(total * sizeof(float));
	float* unmixed = malloc(2 * total * sizeof(float));
	Z_CHECK(r, canonical && chunked && ni_l && ni_r && unmixed);

	if(canonical && chunked && ni_l && ni_r && unmixed) {
		ctx = sf_load(r, &f);
		Z_CHECK(r, ctx != NULL);
		if(ctx) {
			xm_generate_samples(ctx, canonical, (uint16_t)total);
			sf_free(ctx);
		}

		ctx = sf_load(r, &f);
		if(ctx) {
			gen_chunked_interleaved(ctx, chunked, total, chunks, 3);
			Z_CHECK(r, memcmp(canonical, chunked,
			                   2 * total * sizeof(float)) == 0);
			sf_free(ctx);
		}

		ctx = sf_load(r, &f);
		if(ctx) {
			unsigned done = 0, ci = 0;
			while(done < total) {
				unsigned n = chunks[ci++ % 3];
				if(n > total - done) n = total - done;
				xm_generate_samples_noninterleaved(ctx, ni_l + done,
				                                   ni_r + done, (uint16_t)n);
				done += n;
			}
			for(unsigned i = 0; i < total; ++i) {
				chunked[2 * i] = ni_l[i];
				chunked[2 * i + 1] = ni_r[i];
			}
			Z_CHECK(r, memcmp(canonical, chunked,
			                   2 * total * sizeof(float)) == 0);
			sf_free(ctx);
		}

		ctx = sf_load(r, &f);
		if(ctx) {
			unsigned done = 0, ci = 0;
			while(done < total) {
				unsigned n = chunks[ci++ % 3];
				if(n > total - done) n = total - done;
				xm_generate_samples_unmixed(ctx, unmixed + 2 * done,
				                             (uint16_t)n);
				done += n;
			}
			Z_CHECK(r, memcmp(canonical, unmixed,
			                   2 * total * sizeof(float)) == 0);
			sf_free(ctx);
		}
	}
	free(canonical);
	free(chunked);
	free(ni_l);
	free(ni_r);
	free(unmixed);

	/* Q02 retrigger: find the second tick independently, then check that
	   the restarted phase has the same settled expansion as a fresh note. */
	f = make_play_fixture(17, 0x02);
	ctx = sf_load(r, &f);
	Z_CHECK(r, ctx != NULL);
	if(ctx) {
		xm_set_sample_rate(ctx, 48000);
		uint32_t remaining = 0;
		unsigned ticks = 0;
		unsigned retrigger_frame = 0;
		for(unsigned frame = 0; frame < 2048; ++frame) {
			bool ticked = remaining < TICK_SUBSAMPLES;
			if(ticked) {
				++ticks;
				if(ticks == 2) { retrigger_frame = frame; break; }
				uint32_t in_tick = 48000u * (10u * TICK_SUBSAMPLES / 4u)
				                   / 255u;
				remaining = remaining - TICK_SUBSAMPLES + in_tick;
			} else {
				remaining -= TICK_SUBSAMPLES;
			}
		}
		Z_CHECK(r, retrigger_frame > 255 && retrigger_frame < 1000);

		unsigned need = retrigger_frame + WARM_FRAMES + 1;
		float* all = calloc(2 * need, sizeof(float));
		Z_CHECK(r, all != NULL);
		if(all) {
			xm_generate_samples(ctx, all, (uint16_t)need);
			uint32_t length;
			xm_sample_point_t* lp = xm_get_sample_waveform_channel(ctx, 0, 0, &length);
			xm_sample_point_t* rp = xm_get_sample_waveform_channel(ctx, 0, 1, NULL);
			struct ref_plane pl = {lp, length, 64, 224};
			struct ref_plane pr = {rp, length, 64, 224};
			uint32_t step = query_step(ctx);
			float gl, gr;
			pan_gains(51, &gl, &gr);
			unsigned start = ramp_is_enabled() ? 45 : WARM_FRAMES;
			uint64_t pos = (uint64_t)start * step;
			unsigned idx = retrigger_frame + WARM_FRAMES;
			float el = ref_sample(&pl, pos) * 0.25f * gl;
			float er = ref_sample(&pr, pos) * 0.25f * gr;
			Z_CHECK(r, fabsf(all[2 * idx] - el) < 2.0e-6f);
			Z_CHECK(r, fabsf(all[2 * idx + 1] - er) < 2.0e-6f);
			free(all);
		}
		sf_free(ctx);
	}
}

/* ----- ramp group ----- */

static struct sf_file make_handoff_fixture(int kind) {
	static int16_t a_l[16], a_r[16], b_l[16], b_r[16], mono[16], empty[1];
	for(unsigned i = 0; i < 16; ++i) {
		a_l[i] = 12000;
		a_r[i] = -14000;
		b_l[i] = 8000;
		b_r[i] = -4000;
		mono[i] = 6000;
	}

	struct sf_file f;
	unsigned ns = kind == 0 ? 2 : 3;
	sf_init(&f, ns, true);
	f.samples[0] = (struct sf_sample){
		.declared_length = 16, .loop_start = 0, .loop_end = 16,
		.flags = 1 | 2 | 4, .volume = 64, .left = a_l, .right = a_r
	};
	if(kind == 0) {
		/* stereo -> stereo */
		f.samples[1] = (struct sf_sample){
			.declared_length = 16, .loop_start = 0, .loop_end = 16,
			.flags = 1 | 2 | 4, .volume = 64, .left = b_l, .right = b_r
		};
		f.events[0] = (struct sf_event){0, 0x40, 1, 24, 64};
		f.events[1] = (struct sf_event){1, 0x40, 2, 0, 0};
	} else if(kind == 1) {
		/* stereo -> mono */
		f.samples[1] = (struct sf_sample){
			.declared_length = 16, .loop_start = 0, .loop_end = 16,
			.flags = 1 | 4, .volume = 64, .left = mono
		};
		f.events[0] = (struct sf_event){0, 0x40, 1, 24, 64};
		f.events[1] = (struct sf_event){1, 0x40, 2, 0, 0};
	} else {
		/* stereo -> a valid but empty sample */
		f.samples[1] = (struct sf_sample){
			.declared_length = 16, .loop_start = 0, .loop_end = 16,
			.flags = 1 | 4, .volume = 64, .left = mono
		};
		f.samples[2] = (struct sf_sample){
			.declared_length = 0, .flags = 0, .volume = 64, .left = empty
		};
		f.events[0] = (struct sf_event){0, 0x40, 1, 24, 64};
		f.events[1] = (struct sf_event){1, 0x40, 3, 0, 0};
	}
	f.num_events = 2;
	return f;
}

void check_ramp(struct check_result* r) {
	for(int kind = 0; kind < 3; ++kind) {
		struct sf_file f = make_handoff_fixture(kind);
		xm_context_t* ctx = sf_load(r, &f);
		Z_CHECK(r, ctx != NULL);
		if(!ctx) continue;

		float warm[2 * WARM_FRAMES];
		xm_generate_samples(ctx, warm, WARM_FRAMES);
		xm_seek(ctx, 0, 1, 0);

		const unsigned n = 24;
		float out[2 * 24];
		xm_generate_samples(ctx, out, n);

		float gl, gr;
		pan_gains(128, &gl, &gr);
		for(unsigned k = 0; k < n; ++k) {
			float new_l = 0, new_r = 0;
			if(kind == 0) {
				new_l = point_to_float((xm_sample_point_t)8000);
				new_r = point_to_float((xm_sample_point_t)-4000);
			} else if(kind == 1) {
				new_l = new_r = point_to_float((xm_sample_point_t)6000);
			}

			float expected_l, expected_r;
			if(ramp_is_enabled()) {
				float t = (float)k / 255.f;
				float old_l = point_to_float((xm_sample_point_t)12000);
				float old_r = point_to_float((xm_sample_point_t)-14000);
				expected_l = old_l + t * (new_l - old_l);
				expected_r = old_r + t * (new_r - old_r);
			} else {
				expected_l = new_l;
				expected_r = new_r;
			}
			expected_l *= 0.25f * gl;
			expected_r *= 0.25f * gr;

			Z_CHECK(r, fabsf(out[2 * k] - expected_l) < 2.0e-6f);
			Z_CHECK(r, fabsf(out[2 * k + 1] - expected_r) < 2.0e-6f);
			if(kind == 2 && !ramp_is_enabled()) {
				Z_CHECK(r, out[2 * k] == 0.f && out[2 * k + 1] == 0.f);
			}
		}

		/* Identity properties differ by build: ramping preserves the old
		   right tail; without ramping the new voice starts immediately. */
		if(ramp_is_enabled()) {
			Z_CHECK(r, out[1] < 0.f);
			Z_CHECK(r, out[0] > 0.f);
			Z_CHECK(r, fabsf(out[0]) != fabsf(out[1]));
		} else if(kind == 0) {
			Z_CHECK(r, out[1] < 0.f);
			Z_CHECK(r, out[0] > 0.f);
			Z_CHECK(r, fabsf(out[0]) != fabsf(out[1]));
		} else if(kind == 1) {
			Z_CHECK(r, out[0] > 0.f && out[1] > 0.f);
			Z_CHECK(r, out[0] == out[1]);
		} else {
			Z_CHECK(r, out[0] == 0.f && out[1] == 0.f);
		}
		sf_free(ctx);
	}
}

/* ----- formats and snapshots ----- */

static uint32_t xmif_sample_offset(const char* saved) {
	uint32_t header = (uint32_t)(uint8_t)saved[0] * 8u;
	uint16_t patterns = (uint16_t)((uint8_t)saved[0x14]
	                               | ((uint8_t)saved[0x15] << 8));
	uint32_t rows = (uint32_t)(uint8_t)saved[0x0C]
	                | ((uint32_t)(uint8_t)saved[0x0D] << 8)
	                | ((uint32_t)(uint8_t)saved[0x0E] << 16)
	                | ((uint32_t)(uint8_t)saved[0x0F] << 24);
	unsigned channels = (uint8_t)saved[0x19];
	unsigned instruments = (uint8_t)saved[0x18];
	unsigned pattern_stride = (uint8_t)saved[0x20] * 8u;
	unsigned slot_stride = (uint8_t)saved[0x21] * 8u;
	unsigned instrument_stride = (uint8_t)saved[0x22] * 8u;
	return header + patterns * pattern_stride
	       + rows * channels * slot_stride
	       + instruments * instrument_stride;
}

void check_formats(struct check_result* r) {
	static int16_t st_l[20], st_r[20], mono[11];
	for(unsigned i = 0; i < 20; ++i) {
		st_l[i] = (int16_t)(1000 * (i + 1));
		st_r[i] = (int16_t)(-1100 * (i + 1));
	}
	for(unsigned i = 0; i < 11; ++i) mono[i] = (int16_t)(2000 - 400 * (int)i);

	struct sf_file f;
	sf_init(&f, 2, true);
	f.samples[0] = (struct sf_sample){
		.declared_length = 20, .flags = 2 | 4, .volume = 64,
		.left = st_l, .right = st_r
	};
	f.samples[1] = (struct sf_sample){
		.declared_length = 11, .flags = 4, .volume = 64, .left = mono
	};
	f.events[0] = (struct sf_event){0, 0x40, 1, 24, 64};
	f.num_events = 1;

	xm_context_t* ctx = sf_load(r, &f);
	Z_CHECK(r, ctx != NULL);
	if(!ctx) return;

	uint32_t save_size = xm_save_size(ctx);
	char* saved = malloc((size_t)save_size + 8);
	Z_CHECK(r, saved != NULL);
	if(saved) {
		memset(saved + save_size, 0xA5, 8);
		xm_save_context(ctx, saved);
		Z_CHECK(r, memcmp(saved + save_size,
		                  "\xA5\xA5\xA5\xA5\xA5\xA5\xA5\xA5", 8) == 0);
		Z_CHECK(r, (uint8_t)saved[1] == 1);
		uint32_t scalar_points;
		memcpy(&scalar_points, saved + 0x10, 4);
		Z_CHECK(r, scalar_points == 40 + 11);

		uint32_t so = xmif_sample_offset(saved);
		Z_CHECK(r, ((uint8_t)saved[so + 0x0C] & 2) != 0);
		Z_CHECK(r, ((uint8_t)saved[so + 0x18 + 0x0C] & 2) == 0);

		xm_prescan_data_t* scan = malloc(XM_PRESCAN_DATA_SIZE);
		Z_CHECK(r, scan != NULL);
		if(scan) {
			Z_CHECK(r, xm_prescan_module(saved, save_size, scan));
			Z_CHECK(r, xm_size_for_context(scan) > 0);
			void* pool = malloc(xm_size_for_context(scan));
			xm_context_t* reloaded = pool
				? xm_create_context(pool, scan, saved, save_size) : NULL;
			Z_CHECK(r, reloaded != NULL);
			if(reloaded) {
				Z_CHECK(r, xm_get_sample_channels(reloaded, 0) == 2);
				Z_CHECK(r, xm_get_sample_channels(reloaded, 1) == 1);
				uint32_t ll = 0, rl = 0;
				xm_sample_point_t* lp = xm_get_sample_waveform_channel(reloaded, 0, 0, &ll);
				xm_sample_point_t* rp = xm_get_sample_waveform_channel(reloaded, 0, 1, &rl);
				Z_CHECK(r, ll == 20 && rl == 20);
				Z_CHECK(r, abs((int)lp[3] - (int)st_l[3]) <= 1);
				Z_CHECK(r, abs((int)rp[7] - (int)st_r[7]) <= 1);

				/* Edits to the returned stable plane are audible. */
				rp[0] = 32000;
				uint32_t edit_len;
				xm_sample_point_t* again = xm_get_sample_waveform_channel(reloaded, 0, 1, &edit_len);
				Z_CHECK(r, again == rp && edit_len == 20 && again[0] == 32000);

				unsigned edit_frames = ramp_is_enabled() ? 220 : 1;
				float edit_l[220], edit_r[220];
				xm_generate_samples_noninterleaved(
					reloaded, edit_l, edit_r, (uint16_t)edit_frames);
				Z_CHECK(r, edit_r[edit_frames - 1] > 0.05f);
			}
			free(pool);
			free(scan);
		}
		free(saved);
	}

	/* Mono-only modules remain LIBXMIF version 0. */
	struct sf_file mf;
	sf_init(&mf, 1, false);
	static int16_t mono8[16];
	for(unsigned i = 0; i < 16; ++i) mono8[i] = (int16_t)(256 * (i - 8));
	mf.samples[0] = (struct sf_sample){
		.declared_length = 16, .flags = 1, .left = mono8
	};
	xm_context_t* mono_ctx = sf_load(r, &mf);
	Z_CHECK(r, mono_ctx != NULL);
	if(mono_ctx) {
		uint32_t sz = xm_save_size(mono_ctx);
		char* v0 = malloc(sz);
		Z_CHECK(r, v0 != NULL);
		if(v0) {
			xm_save_context(mono_ctx, v0);
			Z_CHECK(r, (uint8_t)v0[1] == 0);
			Z_CHECK(r, ((uint8_t)v0[xmif_sample_offset(v0) + 0x0C] & 2) == 0);

			xm_prescan_data_t* scan = malloc(XM_PRESCAN_DATA_SIZE);
			if(scan && xm_prescan_module(v0, sz, scan)) {
				void* pool = malloc(xm_size_for_context(scan));
				xm_context_t* again = pool
					? xm_create_context(pool, scan, v0, sz) : NULL;
				Z_CHECK(r, again != NULL);
				if(again) {
					uint32_t sz2 = xm_save_size(again);
					Z_CHECK(r, sz2 == sz);
					char* v0b = malloc(sz2);
					Z_CHECK(r, v0b != NULL);
					if(v0b) {
						xm_save_context(again, v0b);
						Z_CHECK(r, memcmp(v0, v0b, sz) == 0);
						free(v0b);
					}
				}
				free(pool);
			}
			free(scan);
			free(v0);
		}
		sf_free(mono_ctx);
	}

	/* Reset continues with the same sound as a fresh context. */
	xm_set_sample_rate(ctx, 48000);
	float before[128];
	xm_generate_samples(ctx, before, 64);
	xm_reset_context(ctx);
	xm_set_sample_rate(ctx, 48000);
	float after[128];
	xm_generate_samples(ctx, after, 64);
	struct sf_file fresh = f;
	xm_context_t* reset_ref = sf_load(r, &fresh);
	Z_CHECK(r, reset_ref != NULL);
	if(reset_ref) {
		float expected[128];
		xm_generate_samples(reset_ref, expected, 64);
		Z_CHECK(r, memcmp(after, expected, sizeof(expected)) == 0);
		sf_free(reset_ref);
	}

	/* Half-tick dump: source continuation and restored image match. */
	xm_reset_context(ctx);
	xm_set_sample_rate(ctx, 48000);
	const unsigned half_tick = 235;
	float half[2 * 235];
	xm_generate_samples(ctx, half, half_tick);

	uint32_t dump_size = xm_dump_size(ctx);
	char* dump = malloc(dump_size);
	xm_context_t* control = sf_load(r, &f);
	Z_CHECK(r, dump != NULL && control != NULL);
	if(dump && control) {
		float control_half[2 * 235];
		xm_generate_samples(control, control_half, half_tick);
		Z_CHECK(r, memcmp(half, control_half, sizeof(half)) == 0);

		xm_dump_context(ctx, dump);
		const unsigned tail_frames = 600;
		float* source_tail = malloc(2 * tail_frames * sizeof(float));
		float* control_tail = malloc(2 * tail_frames * sizeof(float));
		float* restored_tail = malloc(2 * tail_frames * sizeof(float));
		Z_CHECK(r, source_tail && control_tail && restored_tail);

		if(source_tail && control_tail && restored_tail) {
			for(unsigned i = 0; i < tail_frames; ++i) {
				xm_generate_samples(ctx, source_tail + 2 * i, 1);
				xm_generate_samples(control, control_tail + 2 * i, 1);
			}
			Z_CHECK(r, memcmp(source_tail, control_tail,
			                   2 * tail_frames * sizeof(float)) == 0);

			xm_context_t* restored = xm_restore_context(dump);
			Z_CHECK(r, restored != NULL);
			for(unsigned i = 0; i < tail_frames; ++i) {
				xm_generate_samples(restored, restored_tail + 2 * i, 1);
			}
			Z_CHECK(r, memcmp(restored_tail, control_tail,
			                   2 * tail_frames * sizeof(float)) == 0);
		}
		free(source_tail);
		free(control_tail);
		free(restored_tail);
	}
	free(dump);
	sf_free(control);
	sf_free(ctx);
}
