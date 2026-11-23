#include <string.h>
#include <re.h>
#include <rem.h>
#include <re_pcm.h>
#include "../task_support/check.h"


/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

static int make_rtp(struct mbuf **mbp, uint32_t ssrc, uint8_t pt,
		    uint16_t seq, uint32_t ts, const int16_t *sampv,
		    size_t frames, uint8_t channels)
{
	struct rtp_header h = {0};
	struct mbuf *mb;
	size_t i, n;
	int err;

	mb = mbuf_alloc(64 + frames * channels * 2);
	if (!mb)
		return ENOMEM;

	h.ver  = RTP_VERSION;
	h.pt   = pt;
	h.seq  = seq;
	h.ts   = ts;
	h.ssrc = ssrc;

	err = rtp_hdr_encode(mb, &h);
	if (err) {
		mem_deref(mb);
		return err;
	}

	n = frames * channels;
	for (i = 0; i < n; ++i) {
		uint16_t v = (uint16_t)sampv[i];
		uint8_t b[2];

		b[0] = (uint8_t)(v >> 8);
		b[1] = (uint8_t)(v & 0xff);
		err = mbuf_write_mem(mb, b, 2);
		if (err) {
			mem_deref(mb);
			return err;
		}
	}

	mb->pos = 0;
	*mbp = mb;

	return 0;
}


static int push_samples(struct rtp_pcm *rx, uint32_t ssrc, uint8_t pt,
			uint16_t seq, uint32_t ts, const int16_t *sampv,
			size_t frames, uint8_t channels)
{
	struct mbuf *mb = NULL;
	int err;

	err = make_rtp(&mb, ssrc, pt, seq, ts, sampv, frames, channels);
	if (err)
		return err;

	err = rtp_pcm_push(rx, mb);
	mem_deref(mb);

	return err;
}


/* Fill a buffer with a deterministic ramp starting at base */
static void fill_ramp(int16_t *v, size_t n, int16_t base)
{
	size_t i;

	for (i = 0; i < n; ++i)
		v[i] = (int16_t)(base + (int16_t)i);
}


/* ------------------------------------------------------------------ */
/* Timeline group                                                      */
/* ------------------------------------------------------------------ */

/* Variable-length packets and stereo partial reads */
static int test_timeline_variable_and_stereo(void)
{
	struct aubuf *ab = NULL;
	int16_t in1[] = {1, 2, 3, 4};          /* 2 stereo frames */
	int16_t in2[] = {10, 20, 30, 40, 50, 60}; /* 3 stereo frames */
	int16_t out[8];
	int err;

	err = aubuf_timeline_alloc(&ab, 2, 16);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	TASK_CHECK(aubuf_timeline_write(ab, 0, in1, 2) == 0);
	TASK_CHECK(aubuf_timeline_write(ab, 2, in2, 3) == 0);

	memset(out, 0x7f, sizeof(out));
	TASK_CHECK(aubuf_timeline_read(ab, out, 2) == 0);
	TASK_CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4);

	/* partial read of the second write */
	TASK_CHECK(aubuf_timeline_read(ab, out, 2) == 0);
	TASK_CHECK(out[0] == 10 && out[1] == 20 && out[2] == 30 && out[3] == 40);

	TASK_CHECK(aubuf_timeline_read(ab, out, 1) == 0);
	TASK_CHECK(out[0] == 50 && out[1] == 60);

	mem_deref(ab);
	return 0;
}


