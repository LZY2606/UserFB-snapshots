/**
 * @file re_pcm.h  RTP L16 PCM receiver
 *
 * Copyright (C) 2026 Baresip Foundation (https://github.com/baresip)
 */

#ifndef RE_PCM_H__
#define RE_PCM_H__

#include <stddef.h>
#include <stdint.h>

struct mbuf;
struct rtp_pcm;

int  rtp_pcm_alloc(struct rtp_pcm **rxp, uint32_t ssrc, uint8_t pt,
		   uint8_t channels, size_t window_frames);
int  rtp_pcm_push(struct rtp_pcm *rx, struct mbuf *mb);
int  rtp_pcm_read(struct rtp_pcm *rx, int16_t *out, size_t frames);
void rtp_pcm_reset(struct rtp_pcm *rx);

#endif
