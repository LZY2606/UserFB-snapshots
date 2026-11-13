/* libFLAC bounded look-ahead blocksize search tests.
 *
 * This translation unit provides target_cases/target_count and no main();
 * task_support/driver.c drives the cases.  All materials, encoders and
 * decoders below are created purely in C from the public and private
 * libFLAC headers.
 */
#include "cases.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FLAC/stream_encoder.h"
#include "FLAC/stream_decoder.h"
#include "FLAC/format.h"
#include "FLAC/metadata.h"
#include "private/bitwriter.h"
#include "private/stream_encoder_framing.h"
#ifdef FLAC__TEST_BLOCKSIZE_SEARCH
#include "private/stream_encoder.h"
#endif

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static uint32_t rng_state = 0x12345678u;
static uint32_t xs32(void)
{
	uint32_t x = rng_state;
	x ^= x << 13; x ^= x >> 17; x ^= x << 5;
	rng_state = x;
	return x;
}

static uint32_t utf8_len(uint64_t v)
{
	if(v < 0x80) return 1;
	if(v < 0x800) return 2;
	if(v < 0x10000) return 3;
	if(v < 0x200000) return 4;
	if(v < 0x4000000) return 5;
	return 6;
}

/* ------------------------------------------------------------------ */
/* in-memory encoder sink                                              */
/* ------------------------------------------------------------------ */

typedef struct {
	FLAC__byte *data;
	size_t len;
	size_t cap;
	size_t pos;            /* artificial cursor for seek/tell */
	uint32_t audio_frames; /* number of samples>0 writes delivered */
	uint32_t fail_after;   /* 0 = never fail; otherwise 1-based audio frame */
	FLAC__bool failed;
	struct { size_t offset; size_t len; uint32_t samples; uint32_t frame_no; } ranges[256];
	FLAC__StreamMetadata_StreamInfo last_info;
	FLAC__bool got_info;
} Sink;

static void sink_init(Sink *s)
{
	memset(s, 0, sizeof(*s));
	s->fail_after = 0;
}

static void sink_free(Sink *s)
{
	free(s->data);
	memset(s, 0, sizeof(*s));
}