/* Out-of-order overlapping writes: first writer wins */
static int test_timeline_overlap(void)
{
	struct aubuf *ab = NULL;
	int16_t a[] = {100, 101, 102, 103};
	int16_t b[] = {-1, -2, -3, -4};
	int16_t out[4];
	int err;

	err = aubuf_timeline_alloc(&ab, 1, 8);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	TASK_CHECK(aubuf_timeline_write(ab, 0, a, 4) == 0);
	/* overlap positions 2..3, extend to 4..5 */
	TASK_CHECK(aubuf_timeline_write(ab, 2, b, 4) == 0);

	TASK_CHECK(aubuf_timeline_read(ab, out, 4) == 0);
	TASK_CHECK(out[0] == 100 && out[1] == 101);
	TASK_CHECK(out[2] == 102 && out[3] == 103);

	/* the earlier write wins at the overlap, but 4..5 are new */
	TASK_CHECK(aubuf_timeline_read(ab, out, 2) == 0);
	TASK_CHECK(out[0] == -3 && out[1] == -4);

	mem_deref(ab);
	return 0;
}


/* Hole in the middle is output as silence */
static int test_timeline_hole_silence(void)
{
	struct aubuf *ab = NULL;
	int16_t a[] = {7, 8};
	int16_t b[] = {9, 10};
	int16_t out[6];
	int err;

	err = aubuf_timeline_alloc(&ab, 1, 8);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	TASK_CHECK(aubuf_timeline_write(ab, 0, a, 2) == 0);
	TASK_CHECK(aubuf_timeline_write(ab, 4, b, 2) == 0);

	memset(out, 0x7f, sizeof(out));
	TASK_CHECK(aubuf_timeline_read(ab, out, 6) == 0);
	TASK_CHECK(out[0] == 7 && out[1] == 8);
	TASK_CHECK(out[2] == 0 && out[3] == 0);
	TASK_CHECK(out[4] == 9 && out[5] == 10);

	mem_deref(ab);
	return 0;
}


/* Late write behind the consumption front is rejected */
static int test_timeline_late_reject(void)
{
	struct aubuf *ab = NULL;
	int16_t a[] = {1, 2, 3, 4};
	int16_t late[] = {50, 51};
	int16_t out[4];
	int err;

	err = aubuf_timeline_alloc(&ab, 1, 8);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	TASK_CHECK(aubuf_timeline_write(ab, 0, a, 4) == 0);
	TASK_CHECK(aubuf_timeline_read(ab, out, 2) == 0);
	TASK_CHECK(out[0] == 1 && out[1] == 2);

	/* frame 0/1 are behind front=2 */
	TASK_CHECK(aubuf_timeline_write(ab, 0, late, 2) == ERANGE);
	TASK_CHECK(aubuf_timeline_write(ab, 1, late, 2) == ERANGE);

	/* front stays at 2; next read returns the not-yet-consumed part */
	TASK_CHECK(aubuf_timeline_read(ab, out, 2) == 0);
	TASK_CHECK(out[0] == 3 && out[1] == 4);

	mem_deref(ab);
	return 0;
}


/* Window violation leaves state untouched and can recover */
static int test_timeline_window_recovery(void)
{
	struct aubuf *ab = NULL;
	int16_t a[] = {1, 2};
	int16_t big[] = {10, 11, 12, 13};
	int16_t out[8];
	int err;

	err = aubuf_timeline_alloc(&ab, 1, 4);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	TASK_CHECK(aubuf_timeline_write(ab, 0, a, 2) == 0);
	/* 4 frames at frame 2 -> ends at 6, beyond window 0..3 */
	TASK_CHECK(aubuf_timeline_write(ab, 2, big, 4) == ERANGE);
	/* frame 4 is outside window too */
	TASK_CHECK(aubuf_timeline_write(ab, 4, a, 2) == ERANGE);
	/* write fully inside window still works */
	TASK_CHECK(aubuf_timeline_write(ab, 2, big, 2) == 0);

	TASK_CHECK(aubuf_timeline_read(ab, out, 4) == 0);
	TASK_CHECK(out[0] == 1 && out[1] == 2 && out[2] == 10 && out[3] == 11);

	/* consumed front = 4; window is now [4, 8) */
	TASK_CHECK(aubuf_timeline_read(ab, out, 1) == 0);
	TASK_CHECK(out[0] == 0);
	TASK_CHECK(aubuf_timeline_write(ab, 5, a, 2) == 0);
	TASK_CHECK(aubuf_timeline_read(ab, out, 3) == 0);
	TASK_CHECK(out[0] == 1 && out[1] == 2 && out[2] == 0);

	mem_deref(ab);
	return 0;
}


