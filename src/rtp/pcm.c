/**
 * @file pcm.c  RTP L16 PCM receiver
 *
 * Receives RTP L16 (big-endian signed 16-bit) audio, handles network
 * reordering/loss and sender restarts, and feeds a continuous PCM stream
 * backed by the timeline mode of the audio buffer.
 *
 * Copyright (C) 2026 Baresip Foundation (https://github.com/baresip)
 */
#include <string.h>
#include <re.h>
#include <rem.h>
#include <re_pcm.h>


enum { TIMELINE_WINDOW_MAX = 65536 };


struct rtp_pcm {
	/* Configuration */
	uint32_t ssrc;
	uint8_t  pt;
	uint8_t  channels;
	size_t   window_frames;

	/* Sequence tracking (reused from source.c) */
	struct rtp_source seq;
	bool seq_started;

	/* Playback timeline */
	struct aubuf *ab;
	bool have_anchor;          /**< Timeline anchor is established     */
	uint32_t anchor_ts;        /**< RTP timestamp anchored to frame 0  */
	uint64_t front;            /**< Mirrors the aubuf consumption front */
};


static void pcm_destructor(void *data)
{
	struct rtp_pcm *rx = data;

	mem_deref(rx->ab);
}


/**
 * Allocate an RTP L16 PCM receiver.
 *
 * @param rxp           Pointer to allocated receiver
 * @param ssrc          Expected synchronization source
 * @param pt            Expected payload type (0..127)
 * @param channels      Number of interleaved channels (1 or 2)
 * @param window_frames Reorder window in sample frames (1..65536)
 *
 * @return 0 for success, otherwise error code
 */
int rtp_pcm_alloc(struct rtp_pcm **rxp, uint32_t ssrc, uint8_t pt,
		  uint8_t channels, size_t window_frames)
{
	struct rtp_pcm *rx;
	int err;

	if (!rxp || pt > 127 ||
	    (channels != 1 && channels != 2) ||
	    window_frames < 1 || window_frames > TIMELINE_WINDOW_MAX)
		return EINVAL;

	rx = mem_zalloc(sizeof(*rx), pcm_destructor);
	if (!rx)
		return ENOMEM;

	rx->ssrc          = ssrc;
	rx->pt            = pt;
	rx->channels      = channels;
	rx->window_frames = window_frames;

	err = aubuf_timeline_alloc(&rx->ab, channels, window_frames);
	if (err) {
		mem_deref(rx);
		return err;
	}

	*rxp = rx;

	return 0;
}


/**
 * Reset the receiver stream state.  Configuration is retained.
 *
 * Clears the PCM timeline, the anchor and the sequence state, including a
 * pending restart candidate.
 *
 * @param rx Receiver
 */
void rtp_pcm_reset(struct rtp_pcm *rx)
{
	if (!rx)
		return;

	memset(&rx->seq, 0, sizeof(rx->seq));
	rx->seq_started = false;

	rx->have_anchor = false;
	rx->anchor_ts   = 0;
	rx->front       = 0;

	aubuf_flush(rx->ab);
}


/* Convert the validated L16 payload into host samples and write it to the
 * timeline at the given absolute frame. */
static int write_payload(struct rtp_pcm *rx, const uint8_t *p, size_t nframes,
			 uint64_t frame)
{
	size_t nsamp = nframes * rx->channels;
	int16_t *sampv;
	int err;
	size_t i;

	sampv = mem_alloc(nsamp * sizeof(*sampv), NULL);
	if (!sampv)
		return ENOMEM;

	for (i = 0; i < nsamp; ++i) {
		uint16_t v = (uint16_t)((uint16_t)p[i * 2 + 0] << 8) |
			     (uint16_t)p[i * 2 + 1];
		sampv[i] = (int16_t)v;
	}

	err = aubuf_timeline_write(rx->ab, frame, sampv, nframes);

	mem_deref(sampv);

	return err;
}


/* Check whether a packet spanning [frame, frame+nframes) fits completely in
 * [front, front+window). */
static bool frame_in_window(const struct rtp_pcm *rx, uint64_t frame,
			    size_t nframes)
{
	if (nframes > rx->window_frames)
		return false;

	if (frame < rx->front)
		return false;

	return frame - rx->front <= rx->window_frames - nframes;
}


/**
 * Push one received RTP packet.
 *
 * The RTP header, version, SSRC/PT, CSRC list, extension and padding are
 * fully validated.  Format or parameter errors (EINVAL/EBADMSG) leave the
 * mbuf position, the sequence state and the playback state untouched.
 *
 * @param rx Receiver
 * @param mb Received RTP packet (position at the start of the header)
 *
 * @return 0 if the packet was accepted, EAGAIN if it is an isolated large
 *         sequence jump (restart candidate kept), ERANGE if it was already
 *         consumed or falls outside the window, otherwise an error code.
 */