static FLAC__StreamEncoderWriteStatus sink_write(const FLAC__StreamEncoder *e, const FLAC__byte *p, size_t n, uint32_t samples, uint32_t frame, void *ud)
{
	Sink *s = (Sink *)ud;
	(void)e;
	if(samples > 0) {
		s->audio_frames++;
		if(s->fail_after != 0 && s->audio_frames == s->fail_after) {
			s->failed = true;
			return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
		}
		if(s->audio_frames <= 256) {
			s->ranges[s->audio_frames - 1].offset = s->pos;
			s->ranges[s->audio_frames - 1].len = n;
			s->ranges[s->audio_frames - 1].samples = samples;
			s->ranges[s->audio_frames - 1].frame_no = frame;
		}
	}
	if(s->pos + n > s->cap) {
		size_t ncap = s->cap ? s->cap * 2 : 4096;
		FLAC__byte *grown;
		while(ncap < s->pos + n) ncap *= 2;
		grown = (FLAC__byte *)realloc(s->data, ncap);
		if(!grown) return FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
		s->data = grown;
		s->cap = ncap;
	}
	memcpy(s->data + s->pos, p, n);
	if(s->pos + n > s->len) s->len = s->pos + n;
	s->pos += n;
	return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

static FLAC__StreamEncoderSeekStatus sink_seek(const FLAC__StreamEncoder *e, FLAC__uint64 off, void *ud)
{
	Sink *s = (Sink *)ud;
	(void)e;
	if(off > s->cap) return FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
	s->pos = (size_t)off;
	return FLAC__STREAM_ENCODER_SEEK_STATUS_OK;
}

static FLAC__StreamEncoderTellStatus sink_tell(const FLAC__StreamEncoder *e, FLAC__uint64 *off, void *ud)
{
	Sink *s = (Sink *)ud;
	(void)e;
	*off = s->pos;
	return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

static void sink_metadata(const FLAC__StreamEncoder *e, const FLAC__StreamMetadata *m, void *ud)
{
	Sink *s = (Sink *)ud;
	(void)e;
	if(m->type == FLAC__METADATA_TYPE_STREAMINFO) {
		s->last_info = m->data.stream_info;
		s->got_info = true;
	}
}

/* ------------------------------------------------------------------ */
/* in-memory decoder source                                            */
/* ------------------------------------------------------------------ */

typedef struct {
	const FLAC__byte *data;
	size_t len;
	size_t pos;
	FLAC__int32 *pcm;       /* interleaved reference */
	uint32_t channels;
	uint32_t got_samples;
	FLAC__bool had_error;
} DecSrc;

static FLAC__StreamDecoderReadStatus dec_read(const FLAC__StreamDecoder *d, FLAC__byte *buf, size_t *n, void *ud)
{
	DecSrc *z = (DecSrc *)ud;
	size_t avail;
	(void)d;
	avail = z->len - z->pos;
	if(avail == 0) { *n = 0; return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM; }
	if(*n > avail) *n = avail;
	memcpy(buf, z->data + z->pos, *n);
	z->pos += *n;
	return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

static FLAC__StreamDecoderWriteStatus dec_write(const FLAC__StreamDecoder *d, const FLAC__Frame *frame, const FLAC__int32 * const pcm[], void *ud)
{
	DecSrc *z = (DecSrc *)ud;
	uint32_t i, c;
	(void)d;
	for(i = 0; i < frame->header.blocksize; i++) {
		if(z->got_samples + i >= 0x7fffffffu) { z->had_error = true; return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT; }
		for(c = 0; c < z->channels; c++) {
			if(pcm[c][i] != z->pcm[(z->got_samples + i) * z->channels + c]) {
				z->had_error = true;
				return FLAC__STREAM_DECODER_WRITE_STATUS_ABORT;
			}
		}
	}
	z->got_samples += frame->header.blocksize;
	return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

static void dec_error(const FLAC__StreamDecoder *d, FLAC__StreamDecoderErrorStatus status, void *ud)
{
	DecSrc *z = (DecSrc *)ud;
	(void)d; (void)status;
	z->had_error = true;
}

static FLAC__bool decode_and_compare(const Sink *sink, const FLAC__int32 *ref_interleaved, uint32_t total_samples, uint32_t channels)
{
	FLAC__StreamDecoder *d = FLAC__stream_decoder_new();
	DecSrc z;
	FLAC__bool md5_enabled, ok = false;
	if(!d) return false;
	z.data = sink->data; z.len = sink->len; z.pos = 0;
	z.pcm = (FLAC__int32 *)ref_interleaved; z.channels = channels;
	z.got_samples = 0; z.had_error = false;
	if(FLAC__stream_decoder_set_md5_checking(d, true)
		&& FLAC__stream_decoder_init_stream(d, dec_read, 0, 0, 0, 0, dec_write, 0, dec_error, &z) == FLAC__STREAM_DECODER_INIT_STATUS_OK
		&& FLAC__stream_decoder_process_until_end_of_stream(d)) {
		/* MD5 checking must have been active for the whole decode; query
		 * before finish(), which resets the decoder. */
		md5_enabled = FLAC__stream_decoder_get_md5_checking(d);
		ok = md5_enabled && !z.had_error && z.got_samples == total_samples;
	}
	FLAC__stream_decoder_finish(d);
	FLAC__stream_decoder_delete(d);
	return ok;
}

/* ------------------------------------------------------------------ */
/* frame header parsing (independent of the encoder)                   */
/* ------------------------------------------------------------------ */

typedef struct {
	FLAC__bool variable;
	uint64_t first_sample;
	uint32_t blocksize;
} ParsedHeader;

static FLAC__bool parse_header(const FLAC__byte *p, size_t len, ParsedHeader *out)
{
	size_t pos = 4;
	unsigned bs_code, lead, i;
	uint64_t v;
	if(len < 5) return false;
	if(p[0] != 0xFF || (p[1] & 0xFE) != 0xF8) return false;
	out->variable = (p[1] & 1) == 1;
	bs_code = p[2] >> 4;
	/* UTF-8 coded frame/sample number */
	lead = p[pos++];
	if(lead < 0x80) v = lead;
	else {
		unsigned nb = 0;
		unsigned x = lead;
		while(x & 0x80) { nb++; x <<= 1; }
		if(nb < 2 || nb > 7 || pos + (nb - 1) > len) return false;
		v = lead & ((1u << (8 - nb)) - 1);
		for(i = 1; i < nb; i++) {
			unsigned b = p[pos++];
			if((b & 0xC0) != 0x80) return false;
			v = (v << 6) | (b & 0x3F);
		}
	}
	out->first_sample = v;
	switch(bs_code) {
		case 1: out->blocksize = 192; break;
		case 8: out->blocksize = 256; break;
		case 9: out->blocksize = 512; break;
		case 10: out->blocksize = 1024; break;
		case 11: out->blocksize = 2048; break;
		case 12: out->blocksize = 4096; break;
		case 6:
			if(pos >= len) return false;
			out->blocksize = p[pos] + 1;
			pos += 1;
			break;
		case 7:
			if(pos + 2 > len) return false;
			out->blocksize = ((uint32_t)p[pos] << 8 | p[pos + 1]) + 1;
			pos += 2;
			break;
		default: return false;
	}
	return pos <= len;
}

/* ================================================================== */
/* optimizer group                                                     */
/* ================================================================== */

/* Encode exactly one fixed-blocksize frame with the ordinary fixed
 * mode and return its full audio-frame byte length.  The QLP precision
 * is forced to the shared, already-resolved value. */
static FLAC__bool fixed_frame_cost(uint32_t channels, uint32_t bps, uint32_t level, uint32_t blocksize, uint32_t qlp, const FLAC__int32 *planar[], size_t *cost)
{
	FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
	Sink sink;
	FLAC__bool ok = false;
	sink_init(&sink);
	if(!e) goto done;
	if(!FLAC__stream_encoder_set_channels(e, channels)) goto done;
	if(!FLAC__stream_encoder_set_bits_per_sample(e, bps)) goto done;
	if(!FLAC__stream_encoder_set_sample_rate(e, 48000)) goto done;
	if(!FLAC__stream_encoder_set_compression_level(e, level)) goto done;
	if(!FLAC__stream_encoder_set_blocksize(e, blocksize)) goto done;
	if(!FLAC__stream_encoder_set_qlp_coeff_precision(e, qlp)) goto done;
	if(FLAC__stream_encoder_init_stream(e, sink_write, 0, 0, 0, &sink) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) goto done;
	if(!FLAC__stream_encoder_process(e, planar, blocksize)) goto done;
	if(!FLAC__stream_encoder_finish(e)) goto done;
	if(sink.audio_frames != 1) goto done;
	*cost = sink.ranges[0].len;
	ok = true;
done:
	if(e) FLAC__stream_encoder_delete(e);
	sink_free(&sink);
	return ok;
}

static uint32_t resolve_qlp(uint32_t bps, uint32_t level, uint32_t maximum)
{
	FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
	Sink sink;
	uint32_t qlp = 0;
	sink_init(&sink);
	FLAC__stream_encoder_set_channels(e, 2);
	FLAC__stream_encoder_set_bits_per_sample(e, bps);
	FLAC__stream_encoder_set_sample_rate(e, 48000);
	FLAC__stream_encoder_set_compression_level(e, level);
	FLAC__stream_encoder_set_blocksize(e, maximum);
	if(FLAC__stream_encoder_init_stream(e, sink_write, 0, 0, 0, &sink) == FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
		qlp = FLAC__stream_encoder_get_qlp_coeff_precision(e);
		FLAC__stream_encoder_finish(e);
	}
	FLAC__stream_encoder_delete(e);
	sink_free(&sink);
	return qlp;
}

/* Build a real variable/fixed header with the library framing routine
 * and return its byte length. */
static size_t header_bytes(FLAC__FrameNumberType type, uint64_t number, uint32_t blocksize, uint32_t channels, uint32_t bps)
{
	FLAC__FrameHeader h;
	FLAC__BitWriter *bw = FLAC__bitwriter_new();
	const FLAC__byte *buf;
	size_t bytes = 0;
	if(!bw || !FLAC__bitwriter_init(bw)) { if(bw) FLAC__bitwriter_delete(bw); return 0; }
	memset(&h, 0, sizeof(h));
	h.blocksize = blocksize;
	h.sample_rate = 48000;
	h.channels = channels;
	h.channel_assignment = FLAC__CHANNEL_ASSIGNMENT_INDEPENDENT;
	h.bits_per_sample = bps;
	h.number_type = type;
	if(type == FLAC__FRAME_NUMBER_TYPE_FRAME_NUMBER) h.number.frame_number = (uint32_t)number;
	else h.number.sample_number = number;
	if(FLAC__frame_add_header(&h, bw) && FLAC__bitwriter_get_buffer(bw, &buf, &bytes))
		FLAC__bitwriter_release_buffer(bw);
	FLAC__bitwriter_delete(bw);
	return bytes;
}

/* Materials for one optimizer window: stereo interleaved + planar. */
typedef enum { MAT_SILENCE, MAT_ABRUPT, MAT_ENTROPY, MAT_CORRELATED, MAT_EXTREME24 } Material;

static void build_material(Material m, FLAC__int32 *il, FLAC__int32 *pl0, FLAC__int32 *pl1, uint32_t n, uint32_t bps)
{
	uint32_t i;
	int32_t hi16 = 32767, lo16 = -32768;
	int32_t hi24 = 8388607, lo24 = -8388608;
	for(i = 0; i < n; i++) {
		int32_t l, r;
		switch(m) {
			case MAT_SILENCE: l = r = 0; break;
			case MAT_ABRUPT:
				if(i < n / 2) { l = r = 0; }
				else { l = (xs32() & 1u) ? hi16 : lo16; r = ((int32_t)xs32()) >> 16; }
				break;
			case MAT_ENTROPY:
				l = (int32_t)xs32() >> (32 - (int)bps);
				r = (int32_t)xs32() >> (32 - (int)bps);
				break;
			case MAT_CORRELATED:
				l = (int32_t)xs32() >> (bps == 24 ? 8 : 16);
				r = l + (int32_t)(xs32() % 5) - 2;
				break;
			default: /* MAT_EXTREME24 */
				l = (i & 1u) ? hi24 : lo24;
				r = (i & 2u) ? hi24 : lo24;
				break;
		}
		/* keep every sample within the stream's sample width */
		{
			const int32_t vmax = (int32_t)(((uint32_t)1 << (bps - 1)) - 1);
			const int32_t vmin = -vmax - 1;
			if(l > vmax) l = vmax; else if(l < vmin) l = vmin;
			if(r > vmax) r = vmax; else if(r < vmin) r = vmin;
		}
		il[2*i] = pl0[i] = l;
		il[2*i+1] = pl1[i] = r;
	}
}

#define OPT_MIN 64u
#define OPT_MAX 512u
#define OPT_WIDTH (OPT_MAX / OPT_MIN)   /* 8 minimum slots */
#define OPT_KINDS 4u                    /* 64,128,256,512 */

static const uint32_t opt_kinds[OPT_KINDS] = { 64, 128, 256, 512 };
static const uint32_t opt_slot_len[OPT_KINDS] = { 1, 2, 4, 8 };

/* Independently score every feasible frame of one window using the
 * ordinary fixed encoder, then adjust each frame's header length for the
 * absolute sample number it would carry in variable blocking. */
static FLAC__bool score_window(Material m, uint32_t bps, uint32_t level, uint32_t qlp, uint64_t base_cost[OPT_WIDTH][OPT_KINDS])
{
	FLAC__int32 *il = (FLAC__int32 *)calloc(OPT_MAX * 2, sizeof(FLAC__int32));
	FLAC__int32 *p0 = (FLAC__int32 *)calloc(OPT_MAX, sizeof(FLAC__int32));
	FLAC__int32 *p1 = (FLAC__int32 *)calloc(OPT_MAX, sizeof(FLAC__int32));
	uint32_t start, k;
	FLAC__bool ok = true;
	build_material(m, il, p0, p1, OPT_MAX, bps);
	for(start = 0; start < OPT_WIDTH && ok; start++) {
		for(k = 0; k < OPT_KINDS; k++) {
			uint32_t block = opt_kinds[k];
			uint32_t slots = opt_slot_len[k];
			const FLAC__int32 *planar[2];
			FLAC__int32 *s0, *s1;
			size_t fixed_cost;
			int64_t delta;
			if(start + slots > OPT_WIDTH) { base_cost[start][k] = UINT64_MAX; continue; }
			s0 = p0 + start * OPT_MIN;
			s1 = p1 + start * OPT_MIN;
			planar[0] = s0; planar[1] = s1;
			if(!fixed_frame_cost(2, bps, level, block, qlp, planar, &fixed_cost)) { ok = false; break; }
			/* fixed header carries frame number 0 (1 UTF-8 byte); the
			 * real frame carries absolute first sample (start*OPT_MIN). */
			delta = (int64_t)utf8_len((uint64_t)start * OPT_MIN) - 1;
			base_cost[start][k] = fixed_cost + delta;
		}
	}
	free(il); free(p0); free(p1);
	return ok;
}

/* Recursive brute force over every legal partition. */
static void brute_partition(const uint64_t cost[OPT_WIDTH][OPT_KINDS], uint32_t pos, uint64_t acc, uint32_t frames, const uint32_t *seq, uint64_t *best_cost, uint32_t *best_frames, uint32_t *best_seq)
{
	uint32_t k;
	if(pos == OPT_WIDTH) {
		uint32_t i;
		FLAC__bool better = acc < *best_cost
			|| (acc == *best_cost && frames < *best_frames);
		if(!better && acc == *best_cost && frames == *best_frames) {
			for(i = 0; i < frames; i++)
				if(seq[i] != best_seq[i]) { better = seq[i] > best_seq[i]; break; }
		}
		if(better) {
			*best_cost = acc; *best_frames = frames;
			memcpy(best_seq, seq, sizeof(uint32_t) * frames);
		}
		return;
	}
	for(k = 0; k < OPT_KINDS; k++) {
		uint32_t slots = opt_slot_len[k];
		uint32_t next_seq[OPT_WIDTH];
		if(pos + slots > OPT_WIDTH || cost[pos][k] == UINT64_MAX) continue;
		memcpy(next_seq, seq, sizeof(uint32_t) * frames);
		next_seq[frames] = opt_kinds[k];
		brute_partition(cost, pos + slots, acc + cost[pos][k], frames + 1, next_seq, best_cost, best_frames, best_seq);
	}
}

/* Step-local greedy: smallest current frame bytes per sample, ties take
 * the longer frame. */
static void greedy_partition(const uint64_t cost[OPT_WIDTH][OPT_KINDS], uint64_t *greedy_cost, uint32_t *greedy_frames, uint32_t *greedy_seq)
{
	uint32_t pos = 0, frames = 0;
	uint64_t acc = 0;
	while(pos < OPT_WIDTH) {
		uint32_t best_k = 0, k;
		for(k = 0; k < OPT_KINDS; k++) {
			uint32_t slots = opt_slot_len[k];
			double ratio_best, ratio_k;
			if(pos + slots > OPT_WIDTH || cost[pos][k] == UINT64_MAX) continue;
			ratio_best = (double)cost[pos][best_k] / opt_kinds[best_k];
			ratio_k = (double)cost[pos][k] / opt_kinds[k];
			if(ratio_k < ratio_best || (ratio_k == ratio_best && opt_kinds[k] > opt_kinds[best_k]))
				best_k = k;
		}
		greedy_seq[frames++] = opt_kinds[best_k];
		acc += cost[pos][best_k];
		pos += opt_slot_len[best_k];
	}
	*greedy_cost = acc;
	*greedy_frames = frames;
}

static void run_optimizer(Material m, uint32_t bps, FLAC__bool *saw_non_all_long)
{
	uint64_t cost[OPT_WIDTH][OPT_KINDS];
	uint64_t best_cost = UINT64_MAX, greedy_cost = 0, all_long_cost;
	uint32_t best_frames = 0, greedy_frames = 0, qlp;
	uint32_t best_seq[OPT_WIDTH], greedy_seq[OPT_WIDTH], all_long_seq[OPT_WIDTH];
	uint32_t i;
	const uint32_t level = 5;

	qlp = resolve_qlp(bps, level, OPT_MAX);
	TARGET_CHECK(qlp >= FLAC__MIN_QLP_COEFF_PRECISION);
	TARGET_CHECK(score_window(m, bps, level, qlp, cost));

	memset(best_seq, 0, sizeof(best_seq));
	brute_partition(cost, 0, 0, 0, (const uint32_t[OPT_WIDTH]){0}, &best_cost, &best_frames, best_seq);
	TARGET_CHECK(best_cost != UINT64_MAX);

	greedy_partition(cost, &greedy_cost, &greedy_frames, greedy_seq);
	TARGET_CHECK(greedy_frames >= best_frames);
	TARGET_CHECK(greedy_cost >= best_cost);

	/* all-long reference: a single maximum-length frame */
	all_long_cost = cost[0][OPT_KINDS - 1];
	for(i = 0; i < OPT_WIDTH; i++) all_long_seq[i] = 0;
	all_long_seq[0] = OPT_MAX;
	TARGET_CHECK(all_long_cost != UINT64_MAX);
	TARGET_CHECK(best_cost <= all_long_cost);
	if(best_cost < all_long_cost || best_frames != 1)
		*saw_non_all_long = true;
}

static void case_opt_16bit_materials(void)
{
	FLAC__bool saw = false;
	size_t fixed_h, var_h;
	/* Header length change across the UTF-8 boundaries 127/128 and
	 * 2047/2048, measured with the real framing routine. */
	fixed_h = header_bytes(FLAC__FRAME_NUMBER_TYPE_FRAME_NUMBER, 0, OPT_MIN, 2, 16);
	var_h = header_bytes(FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER, 127, OPT_MIN, 2, 16);
	TARGET_CHECK(var_h == fixed_h);                                   /* 1-byte number */
	var_h = header_bytes(FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER, 128, OPT_MIN, 2, 16);
	TARGET_CHECK(var_h == fixed_h + 1);                               /* crosses into 2 bytes */
	var_h = header_bytes(FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER, 2047, OPT_MIN, 2, 16);
	TARGET_CHECK(var_h == fixed_h + 1);
	var_h = header_bytes(FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER, 2048, OPT_MIN, 2, 16);
	TARGET_CHECK(var_h == fixed_h + 2);                               /* crosses into 3 bytes */

	run_optimizer(MAT_SILENCE, 16, &saw);
	run_optimizer(MAT_ABRUPT, 16, &saw);
	run_optimizer(MAT_ENTROPY, 16, &saw);
	run_optimizer(MAT_CORRELATED, 16, &saw);
	/* At least one material is better partitioned than one long frame. */
	TARGET_CHECK(saw);
}

static void case_opt_24bit_extremes(void)
{
	FLAC__bool saw = false;
	size_t fixed_h, var_h;
	fixed_h = header_bytes(FLAC__FRAME_NUMBER_TYPE_FRAME_NUMBER, 0, OPT_MIN, 2, 24);
	var_h = header_bytes(FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER, 128, OPT_MIN, 2, 24);
	TARGET_CHECK(var_h == fixed_h + 1);
	var_h = header_bytes(FLAC__FRAME_NUMBER_TYPE_SAMPLE_NUMBER, 2048, OPT_MIN, 2, 24);
	TARGET_CHECK(var_h == fixed_h + 2);

	run_optimizer(MAT_EXTREME24, 24, &saw);
	run_optimizer(MAT_ENTROPY, 24, &saw);
	TARGET_CHECK(saw);
}

/* ================================================================== */
/* stream group                                                        */
/* ================================================================== */

#define STREAM_SAMPLES 1000u
#define STREAM_MIN 64u
#define STREAM_MAX 256u

static void make_stream_pcm(FLAC__int32 *il, uint32_t n)
{
	uint32_t i;
	rng_state = 0x9e3779b9u;
	for(i = 0; i < n; i++) {
		int32_t l, r;
		if(i < 200) { l = r = 0; }
		else if(i < 500) { l = (int32_t)(xs32() % 200) - 100; r = l; }
		else { l = ((int32_t)xs32()) >> 16; r = ((int32_t)xs32()) >> 17; }
		il[2*i] = l; il[2*i+1] = r;
	}
}

typedef enum { FEED_WHOLE_PLANAR, FEED_WHOLE_INTERLEAVED, FEED_PER_SAMPLE, FEED_ALTERNATING } FeedMode;

static FLAC__bool run_stream_encode(FeedMode mode, FLAC__bool search_on, Sink *sink, const FLAC__int32 *il, uint32_t n, uint32_t fixed_block)
{
	FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
	FLAC__bool ok = false;
	uint32_t pos = 0, alt = 0;
	const uint32_t chunks[3] = { 1, 7, 113 };
	if(!e) return false;
	FLAC__stream_encoder_set_channels(e, 2);
	FLAC__stream_encoder_set_bits_per_sample(e, 16);
	FLAC__stream_encoder_set_sample_rate(e, 48000);
	FLAC__stream_encoder_set_compression_level(e, 5);
	if(search_on) {
		if(!FLAC__stream_encoder_set_blocksize_search(e, STREAM_MIN, STREAM_MAX)) goto done;
	}
	else {
		if(!FLAC__stream_encoder_set_blocksize(e, fixed_block)) goto done;
	}
	if(FLAC__stream_encoder_init_stream(e, sink_write, sink_seek, sink_tell, sink_metadata, sink) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) goto done;

	switch(mode) {
		case FEED_WHOLE_PLANAR: {
			const FLAC__int32 *planar[2] = { il, il + 1 };
			/* planar needs separate channel arrays; use a deinterleaved copy */
			FLAC__int32 *c0 = (FLAC__int32 *)calloc(n, sizeof(FLAC__int32));
			FLAC__int32 *c1 = (FLAC__int32 *)calloc(n, sizeof(FLAC__int32));
			uint32_t i;
			for(i = 0; i < n; i++) { c0[i] = il[2*i]; c1[i] = il[2*i+1]; }
			planar[0] = c0; planar[1] = c1;
			ok = FLAC__stream_encoder_process(e, planar, n);
			free(c0); free(c1);
			break;
		}
		case FEED_WHOLE_INTERLEAVED:
			ok = FLAC__stream_encoder_process_interleaved(e, il, n);
			break;
		case FEED_PER_SAMPLE:
			ok = true;
			while(ok && pos < n) { ok = FLAC__stream_encoder_process_interleaved(e, il + 2*pos, 1); pos++; }
			break;
		default:
			ok = true;
			while(ok && pos < n) {
				uint32_t c = chunks[alt % 3];
				if(c > n - pos) c = n - pos;
				ok = FLAC__stream_encoder_process_interleaved(e, il + 2*pos, c);
				pos += c; alt++;
			}
			break;
	}
	if(ok && !FLAC__stream_encoder_finish(e)) ok = false;
done:
	FLAC__stream_encoder_delete(e);
	return ok;
}

static FLAC__bool sinks_equal(const Sink *a, const Sink *b)
{
	return a->len == b->len && memcmp(a->data, b->data, a->len) == 0;
}

/* Compare only the delivered audio frames; metadata (e.g. a seek table)
 * may legitimately differ between otherwise identical encodings. */
static FLAC__bool audio_frames_equal(const Sink *a, const Sink *b)
{
	uint32_t i;
	if(a->audio_frames != b->audio_frames) return false;
	for(i = 0; i < a->audio_frames; i++) {
		if(a->ranges[i].len != b->ranges[i].len) return false;
		if(a->ranges[i].samples != b->ranges[i].samples) return false;
		if(memcmp(a->data + a->ranges[i].offset, b->data + b->ranges[i].offset, a->ranges[i].len) != 0)
			return false;
	}
	return true;
}

static void case_stream_feed_equivalence(void)
{
	FLAC__int32 *il = (FLAC__int32 *)calloc(STREAM_SAMPLES * 2, sizeof(FLAC__int32));
	Sink a, b, c, d, e2;
	make_stream_pcm(il, STREAM_SAMPLES);

	sink_init(&a); sink_init(&b); sink_init(&c); sink_init(&d); sink_init(&e2);
	TARGET_CHECK(run_stream_encode(FEED_WHOLE_PLANAR, true, &a, il, STREAM_SAMPLES, 0));
	TARGET_CHECK(run_stream_encode(FEED_WHOLE_INTERLEAVED, true, &b, il, STREAM_SAMPLES, 0));
	TARGET_CHECK(run_stream_encode(FEED_PER_SAMPLE, true, &c, il, STREAM_SAMPLES, 0));
	TARGET_CHECK(run_stream_encode(FEED_ALTERNATING, true, &d, il, STREAM_SAMPLES, 0));
	/* a second, independent instance interleaved with the others */
	TARGET_CHECK(run_stream_encode(FEED_WHOLE_INTERLEAVED, true, &e2, il, STREAM_SAMPLES, 0));

	TARGET_CHECK(sinks_equal(&a, &b));
	TARGET_CHECK(sinks_equal(&a, &c));
	TARGET_CHECK(sinks_equal(&a, &d));
	TARGET_CHECK(sinks_equal(&a, &e2));

	/* every variant decodes back to the original PCM, MD5 checked */
	TARGET_CHECK(decode_and_compare(&a, il, STREAM_SAMPLES, 2));

	/* final partial window: 1000 = 3*256 + 232, and 232 = 3*64 + 40,
	 * so the last frame must be a 40-sample sub-minimum tail. */
	TARGET_CHECK(a.got_info && a.last_info.total_samples == STREAM_SAMPLES);
	TARGET_CHECK(a.ranges[a.audio_frames - 1].samples == 40);
	TARGET_CHECK(decode_and_compare(&d, il, STREAM_SAMPLES, 2));

	sink_free(&a); sink_free(&b); sink_free(&c); sink_free(&d); sink_free(&e2);
	free(il);
}

static FLAC__bool try_search_params(uint32_t mn, uint32_t mx)
{
	FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
	FLAC__bool r = FLAC__stream_encoder_set_blocksize_search(e, mn, mx);
	FLAC__stream_encoder_delete(e);
	return r;
}

static void case_stream_config_rules(void)
{
	FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
	uint32_t block_before;

	/* invalid parameter pairs are rejected */
	TARGET_CHECK(!try_search_params(0, 256));
	TARGET_CHECK(!try_search_params(64, 64));
	TARGET_CHECK(!try_search_params(65, 256));
	TARGET_CHECK(!try_search_params(32, 256));
	TARGET_CHECK(!try_search_params(64, 8192));
	TARGET_CHECK(!try_search_params(256, 64));
	TARGET_CHECK(!try_search_params(64, 4096));   /* ratio 64 > 16 */
	TARGET_CHECK(try_search_params(64, 1024));    /* ratio 16, legal */

	/* a rejected call leaves the configuration untouched */
	FLAC__stream_encoder_set_blocksize_search(e, 64, 256);
	block_before = FLAC__stream_encoder_get_blocksize(e);
	TARGET_CHECK(block_before == 256);
	TARGET_CHECK(!FLAC__stream_encoder_set_blocksize_search(e, 64, 64));
	TARGET_CHECK(FLAC__stream_encoder_get_blocksize(e) == block_before);

	/* while enabled, set_blocksize is refused and get_blocksize is maximum */
	TARGET_CHECK(!FLAC__stream_encoder_set_blocksize(e, 128));
	TARGET_CHECK(FLAC__stream_encoder_get_blocksize(e) == 256);

	/* disabling restores the prior fixed blocksize setting */
	FLAC__stream_encoder_set_blocksize_search(e, 0, 0);
	TARGET_CHECK(FLAC__stream_encoder_set_blocksize(e, 128));
	TARGET_CHECK(FLAC__stream_encoder_get_blocksize(e) == 128);

	/* loose mid/side and minimum-bitrate limiting force init failure */
	{
		FLAC__StreamEncoder *g = FLAC__stream_encoder_new();
		Sink s;
		sink_init(&s);
		FLAC__stream_encoder_set_channels(g, 2);
		FLAC__stream_encoder_set_bits_per_sample(g, 16);
		FLAC__stream_encoder_set_sample_rate(g, 48000);
		FLAC__stream_encoder_set_do_mid_side_stereo(g, true);
		FLAC__stream_encoder_set_loose_mid_side_stereo(g, true);
		FLAC__stream_encoder_set_blocksize_search(g, 64, 256);
		TARGET_CHECK(FLAC__stream_encoder_init_stream(g, sink_write, 0, 0, 0, &s) != FLAC__STREAM_ENCODER_INIT_STATUS_OK);
		FLAC__stream_encoder_delete(g);
		sink_free(&s);
	}
	{
		FLAC__StreamEncoder *g = FLAC__stream_encoder_new();
		Sink s;
		sink_init(&s);
		FLAC__stream_encoder_set_channels(g, 2);
		FLAC__stream_encoder_set_bits_per_sample(g, 16);
		FLAC__stream_encoder_set_sample_rate(g, 48000);
		FLAC__stream_encoder_set_limit_min_bitrate(g, true);
		FLAC__stream_encoder_set_blocksize_search(g, 64, 256);
		TARGET_CHECK(FLAC__stream_encoder_init_stream(g, sink_write, 0, 0, 0, &s) != FLAC__STREAM_ENCODER_INIT_STATUS_OK);
		FLAC__stream_encoder_delete(g);
		sink_free(&s);
	}
	/* post-init configuration calls are refused */
	{
		FLAC__StreamEncoder *g = FLAC__stream_encoder_new();
		Sink s;
		sink_init(&s);
		FLAC__stream_encoder_set_channels(g, 1);
		FLAC__stream_encoder_set_bits_per_sample(g, 16);
		FLAC__stream_encoder_set_sample_rate(g, 48000);
		FLAC__stream_encoder_set_blocksize_search(g, 64, 256);
		TARGET_CHECK(FLAC__stream_encoder_init_stream(g, sink_write, 0, 0, 0, &s) == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
		TARGET_CHECK(!FLAC__stream_encoder_set_blocksize_search(g, 64, 512));
		TARGET_CHECK(!FLAC__stream_encoder_set_blocksize(g, 128));
		FLAC__stream_encoder_finish(g);
		FLAC__stream_encoder_delete(g);
		sink_free(&s);
	}
	FLAC__stream_encoder_delete(e);
}

static void case_stream_verify_mode(void)
{
	/* Exercise the internal verify decoder (verify FIFO + per-frame
	 * verification) while blocksize search is enabled, for both planar and
	 * interleaved feeding, including a sub-minimum tail. */
	const uint32_t n = 700; /* 2*256 + 188; 188 = 2*64 + 60 tail */
	FLAC__int32 *il = (FLAC__int32 *)calloc(n * 2, sizeof(FLAC__int32));
	FLAC__int32 *c0 = (FLAC__int32 *)calloc(n, sizeof(FLAC__int32));
	FLAC__int32 *c1 = (FLAC__int32 *)calloc(n, sizeof(FLAC__int32));
	const FLAC__int32 *planar[2];
	Sink a, b;
	FLAC__StreamEncoder *e;
	uint32_t i;
	make_stream_pcm(il, n);
	for(i = 0; i < n; i++) { c0[i] = il[2*i]; c1[i] = il[2*i+1]; }
	planar[0] = c0; planar[1] = c1;

	sink_init(&a); sink_init(&b);

	e = FLAC__stream_encoder_new();
	FLAC__stream_encoder_set_channels(e, 2);
	FLAC__stream_encoder_set_bits_per_sample(e, 16);
	FLAC__stream_encoder_set_sample_rate(e, 48000);
	FLAC__stream_encoder_set_compression_level(e, 5);
	FLAC__stream_encoder_set_verify(e, true);
	FLAC__stream_encoder_set_blocksize_search(e, STREAM_MIN, STREAM_MAX);
	TARGET_CHECK(FLAC__stream_encoder_init_stream(e, sink_write, sink_seek, sink_tell, sink_metadata, &a) == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
	TARGET_CHECK(FLAC__stream_encoder_process(e, planar, n));
	TARGET_CHECK(FLAC__stream_encoder_finish(e));
	TARGET_CHECK(FLAC__stream_encoder_get_state(e) == FLAC__STREAM_ENCODER_UNINITIALIZED);
	FLAC__stream_encoder_delete(e);

	e = FLAC__stream_encoder_new();
	FLAC__stream_encoder_set_channels(e, 2);
	FLAC__stream_encoder_set_bits_per_sample(e, 16);
	FLAC__stream_encoder_set_sample_rate(e, 48000);
	FLAC__stream_encoder_set_compression_level(e, 5);
	FLAC__stream_encoder_set_verify(e, true);
	FLAC__stream_encoder_set_blocksize_search(e, STREAM_MIN, STREAM_MAX);
	TARGET_CHECK(FLAC__stream_encoder_init_stream(e, sink_write, sink_seek, sink_tell, sink_metadata, &b) == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
	TARGET_CHECK(FLAC__stream_encoder_process_interleaved(e, il, n));
	TARGET_CHECK(FLAC__stream_encoder_finish(e));
	FLAC__stream_encoder_delete(e);

	/* verify and non-verify builds produce identical audio frames */
	TARGET_CHECK(audio_frames_equal(&a, &b));
	TARGET_CHECK(decode_and_compare(&a, il, n, 2));
	TARGET_CHECK(a.last_info.total_samples == n);
	/* last frame is the 60-sample sub-minimum tail */
	TARGET_CHECK(a.ranges[a.audio_frames - 1].samples == 60);

	sink_free(&a); sink_free(&b);
	free(c0); free(c1); free(il);
}

static void case_stream_tail_empty_disable(void)
{
	FLAC__int32 il[512 * 2];
	FLAC__int32 fixed_pcm[512 * 2];
	Sink a, b, c, d;
	uint32_t i;
	for(i = 0; i < 512; i++) { il[2*i] = il[2*i+1] = ((int32_t)xs32()) >> 16; fixed_pcm[2*i] = fixed_pcm[2*i+1] = il[2*i]; }

	/* exact window boundary: no tail frame, finishes cleanly */
	sink_init(&a);
	TARGET_CHECK(run_stream_encode(FEED_WHOLE_INTERLEAVED, true, &a, il, 512, 0));
	TARGET_CHECK(a.got_info && a.last_info.total_samples == 512);
	TARGET_CHECK(decode_and_compare(&a, il, 512, 2));
	{
		uint64_t acc = 0;
		for(i = 0; i < a.audio_frames; i++) {
			TARGET_CHECK(a.ranges[i].samples >= STREAM_MIN);
			acc += a.ranges[i].samples;
		}
		TARGET_CHECK(acc == 512);
	}

	/* empty stream: zero samples, zero-length calls do not advance */
	sink_init(&b);
	{
		FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
		FLAC__stream_encoder_set_channels(e, 2);
		FLAC__stream_encoder_set_bits_per_sample(e, 16);
		FLAC__stream_encoder_set_sample_rate(e, 48000);
		FLAC__stream_encoder_set_compression_level(e, 5);
		FLAC__stream_encoder_set_blocksize_search(e, STREAM_MIN, STREAM_MAX);
		TARGET_CHECK(FLAC__stream_encoder_init_stream(e, sink_write, sink_seek, sink_tell, sink_metadata, &b) == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
		TARGET_CHECK(FLAC__stream_encoder_process_interleaved(e, il, 0));
		TARGET_CHECK(FLAC__stream_encoder_process(e, (const FLAC__int32 * const[2]){ il, il+1 }, 0));
		TARGET_CHECK(FLAC__stream_encoder_finish(e));
		TARGET_CHECK(b.audio_frames == 0 && b.got_info && b.last_info.total_samples == 0);
		FLAC__stream_encoder_delete(e);
	}

	/* disabled search is byte-identical to a never-enabled fixed encoder */
	sink_init(&c); sink_init(&d);
	TARGET_CHECK(run_stream_encode(FEED_WHOLE_INTERLEAVED, false, &c, fixed_pcm, 512, 128));
	{
		FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
		FLAC__stream_encoder_set_channels(e, 2);
		FLAC__stream_encoder_set_bits_per_sample(e, 16);
		FLAC__stream_encoder_set_sample_rate(e, 48000);
		FLAC__stream_encoder_set_compression_level(e, 5);
		FLAC__stream_encoder_set_blocksize_search(e, 64, 256);
		FLAC__stream_encoder_set_blocksize_search(e, 0, 0);
		TARGET_CHECK(FLAC__stream_encoder_set_blocksize(e, 128));
		TARGET_CHECK(FLAC__stream_encoder_init_stream(e, sink_write, sink_seek, sink_tell, sink_metadata, &d) == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
		TARGET_CHECK(FLAC__stream_encoder_process_interleaved(e, fixed_pcm, 512));
		TARGET_CHECK(FLAC__stream_encoder_finish(e));
		FLAC__stream_encoder_delete(e);
	}
	TARGET_CHECK(sinks_equal(&c, &d));

	sink_free(&a); sink_free(&b); sink_free(&c); sink_free(&d);
}

/* ================================================================== */
/* container group                                                     */
/* ================================================================== */

static void case_container_frame_integrity(void)
{
	FLAC__int32 *il = (FLAC__int32 *)calloc(STREAM_SAMPLES * 2, sizeof(FLAC__int32));
	Sink seekable, plain;
	uint32_t i;
	uint64_t expect_sample = 0;
	make_stream_pcm(il, STREAM_SAMPLES);

	sink_init(&seekable); sink_init(&plain);
	TARGET_CHECK(run_stream_encode(FEED_WHOLE_INTERLEAVED, true, &seekable, il, STREAM_SAMPLES, 0));
	{
		FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
		FLAC__stream_encoder_set_channels(e, 2);
		FLAC__stream_encoder_set_bits_per_sample(e, 16);
		FLAC__stream_encoder_set_sample_rate(e, 48000);
		FLAC__stream_encoder_set_compression_level(e, 5);
		FLAC__stream_encoder_set_blocksize_search(e, STREAM_MIN, STREAM_MAX);
		TARGET_CHECK(FLAC__stream_encoder_init_stream(e, sink_write, 0, 0, sink_metadata, &plain) == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
		TARGET_CHECK(FLAC__stream_encoder_process_interleaved(e, il, STREAM_SAMPLES));
		TARGET_CHECK(FLAC__stream_encoder_finish(e));
		FLAC__stream_encoder_delete(e);
	}

	/* seekable and non-seekable native streams differ only in metadata;
	 * their audio frames are byte-identical and both decode sequentially
	 * with MD5 verification. */
	TARGET_CHECK(audio_frames_equal(&seekable, &plain));
	TARGET_CHECK(decode_and_compare(&seekable, il, STREAM_SAMPLES, 2));
	TARGET_CHECK(decode_and_compare(&plain, il, STREAM_SAMPLES, 2));

	/* independently read every actual frame header */
	TARGET_CHECK(seekable.got_info);
	TARGET_CHECK(seekable.last_info.min_blocksize == STREAM_MIN);
	TARGET_CHECK(seekable.last_info.max_blocksize == STREAM_MAX);
	TARGET_CHECK(seekable.last_info.total_samples == STREAM_SAMPLES);
	for(i = 0; i < seekable.audio_frames; i++) {
		ParsedHeader h;
		size_t off = seekable.ranges[i].offset;
		TARGET_CHECK(off + seekable.ranges[i].len <= seekable.len);
		TARGET_CHECK(parse_header(seekable.data + off, seekable.ranges[i].len, &h));
		TARGET_CHECK(h.variable);
		TARGET_CHECK(h.first_sample == expect_sample);
		TARGET_CHECK(h.blocksize == seekable.ranges[i].samples);
		TARGET_CHECK(h.blocksize >= STREAM_MIN - 1 || i == seekable.audio_frames - 1);
		TARGET_CHECK(h.blocksize <= STREAM_MAX);
		/* the write callback's frame number is the 0-based ordinal, even
		 * though the frame header carries the absolute sample number */
		TARGET_CHECK(seekable.ranges[i].frame_no == i);
		expect_sample += h.blocksize;
	}
	TARGET_CHECK(expect_sample == STREAM_SAMPLES);
	/* frame byte bounds recorded in STREAMINFO */
	TARGET_CHECK(seekable.last_info.min_framesize > 0 && seekable.last_info.max_framesize >= seekable.last_info.min_framesize);

	sink_free(&seekable); sink_free(&plain);
	free(il);
}

/* locate the seektable metadata block inside a native FLAC buffer */
static const FLAC__byte *find_seektable(const Sink *s, uint32_t *num_points)
{
	size_t pos = 4; /* skip fLaC */
	while(pos + 4 <= s->len) {
		unsigned type = s->data[pos] & 0x7F;
		unsigned last = s->data[pos] >> 7;
		uint32_t length = ((uint32_t)s->data[pos+1] << 16) | ((uint32_t)s->data[pos+2] << 8) | s->data[pos+3];
		pos += 4;
		if(type == 3) { *num_points = length / 18; return s->data + pos; }
		if(last) break;
		pos += length;
	}
	*num_points = 0;
	return 0;
}

static void case_container_seektable_writefail(void)
{
	FLAC__int32 *il = (FLAC__int32 *)calloc(STREAM_SAMPLES * 2, sizeof(FLAC__int32));
	Sink s;
	FLAC__StreamMetadata *st;
	const FLAC__byte *table;
	uint32_t num_points = 0, i;
	make_stream_pcm(il, STREAM_SAMPLES);

	st = FLAC__metadata_object_new(FLAC__METADATA_TYPE_SEEKTABLE);
	/* Strictly increasing template.  The first two targets (10 and 20)
	 * always fall inside frame 0 because its length is at least minimum,
	 * exercising the same-frame seekpoint dedup at finish time. */
	{
		FLAC__uint64 targets[8] = { 10, 20, 250, 400, 600, 750, 900, 990 };
		TARGET_CHECK(FLAC__metadata_object_seektable_template_append_points(st, targets, 8));
	}

	sink_init(&s);
	{
		FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
		FLAC__StreamMetadata *meta[1] = { st };
		FLAC__stream_encoder_set_channels(e, 2);
		FLAC__stream_encoder_set_bits_per_sample(e, 16);
		FLAC__stream_encoder_set_sample_rate(e, 48000);
		FLAC__stream_encoder_set_compression_level(e, 5);
		FLAC__stream_encoder_set_blocksize_search(e, STREAM_MIN, STREAM_MAX);
		FLAC__stream_encoder_set_metadata(e, meta, 1);
		TARGET_CHECK(FLAC__stream_encoder_init_stream(e, sink_write, sink_seek, sink_tell, sink_metadata, &s) == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
		TARGET_CHECK(FLAC__stream_encoder_process_interleaved(e, il, STREAM_SAMPLES));
		TARGET_CHECK(FLAC__stream_encoder_finish(e));
		FLAC__stream_encoder_delete(e);
	}
	table = find_seektable(&s, &num_points);
	TARGET_CHECK(table != 0 && num_points == st->data.seek_table.num_points);

	/* every resolved seek point references a real delivered frame at its
	 * true byte offset, with the frame's actual sample count; resolved
	 * sample numbers are unique (same-frame targets were deduplicated). */
	{
		uint64_t prev_sample = 0;
		FLAC__bool have_prev = false;
		uint32_t resolved = 0, placeholders = 0;
		/* seek table offsets are relative to the first audio frame */
		const size_t audio_start = s.ranges[0].offset;
		for(i = 0; i < num_points; i++) {
			const FLAC__byte *q = table + 18 * i;
			uint64_t sample, offset;
			uint32_t frame_samples, j;
			sample = 0; offset = 0; frame_samples = 0;
			{
				int b;
				for(b = 0; b < 8; b++) sample = (sample << 8) | q[b];
				for(b = 8; b < 16; b++) offset = (offset << 8) | q[b];
				frame_samples = ((uint32_t)q[16] << 8) | q[17];
			}
			if(sample == FLAC__STREAM_METADATA_SEEKPOINT_PLACEHOLDER) { placeholders++; continue; }
			resolved++;
			if(have_prev)
				TARGET_CHECK(sample > prev_sample);
			prev_sample = sample; have_prev = true;
			for(j = 0; j < s.audio_frames; j++) {
				if((size_t)offset == s.ranges[j].offset - audio_start) {
					ParsedHeader h;
					TARGET_CHECK(parse_header(s.data + s.ranges[j].offset, s.ranges[j].len, &h));
					TARGET_CHECK(h.first_sample == sample);
					TARGET_CHECK(s.ranges[j].samples == frame_samples);
					break;
				}
			}
			TARGET_CHECK(j < s.audio_frames);
		}
		TARGET_CHECK(have_prev);
		/* The two targets inside frame 0 collapse onto a single point; the
		 * encoder's existing dedup turns the duplicate into a placeholder. */
		TARGET_CHECK(placeholders >= 1);
		TARGET_CHECK(resolved + placeholders == num_points);
	}

	/* write failure: sticky client error, no frames delivered after it */
	{
		Sink f;
		FLAC__StreamEncoder *e = FLAC__stream_encoder_new();
		sink_init(&f);
		f.fail_after = 3;
		FLAC__stream_encoder_set_channels(e, 2);
		FLAC__stream_encoder_set_bits_per_sample(e, 16);
		FLAC__stream_encoder_set_sample_rate(e, 48000);
		FLAC__stream_encoder_set_compression_level(e, 5);
		FLAC__stream_encoder_set_blocksize_search(e, STREAM_MIN, STREAM_MAX);
		TARGET_CHECK(FLAC__stream_encoder_init_stream(e, sink_write, sink_seek, sink_tell, 0, &f) == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
		TARGET_CHECK(!FLAC__stream_encoder_process_interleaved(e, il, STREAM_SAMPLES));
		TARGET_CHECK(FLAC__stream_encoder_get_state(e) == FLAC__STREAM_ENCODER_CLIENT_ERROR);
		/* sticky: further processing is refused */
		TARGET_CHECK(!FLAC__stream_encoder_process_interleaved(e, il, 8));
		/* finish must not deliver any frame that came after the failure */
		FLAC__stream_encoder_finish(e);
		TARGET_CHECK(f.audio_frames == 3);
		FLAC__stream_encoder_delete(e);
		sink_free(&f);
	}

	FLAC__metadata_object_delete(st);
	sink_free(&s);
	free(il);
}

/* ------------------------------------------------------------------ */
/* case registry                                                       */
/* ------------------------------------------------------------------ */

#ifdef FLAC__TEST_BLOCKSIZE_SEARCH
/* Independently count the feasible (start,length) pairs for a stream of
 * total_samples under (minimum,maximum), using only the partition rules
 * (frame lengths are minimum * 2^k, never crossing a window).  This is the
 * number of times the encoder must actually construct a trial frame. */
static uint32_t expected_candidate_constructs(uint32_t total_samples, uint32_t minimum, uint32_t maximum)
{
	uint32_t remaining = total_samples;
	uint32_t total = 0;
	while(remaining > 0) {
		uint32_t window = remaining < maximum ? remaining : maximum;
		uint32_t width = window / minimum;      /* partitionable slots */
		uint32_t start;
		/* a run shorter than minimum is the single tail frame, not a
		 * window candidate */
		if(width == 0) break;
		for(start = 0; start < width; start++) {
			uint32_t slots = 1;
			while(slots <= width - start) { total++; slots <<= 1; }
		}
		remaining -= window;
	}
	return total;
}

static void case_container_candidate_construction(void)
{
	FLAC__int32 *il = (FLAC__int32 *)calloc(STREAM_SAMPLES * 2, sizeof(FLAC__int32));
	Sink sink;
	FLAC__StreamEncoder *e;
	uint32_t constructs = 0, lengths_count = 0, i;
	const uint32_t *lengths = 0;
	const uint32_t allowed[3] = { STREAM_MIN, 128, STREAM_MAX };
	make_stream_pcm(il, STREAM_SAMPLES);

	sink_init(&sink);
	e = FLAC__stream_encoder_new();
	FLAC__stream_encoder_set_channels(e, 2);
	FLAC__stream_encoder_set_bits_per_sample(e, 16);
	FLAC__stream_encoder_set_sample_rate(e, 48000);
	FLAC__stream_encoder_set_compression_level(e, 5);
	TARGET_CHECK(FLAC__stream_encoder_set_blocksize_search(e, STREAM_MIN, STREAM_MAX));
	TARGET_CHECK(FLAC__stream_encoder_init_stream(e, sink_write, sink_seek, sink_tell, sink_metadata, &sink) == FLAC__STREAM_ENCODER_INIT_STATUS_OK);
	TARGET_CHECK(FLAC__stream_encoder_process_interleaved(e, il, STREAM_SAMPLES));
	TARGET_CHECK(FLAC__stream_encoder_finish(e));

	FLAC__stream_encoder_test_get_blocksize_search_stats(e, &constructs, &lengths, &lengths_count);

	/* The real construction count matches the independently enumerated
	 * feasible pairs: every candidate is analyzed once and partition-path
	 * enumeration never triggers a second encode. */
	TARGET_CHECK(constructs == expected_candidate_constructs(STREAM_SAMPLES, STREAM_MIN, STREAM_MAX));
	TARGET_CHECK(lengths_count == constructs);
	for(i = 0; i < lengths_count; i++) {
		uint32_t j, ok = 0;
		for(j = 0; j < 3; j++)
			if(lengths[i] == allowed[j]) { ok = 1; break; }
		TARGET_CHECK(ok);
	}
	/* delivered frames are a subset of the frames actually constructed */
	TARGET_CHECK(sink.audio_frames <= constructs);
	TARGET_CHECK(sink.audio_frames >= 1);

	FLAC__stream_encoder_delete(e);
	sink_free(&sink);
	free(il);
}
#endif

const TargetCase target_cases[] = {
	{ "optimizer", "opt_16bit_materials",  case_opt_16bit_materials },
	{ "optimizer", "opt_24bit_extremes",   case_opt_24bit_extremes },
	{ "stream",    "stream_feed_equivalence", case_stream_feed_equivalence },
	{ "stream",    "stream_config_rules",  case_stream_config_rules },
	{ "stream",    "stream_tail_empty_disable", case_stream_tail_empty_disable },
	{ "stream",    "stream_verify_mode",   case_stream_verify_mode },
	{ "container", "container_frame_integrity", case_container_frame_integrity },
	{ "container", "container_seektable_writefail", case_container_seektable_writefail },
#ifdef FLAC__TEST_BLOCKSIZE_SEARCH
	{ "container", "container_candidate_construction", case_container_candidate_construction },
#endif
};
const size_t target_count = sizeof(target_cases) / sizeof(target_cases[0]);