/* Old-mode calls to timeline API return EINVAL */
static int test_timeline_legacy_mode(void)
{
	struct aubuf *ab = NULL;
	int16_t out[2];
	int err;

	err = aubuf_alloc(&ab, 0, 64);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	TASK_CHECK(aubuf_timeline_write(ab, 0, out, 1) == EINVAL);
	TASK_CHECK(aubuf_timeline_read(ab, out, 1) == EINVAL);

	mem_deref(ab);
	return 0;
}


/* Legacy aubuf normal path still works */
static int test_legacy_aubuf_path(void)
{
	struct aubuf *ab = NULL;
	int16_t a[] = {11, 12};
	int16_t b[] = {21, 22};
	int16_t pcm[4] = {0};
	struct auframe af;
	int err;

	err = aubuf_alloc(&ab, 0, 64);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	aubuf_set_live(ab, false);

	auframe_init(&af, AUFMT_S16LE, a, 2, 8000, 1);
	af.timestamp = 1000000;
	TASK_CHECK(aubuf_write_auframe(ab, &af) == 0);

	auframe_init(&af, AUFMT_S16LE, b, 2, 8000, 1);
	af.timestamp = 1000500;
	TASK_CHECK(aubuf_write_auframe(ab, &af) == 0);

	aubuf_read_samp(ab, pcm, 4);
	TASK_CHECK(pcm[0] == 11 && pcm[1] == 12);
	TASK_CHECK(pcm[2] == 21 && pcm[3] == 22);

	mem_deref(ab);
	return 0;
}


/* ------------------------------------------------------------------ */
/* Ingress group                                                       */
/* ------------------------------------------------------------------ */

/* Basic ingress: single channel, big-endian decode, mbuf pos preserved */
static int test_ingress_basic(void)
{
	struct rtp_pcm *rx = NULL;
	struct mbuf *mb = NULL;
	int16_t in[] = {123, -45, 300, -2};
	int16_t out[4];
	int err;

	err = rtp_pcm_alloc(&rx, 7, 96, 1, 8);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	err = make_rtp(&mb, 7, 96, 10, 1000, in, 4, 1);
	TASK_CHECK(err == 0);

	TASK_CHECK(rtp_pcm_push(rx, mb) == 0);
	TASK_CHECK(mb->pos == 0);

	TASK_CHECK(rtp_pcm_read(rx, out, 4) == 0);
	TASK_CHECK(out[0] == 123 && out[1] == -45);
	TASK_CHECK(out[2] == 300 && out[3] == -2);

	mem_deref(mb);
	mem_deref(rx);
	return 0;
}


/* Read only silence before first packet; no anchor is created */
static int test_ingress_silence_before(void)
{
	struct rtp_pcm *rx = NULL;
	int16_t out[4];
	int err;

	err = rtp_pcm_alloc(&rx, 7, 96, 1, 8);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	memset(out, 0x7f, sizeof(out));
	TASK_CHECK(rtp_pcm_read(rx, out, 4) == 0);
	TASK_CHECK(out[0] == 0 && out[1] == 0 && out[2] == 0 && out[3] == 0);

	/* anchor is established by the first packet at frame 0, and since the
	 * pre-roll read did not advance the timeline, the packet is at 0 */
	TASK_CHECK(push_samples(rx, 7, 96, 10, 500, (int16_t[]){5, 6}, 2,
				1) == 0);
	TASK_CHECK(rtp_pcm_read(rx, out, 2) == 0);
	TASK_CHECK(out[0] == 5 && out[1] == 6);

	mem_deref(rx);
	return 0;
}


