/**
 * @file pcm_replay.c  RTP L16 PCM receiver example
 *
 * Synthesizes RTP L16 packets in memory, feeds them through the public
 * rtp_pcm receiver and prints the decoded PCM samples.  No sound card or
 * network is used.
 */
#include <stdio.h>
#include <string.h>
#include <re.h>
#include <rem.h>
#include <re_pcm.h>


#define SSRC 0x12345678u
#define PT   96


static int make_rtp(struct mbuf **mbp, uint16_t seq, uint32_t ts,
		    const int16_t *sampv, size_t frames, uint8_t channels)
{
	struct rtp_header h = {0};
	struct mbuf *mb;
	size_t i, nsamp = frames * channels;

	mb = mbuf_alloc(64 + nsamp * 2);
	if (!mb)
		return ENOMEM;

	h.ver  = RTP_VERSION;
	h.pt   = PT;
	h.seq  = seq;
	h.ts   = ts;
	h.ssrc = SSRC;

	if (rtp_hdr_encode(mb, &h)) {
		mem_deref(mb);
		return EINVAL;
	}

	for (i = 0; i < nsamp; ++i) {
		uint16_t v = (uint16_t)sampv[i];

		mbuf_write_u8(mb, (uint8_t)(v >> 8));
		mbuf_write_u8(mb, (uint8_t)(v & 0xff));
	}

	mb->pos = 0;
	*mbp = mb;

	return 0;
}


static void push(struct rtp_pcm *rx, uint16_t seq, uint32_t ts,
		 const int16_t *sampv, size_t frames, uint8_t channels)
{
	struct mbuf *mb = NULL;

	if (make_rtp(&mb, seq, ts, sampv, frames, channels))
		return;

	(void)rtp_pcm_push(rx, mb);
	mem_deref(mb);
}


int main(void)
{
	struct rtp_pcm *rx = NULL;
	int16_t stereo[6] = {1000, -1000, 2000, -2000, 3000, -3000};
	int16_t mono[6]   = {10, 20, 30, 40, 50, 60};
	int16_t out[8];
	size_t i;
	int err;

	/* --- Mono example --- */
	err = rtp_pcm_alloc(&rx, SSRC, PT, 1, 64);
	if (err) {
		fprintf(stderr, "rtp_pcm_alloc: %d\n", err);
		return 1;
	}

	/* Packet loss in the middle: seq 0, then seq 2 (frames 4..5). */
	push(rx, 0, 0, mono, 2, 1);          /* frames 0..1 */
	push(rx, 2, 4, &mono[4], 2, 1);      /* frames 4..5 */

	printf("mono playback (6 frames, gap at 2..3 -> silence):\n");
	if (rtp_pcm_read(rx, out, 6))
		return 1;
	for (i = 0; i < 6; ++i)
		printf("  pcm[%zu] = %d\n", i, out[i]);

	mem_deref(rx);

	/* --- Stereo example with sender restart --- */
	err = rtp_pcm_alloc(&rx, SSRC, PT, 2, 64);
	if (err) {
		fprintf(stderr, "rtp_pcm_alloc: %d\n", err);
		return 1;
	}

	printf("stereo playback:\n");
	push(rx, 0, 0, stereo, 3, 2);        /* frames 0..2 */
	if (rtp_pcm_read(rx, out, 3))
		return 1;
	for (i = 0; i < 3; ++i)
		printf("  frame[%zu] = %d, %d\n", i,
		       out[i * 2 + 0], out[i * 2 + 1]);

	/* A large sequence jump is only a candidate ... */
	push(rx, 40000, 9000, stereo, 1, 2);
	/* ... a second sequential packet confirms the restart. */
	push(rx, 40001, 9001, &stereo[2], 2, 2);

	printf("after restart (new generation):\n");
	if (rtp_pcm_read(rx, out, 2))
		return 1;
	for (i = 0; i < 2; ++i)
		printf("  frame[%zu] = %d, %d\n", i,
		       out[i * 2 + 0], out[i * 2 + 1]);

	mem_deref(rx);

	return 0;
}
