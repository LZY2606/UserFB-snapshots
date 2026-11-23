/**
 * @file source.c  Real-time Transport Control Protocol source
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re_types.h>
#include <re_fmt.h>
#include <re_mem.h>
#include <re_mbuf.h>
#include <re_list.h>
#include <re_hash.h>
#include <re_sa.h>
#include <re_rtp.h>
#include "rtcp.h"


enum {
	RTP_SEQ_MOD = 1<<16,
};

static enum rtp_seq_event update_seq(struct rtp_source *s, uint16_t seq)
{
	uint16_t udelta;
	const int MAX_DROPOUT = 3000;
	const int MAX_MISORDER = 100;
	const int MIN_SEQUENTIAL = 2;

	if (!s)
		return RTP_SEQ_DROP;

	udelta = seq - s->max_seq;

	/*
	 * Source is not valid until MIN_SEQUENTIAL packets with
	 * sequential sequence numbers have been received.
	 */
	if (s->probation) {

		/* packet is in sequence */
		if (seq == s->max_seq + 1) {
			s->probation--;
			s->max_seq = seq;
			if (s->probation == 0) {
				rtp_source_init_seq(s, seq);
				s->received++;
				return RTP_SEQ_ACCEPT;
			}
		}
		else {
			s->probation = MIN_SEQUENTIAL - 1;
			s->max_seq = seq;
		}
		return RTP_SEQ_DROP;
	}
	else if (udelta < MAX_DROPOUT) {

		/* in order, with permissible gap */
		if (seq < s->max_seq) {
			/*
			 * Sequence number wrapped - count another 64K cycle.
			 */
			s->cycles += RTP_SEQ_MOD;
		}
		s->max_seq = seq;
	}
	else if (udelta <= RTP_SEQ_MOD - MAX_MISORDER) {

		/* the sequence number made a very large jump */
		if (seq == s->bad_seq) {
			/*
			 * Two sequential packets -- assume that the other side
			 * restarted without telling us so just re-sync
			 * (i.e., pretend this was the first packet).
			 */
			rtp_source_init_seq(s, seq);
			return RTP_SEQ_RESTART;
		}
		else {
			s->bad_seq = (seq + 1) & (RTP_SEQ_MOD-1);
			return RTP_SEQ_DROP;
		}
	}
	else {
		/* duplicate or reordered packet */
	}

	s->received++;
	return RTP_SEQ_ACCEPT;
}


void rtp_source_init_seq(struct rtp_source *s, uint16_t seq)
{
	if (!s)
		return;

	s->base_seq = seq;
	s->max_seq = seq;
	s->bad_seq = RTP_SEQ_MOD + 1;   /* so seq == bad_seq is false */
	s->cycles = 0;
	s->received = 0;
	s->received_prior = 0;
	s->expected_prior = 0;
	/* other initialization */
}


/*
 * See RFC 3550 - A.1 RTP Data Header Validity Checks
 */
int rtp_source_update_seq(struct rtp_source *s, uint16_t seq)
{
	return update_seq(s, seq) != RTP_SEQ_DROP;
}


/*
 * Extended variant of rtp_source_update_seq() which also reports a
 * detected sender restart.  The return value is one of enum rtp_seq_event.
 */
enum rtp_seq_event rtp_source_update_seq_ex(struct rtp_source *s, uint16_t seq)
{
	return update_seq(s, seq);
}


/* RFC 3550 A.8
 *
 * The inputs are:
 *
 *     rtp_ts:  the timestamp from the incoming RTP packet
 *     arrival: the current time in the same units.
 */
void rtp_source_calc_jitter(struct rtp_source *s, uint32_t rtp_ts,
			uint32_t arrival)
{
	const int transit = arrival - rtp_ts;
	int d = transit - s->transit;

	if (!s->transit) {
		s->transit = transit;
		return;
	}

	s->transit = transit;

	if (d < 0)
		d = -d;

	s->jitter += d - ((s->jitter + 8) >> 4);
}


/* A.3 */
int rtp_source_calc_lost(const struct rtp_source *s)
{
	int extended_max = s->cycles + s->max_seq;
	int expected = extended_max - s->base_seq + 1;
	int lost;

	lost = expected - s->received;

	/* Clamp at 24 bits */
	if (lost > 0x7fffff)
		lost = 0x7fffff;
	else if (lost < -0x7fffff)
		lost = -0x7fffff;

	return lost;
}


/* A.3 */
uint8_t rtp_source_calc_fraction_lost(struct rtp_source *s)
{
	int extended_max = s->cycles + s->max_seq;
	int expected = extended_max - s->base_seq + 1;
	int expected_interval = expected - s->expected_prior;
	int received_interval;
	int lost_interval;
	uint8_t fraction;

	s->expected_prior = expected;

	received_interval = s->received - s->received_prior;

	s->received_prior = s->received;

	lost_interval = expected_interval - received_interval;

	if (expected_interval == 0 || lost_interval <= 0)
		fraction = 0;
	else
		fraction = (lost_interval << 8) / expected_interval;

	return fraction;
}