/* Out-of-order overlapping packets: first accepted sample wins */
static int test_ingress_reorder_overlap(void)
{
	struct rtp_pcm *rx = NULL;
	int16_t out[6];
	int err;

	err = rtp_pcm_alloc(&rx, 7, 96, 1, 16);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	/* seq 10, ts 0: frames 0..3 */
	TASK_CHECK(push_samples(rx, 7, 96, 10, 0,
				(int16_t[]){1, 2, 3, 4}, 4, 1) == 0);
	/* seq 12, ts 4: frames 4..5 */
	TASK_CHECK(push_samples(rx, 7, 96, 12, 4,
				(int16_t[]){5, 6}, 2, 1) == 0);
	/* seq 11, ts 2: frames 2..3, overlapping and late in seq but not past
	 * the consumption front */
	TASK_CHECK(push_samples(rx, 7, 96, 11, 2,
				(int16_t[]){30, 40}, 2, 1) == 0);

	TASK_CHECK(rtp_pcm_read(rx, out, 6) == 0);
	TASK_CHECK(out[0] == 1 && out[1] == 2);
	TASK_CHECK(out[2] == 3 && out[3] == 4);
	TASK_CHECK(out[4] == 5 && out[5] == 6);

	mem_deref(rx);
	return 0;
}


/* Hole yields silence, then late data behind the front is rejected */
static int test_ingress_hole_and_late(void)
{
	struct rtp_pcm *rx = NULL;
	int16_t out[8];
	int err;

	err = rtp_pcm_alloc(&rx, 7, 96, 1, 16);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	TASK_CHECK(push_samples(rx, 7, 96, 10, 0,
				(int16_t[]){1, 2}, 2, 1) == 0);
	/* seq 12, ts 4: leaves frames 2..3 missing */
	TASK_CHECK(push_samples(rx, 7, 96, 12, 4,
				(int16_t[]){5, 6}, 2, 1) == 0);

	TASK_CHECK(rtp_pcm_read(rx, out, 6) == 0);
	TASK_CHECK(out[0] == 1 && out[1] == 2);
	TASK_CHECK(out[2] == 0 && out[3] == 0);
	TASK_CHECK(out[4] == 5 && out[5] == 6);

	/* late packet for frames 2..3 (already consumed) -> ERANGE */
	TASK_CHECK(push_samples(rx, 7, 96, 11, 2,
				(int16_t[]){30, 40}, 2, 1) == ERANGE);

	mem_deref(rx);
	return 0;
}


/* Extension and padding handling */
static int test_ingress_ext_padding(void)
{
	struct rtp_pcm *rx = NULL;
	struct mbuf *mb = NULL;
	int16_t out[4];
	int err;

	err = rtp_pcm_alloc(&rx, 7, 96, 1, 16);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	/* Build packet with header extension and padding manually */
	{
		struct rtp_header h = {0};

		mb = mbuf_alloc(128);
		TASK_CHECK(mb != NULL);
		if (!mb) {
			mem_deref(rx);
			return 1;
		}

		h.ver  = RTP_VERSION;
		h.pt   = 96;
		h.seq  = 10;
		h.ts   = 0;
		h.ssrc = 7;
		h.ext  = true;
		h.pad  = true;

		TASK_CHECK(rtp_hdr_encode(mb, &h) == 0);
		/* extension: one 32-bit word */
		TASK_CHECK(mbuf_write_u16(mb, htons(0xbede)) == 0);
		TASK_CHECK(mbuf_write_u16(mb, htons(1)) == 0);
		TASK_CHECK(mbuf_write_u32(mb, htonl(0xdeadbeef)) == 0);
		/* payload: samples 11, 12 (big endian) */
		TASK_CHECK(mbuf_write_mem(mb, (uint8_t[]){0, 11, 0, 12}, 4) == 0);
		/* 2 bytes padding, last byte is the count 2 */
		TASK_CHECK(mbuf_write_mem(mb, (uint8_t[]){0, 2}, 2) == 0);
		mb->pos = 0;
	}

	TASK_CHECK(rtp_pcm_push(rx, mb) == 0);
	TASK_CHECK(mb->pos == 0);

	TASK_CHECK(rtp_pcm_read(rx, out, 2) == 0);
	TASK_CHECK(out[0] == 11 && out[1] == 12);

	mem_deref(mb);
	mem_deref(rx);
	return 0;
}