int rtp_pcm_push(struct rtp_pcm *rx, struct mbuf *mb)
{
	struct rtp_header hdr;
	struct rtp_source tmp;
	enum rtp_seq_event ev;
	size_t saved_pos, saved_end;
	const uint8_t *p;
	size_t payload_len, nframes;
	int64_t frame = 0;
	int err;

	if (!rx || !mb)
		return EINVAL;

	saved_pos = mb->pos;
	saved_end = mb->end;

	err = rtp_hdr_decode(&hdr, mb);
	if (err)
		goto bad;

	if (hdr.ver != RTP_VERSION || hdr.ssrc != rx->ssrc || hdr.pt != rx->pt) {
		err = EBADMSG;
		goto out;
	}

	payload_len = mb->end - mb->pos;
	p = mb->buf + mb->pos;

	if (hdr.pad) {
		size_t pad;

		if (payload_len < 1) {
			err = EBADMSG;
			goto out;
		}

		pad = p[payload_len - 1];
		if (pad < 1 || pad > payload_len) {
			err = EBADMSG;
			goto out;
		}

		payload_len -= pad;
	}

	/* L16 must be non-empty after padding and contain whole sample frames */
	if (payload_len == 0 ||
	    payload_len % ((size_t)rx->channels * sizeof(int16_t))) {
		err = EBADMSG;
		goto out;
	}

	nframes = payload_len / ((size_t)rx->channels * sizeof(int16_t));

	/* First valid packet anchors frame 0 of the timeline */
	if (!rx->seq_started) {

		if (nframes > rx->window_frames) {
			err = ERANGE;
			goto out;
		}

		err = write_payload(rx, p, nframes, 0);
		if (err)
			goto out;

		rtp_source_init_seq(&rx->seq, hdr.seq);
		rx->seq_started = true;
		rx->have_anchor = true;
		rx->anchor_ts   = hdr.ts;
		rx->front       = 0;

		goto out;
	}

	/* Evaluate the sequence update on a scratch copy so that rejected
	 * packets do not commit any sequence state. */
	tmp = rx->seq;
	ev = rtp_source_update_seq_ex(&tmp, hdr.seq);

	switch (ev) {

	case RTP_SEQ_DROP:
		/* Isolated large jump: keep the restart candidate but do not
		 * disturb the audio currently being played. */
		rx->seq = tmp;
		err = EAGAIN;
		goto out;

	case RTP_SEQ_RESTART:
		/* Two sequential packets confirm a sender restart.  Drop the
		 * old PCM, anchor and consumption front, then start a new
		 * generation from this packet. */
		if (nframes > rx->window_frames) {
			err = ERANGE;
			goto out;
		}

		aubuf_flush(rx->ab);

		err = write_payload(rx, p, nframes, 0);
		if (err)
			goto out;

		rx->seq       = tmp;
		rx->have_anchor = true;
		rx->anchor_ts = hdr.ts;
		rx->front     = 0;
		goto out;

	case RTP_SEQ_ACCEPT:
	default:
		break;
	}

	/* Map the RTP timestamp to a frame on the current generation.  A
	 * signed 32-bit difference handles timestamp wrap-around; a single
	 * generation spans less than half a turn. */
	frame = (int64_t)(int32_t)(hdr.ts - rx->anchor_ts);

	if (frame < 0 || !frame_in_window(rx, (uint64_t)frame, nframes)) {
		err = ERANGE;
		goto out;
	}

	err = write_payload(rx, p, nframes, (uint64_t)frame);
	if (err)
		goto out;

	rx->seq = tmp;

	goto out;

 bad:
	err = EBADMSG;

 out:
	mb->pos = saved_pos;
	mb->end = saved_end;

	return err;
}


/**
 * Read a continuous PCM stream.
 *
 * Up to `frames` sample frames are produced.  Missing samples are output as
 * silence.  Before the first packet is received this outputs silence and
 * does not establish an anchor.
 *
 * @param rx    Receiver
 * @param out   Output buffer (frames * channels interleaved samples)
 * @param frames Number of sample frames to read
 *
 * @return 0 for success, otherwise error code
 */
int rtp_pcm_read(struct rtp_pcm *rx, int16_t *out, size_t frames)
{
	int err;

	if (!rx || (!out && frames))
		return EINVAL;

	if (frames > rx->window_frames)
		return EINVAL;

	if (!rx->have_anchor) {
		if (out)
			memset(out, 0, frames * rx->channels * sizeof(*out));
		return 0;
	}

	err = aubuf_timeline_read(rx->ab, out, frames);
	if (err)
		return err;

	rx->front += frames;

	return 0;
}