/* Invalid padding after L16 removal leaves state untouched */
static int test_ingress_pad_bad(void)
{
	struct rtp_pcm *rx = NULL;
	struct mbuf *mb = NULL;
	int16_t out[2];
	int err;

	err = rtp_pcm_alloc(&rx, 7, 96, 1, 16);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	/* Good packet first */
	TASK_CHECK(push_samples(rx, 7, 96, 10, 0,
				(int16_t[]){1, 2}, 2, 1) == 0);

	/* Packet with padding count > payload length */
	{
		struct rtp_header h = {0};

		mb = mbuf_alloc(64);
		TASK_CHECK(mb != NULL);
		if (!mb) {
			mem_deref(rx);
			return 1;
		}

		h.ver = RTP_VERSION;
		h.pt  = 96;
		h.seq = 11;
		h.ts  = 2;
		h.ssrc = 7;
		h.pad = true;

		TASK_CHECK(rtp_hdr_encode(mb, &h) == 0);
		TASK_CHECK(mbuf_write_mem(mb, (uint8_t[]){0, 5, 0, 6}, 4) == 0);
		/* padding count 200 > total payload (5) */
		TASK_CHECK(mbuf_write_mem(mb, (uint8_t[]){200}, 1) == 0);
		mb->pos = 0;
	}

	TASK_CHECK(rtp_pcm_push(rx, mb) == EBADMSG);
	TASK_CHECK(mb->pos == 0);

	/* playback state unchanged, both samples still readable */
	TASK_CHECK(rtp_pcm_read(rx, out, 2) == 0);
	TASK_CHECK(out[0] == 1 && out[1] == 2);

	mem_deref(mb);
	mem_deref(rx);
	return 0;
}


/* Invalid SSRC/PT/version -> EBADMSG, mbuf untouched */
static int test_ingress_bad_header(void)
{
	struct rtp_pcm *rx = NULL;
	int err;

	err = rtp_pcm_alloc(&rx, 7, 96, 1, 16);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	TASK_CHECK(push_samples(rx, 8, 96, 10, 0,
				(int16_t[]){1, 2}, 2, 1) == EBADMSG);
	TASK_CHECK(push_samples(rx, 7, 97, 10, 0,
				(int16_t[]){1, 2}, 2, 1) == EBADMSG);

	/* windows: empty L16 payload -> EBADMSG */
	{
		struct mbuf *mb = NULL;
		int e = make_rtp(&mb, 7, 96, 10, 0, NULL, 0, 1);
		TASK_CHECK(e == 0);
		TASK_CHECK(rtp_pcm_push(rx, mb) == EBADMSG);
		mem_deref(mb);
	}

	/* odd number of bytes -> EBADMSG */
	{
		struct rtp_header h = {0};
		struct mbuf *mb = mbuf_alloc(64);
		TASK_CHECK(mb != NULL);
		h.ver = RTP_VERSION;
		h.pt = 96;
		h.seq = 10;
		h.ts = 0;
		h.ssrc = 7;
		TASK_CHECK(rtp_hdr_encode(mb, &h) == 0);
		TASK_CHECK(mbuf_write_mem(mb, (uint8_t[]){0, 1, 2}, 3) == 0);
		mb->pos = 0;
		TASK_CHECK(rtp_pcm_push(rx, mb) == EBADMSG);
		mem_deref(mb);
	}

	mem_deref(rx);
	return 0;
}


/* Parameter validation for alloc */
static int test_ingress_alloc_validation(void)
{
	struct rtp_pcm *rx = NULL;

	TASK_CHECK(rtp_pcm_alloc(&rx, 7, 128, 1, 8) == EINVAL);
	TASK_CHECK(rtp_pcm_alloc(&rx, 7, 96, 0, 8) == EINVAL);
	TASK_CHECK(rtp_pcm_alloc(&rx, 7, 96, 3, 8) == EINVAL);
	TASK_CHECK(rtp_pcm_alloc(&rx, 7, 96, 1, 0) == EINVAL);
	TASK_CHECK(rtp_pcm_alloc(&rx, 7, 96, 1, 65537) == EINVAL);
	TASK_CHECK(rx == NULL);

	return 0;
}


/* ------------------------------------------------------------------ */
/* Epochs group                                                        */
/* ------------------------------------------------------------------ */

/* Sequence wrap 65535 -> 0 */
static int test_epochs_seq_wrap(void)
{
	struct rtp_source s = {0};

	rtp_source_init_seq(&s, 65535);
	TASK_CHECK(rtp_source_update_seq_ex(&s, 0) == RTP_SEQ_ACCEPT);
	TASK_CHECK(s.cycles == 65536);
	TASK_CHECK(s.received == 1);

	/* legacy API still returns 0/1 */
	rtp_source_init_seq(&s, 65535);
	TASK_CHECK(rtp_source_update_seq(&s, 0) == 1);

	return 0;
}


/* Two consecutive sequence wraps are both accepted, no restart */
static int test_epochs_double_wrap(void)
{
	struct rtp_source s = {0};
	uint32_t n = 2u * 65536u + 4u;
	uint32_t i;
	bool ok = true;

	rtp_source_init_seq(&s, 0);

	for (i = 0; i < n; ++i) {
		uint16_t seq = (uint16_t)((i + 1u) & 0xffffu);

		if (rtp_source_update_seq_ex(&s, seq) != RTP_SEQ_ACCEPT) {
			ok = false;
			break;
		}
	}

	TASK_CHECK(ok);
	TASK_CHECK(s.cycles == 2u * 65536u);

	/* Receiver-level: a sequence wrap mid-stream must not restart */
	{
		struct rtp_pcm *rx = NULL;
		int16_t out[4];

		TASK_CHECK(rtp_pcm_alloc(&rx, 7, 96, 1, 16) == 0);
		if (!rx)
			return 1;

		TASK_CHECK(push_samples(rx, 7, 96, 65534, 0,
					(int16_t[]){1, 2}, 2, 1) == 0);
		TASK_CHECK(push_samples(rx, 7, 96, 65535, 2,
					(int16_t[]){3, 4}, 2, 1) == 0);
		TASK_CHECK(push_samples(rx, 7, 96, 0, 4,
					(int16_t[]){5, 6}, 2, 1) == 0);

		TASK_CHECK(rtp_pcm_read(rx, out, 4) == 0);
		TASK_CHECK(out[0] == 1 && out[1] == 2);
		TASK_CHECK(out[2] == 3 && out[3] == 4);

		mem_deref(rx);
	}

	return 0;
}


/* Timestamp wrap in the PCM receiver */
static int test_epochs_ts_wrap(void)
{
	struct rtp_pcm *rx = NULL;
	int16_t out[8];
	uint32_t base = 0xfffffffcu;
	int err;

	err = rtp_pcm_alloc(&rx, 7, 96, 1, 32);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	/* anchor at base, frames 0..3 */
	TASK_CHECK(push_samples(rx, 7, 96, 10, base,
				(int16_t[]){1, 2, 3, 4}, 4, 1) == 0);
	/* next packet ts wraps around: base + 8 -> 0x...f8 */
	TASK_CHECK(push_samples(rx, 7, 96, 11, base + 8,
				(int16_t[]){9, 10}, 2, 1) == 0);

	TASK_CHECK(rtp_pcm_read(rx, out, 6) == 0);
	TASK_CHECK(out[0] == 1 && out[1] == 2 && out[2] == 3 && out[3] == 4);
	TASK_CHECK(out[4] == 0 && out[5] == 0);  /* frames 4..7 silent gap */

	mem_deref(rx);
	return 0;
}


/* Isolated large jump -> EAGAIN, restart after second confirm */
static int test_epochs_restart(void)
{
	struct rtp_pcm *rx = NULL;
	int16_t out[4];
	int err;

	err = rtp_pcm_alloc(&rx, 7, 96, 1, 32);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	/* generation 1: seq 10, ts 0, frames 0..1 */
	TASK_CHECK(push_samples(rx, 7, 96, 10, 0,
				(int16_t[]){1, 2}, 2, 1) == 0);

	/* big jump: candidate only, returns EAGAIN */
	TASK_CHECK(push_samples(rx, 7, 96, 40000, 5000,
				(int16_t[]){99, 99}, 2, 1) == EAGAIN);

	/* old audio is still playable */
	TASK_CHECK(rtp_pcm_read(rx, out, 2) == 0);
	TASK_CHECK(out[0] == 1 && out[1] == 2);

	/* second sequential packet confirms restart with new generation */
	TASK_CHECK(push_samples(rx, 7, 96, 40001, 5002,
				(int16_t[]){7, 8}, 2, 1) == 0);

	/* the new packet established the new frame-0 anchor */
	TASK_CHECK(rtp_pcm_read(rx, out, 2) == 0);
	TASK_CHECK(out[0] == 7 && out[1] == 8);

	mem_deref(rx);
	return 0;
}


/* Restart clears old PCM: old data is not replayed */
static int test_epochs_restart_clears(void)
{
	struct rtp_pcm *rx = NULL;
	int16_t out[8];
	int err;

	err = rtp_pcm_alloc(&rx, 7, 96, 1, 32);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	/* fill frames 0..3 with data */
	TASK_CHECK(push_samples(rx, 7, 96, 10, 0,
				(int16_t[]){1, 2, 3, 4}, 4, 1) == 0);
	/* consume 0..1 */
	TASK_CHECK(rtp_pcm_read(rx, out, 2) == 0);
	TASK_CHECK(out[0] == 1 && out[1] == 2);

	TASK_CHECK(push_samples(rx, 7, 96, 40000, 9000,
				(int16_t[]){9}, 1, 1) == EAGAIN);
	TASK_CHECK(push_samples(rx, 7, 96, 40001, 9001,
				(int16_t[]){20, 21}, 2, 1) == 0);

	/* old frames 2..3 are gone; new generation starts at frame 0 */
	TASK_CHECK(rtp_pcm_read(rx, out, 2) == 0);
	TASK_CHECK(out[0] == 20 && out[1] == 21);

	mem_deref(rx);
	return 0;
}


/* Normal reorder does not trigger restart */
static int test_epochs_reorder_no_restart(void)
{
	struct rtp_pcm *rx = NULL;
	int16_t out[6];
	int err;

	err = rtp_pcm_alloc(&rx, 7, 96, 1, 16);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	TASK_CHECK(push_samples(rx, 7, 96, 10, 0,
				(int16_t[]){1, 2}, 2, 1) == 0);
	TASK_CHECK(push_samples(rx, 7, 96, 12, 4,
				(int16_t[]){5, 6}, 2, 1) == 0);
	/* reordered seq 11 arrives later */
	TASK_CHECK(push_samples(rx, 7, 96, 11, 2,
				(int16_t[]){3, 4}, 2, 1) == 0);

	TASK_CHECK(rtp_pcm_read(rx, out, 6) == 0);
	TASK_CHECK(out[0] == 1 && out[1] == 2);
	TASK_CHECK(out[2] == 3 && out[3] == 4);
	TASK_CHECK(out[4] == 5 && out[5] == 6);

	mem_deref(rx);
	return 0;
}


/* Reset clears stream and candidate, config retained */
static int test_epochs_reset(void)
{
	struct rtp_pcm *rx = NULL;
	int16_t out[4];
	int err;

	err = rtp_pcm_alloc(&rx, 7, 96, 1, 16);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	TASK_CHECK(push_samples(rx, 7, 96, 10, 0,
				(int16_t[]){1, 2}, 2, 1) == 0);
	/* pending candidate */
	TASK_CHECK(push_samples(rx, 7, 96, 40000, 8000,
				(int16_t[]){9, 9}, 2, 1) == EAGAIN);

	rtp_pcm_reset(rx);

	/* read only silence after reset */
	TASK_CHECK(rtp_pcm_read(rx, out, 2) == 0);
	TASK_CHECK(out[0] == 0 && out[1] == 0);

	/* reset cleared the sequence state, so the next valid packet is a
	 * fresh first packet and anchors a new generation */
	TASK_CHECK(push_samples(rx, 7, 96, 3, 100,
				(int16_t[]){4, 5}, 2, 1) == 0);
	TASK_CHECK(rtp_pcm_read(rx, out, 2) == 0);
	TASK_CHECK(out[0] == 4 && out[1] == 5);

	mem_deref(rx);
	return 0;
}


/* Rejected window packet does not commit sequence state */
static int test_epochs_window_no_seq_commit(void)
{
	struct rtp_pcm *rx = NULL;
	int16_t out[4];
	int err;

	err = rtp_pcm_alloc(&rx, 7, 96, 1, 4);
	TASK_CHECK(err == 0);
	if (err)
		return 1;

	TASK_CHECK(push_samples(rx, 7, 96, 10, 0,
				(int16_t[]){1, 2}, 2, 1) == 0);
	/* ts beyond window (frame 4) -> ERANGE */
	TASK_CHECK(push_samples(rx, 7, 96, 11, 4,
				(int16_t[]){9, 10}, 2, 1) == ERANGE);
	/* sequence 11 was not committed, so seq 11 with an in-window
	 * timestamp is still accepted as in order */
	TASK_CHECK(push_samples(rx, 7, 96, 11, 2,
				(int16_t[]){3, 4}, 2, 1) == 0);

	TASK_CHECK(rtp_pcm_read(rx, out, 4) == 0);
	TASK_CHECK(out[0] == 1 && out[1] == 2);
	TASK_CHECK(out[2] == 3 && out[3] == 4);

	mem_deref(rx);
	return 0;
}


/* ------------------------------------------------------------------ */
/* Case table                                                          */
/* ------------------------------------------------------------------ */

const struct task_case task_cases[] = {
	/* timeline */
	{"timeline", "variable_stereo", test_timeline_variable_and_stereo},
	{"timeline", "overlap", test_timeline_overlap},
	{"timeline", "hole_silence", test_timeline_hole_silence},
	{"timeline", "late_reject", test_timeline_late_reject},
	{"timeline", "window_recovery", test_timeline_window_recovery},
	{"timeline", "legacy_mode", test_timeline_legacy_mode},
	{"timeline", "legacy_aubuf", test_legacy_aubuf_path},
	/* ingress */
	{"ingress", "basic", test_ingress_basic},
	{"ingress", "silence_before", test_ingress_silence_before},
	{"ingress", "reorder_overlap", test_ingress_reorder_overlap},
	{"ingress", "hole_and_late", test_ingress_hole_and_late},
	{"ingress", "ext_padding", test_ingress_ext_padding},
	{"ingress", "pad_bad", test_ingress_pad_bad},
	{"ingress", "bad_header", test_ingress_bad_header},
	{"ingress", "alloc_validation", test_ingress_alloc_validation},
	/* epochs */
	{"epochs", "seq_wrap", test_epochs_seq_wrap},
	{"epochs", "double_wrap", test_epochs_double_wrap},
	{"epochs", "ts_wrap", test_epochs_ts_wrap},
	{"epochs", "restart", test_epochs_restart},
	{"epochs", "restart_clears", test_epochs_restart_clears},
	{"epochs", "reorder_no_restart", test_epochs_reorder_no_restart},
	{"epochs", "reset", test_epochs_reset},
	{"epochs", "window_no_seq_commit", test_epochs_window_no_seq_commit},
};

const size_t task_case_count = RE_ARRAY_SIZE(task_cases);
