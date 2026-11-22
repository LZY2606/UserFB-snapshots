/**
 * @file re_jbuf.h  Interface to Jitter Buffer
 *
 * Copyright (C) 2010 Creytiv.com
 */
struct jbuf;
struct rtp_header;
struct mbuf;

/** Maximum slot window for the RED jitter buffer */
#define JBUF_RED_WINDOW_MAX 32768

/**
 * Configuration for an RFC 2198 RED capable jitter buffer.
 *
 * The jitter buffer is built around a single SSRC with a fixed sample rate
 * and a fixed number of timestamp ticks per packet (ptime).
 */
struct jbuf_red_cfg {
	uint32_t ssrc;         /**< SSRC of the stream                    */
	uint8_t  pt;           /**< Payload type of the normal audio      */
	uint8_t  red_pt;       /**< Payload type of the RED packets       */
	uint32_t rate;         /**< Sample rate in Hz (must be > 0)       */
	uint32_t ptime_ticks;  /**< RTP timestamp ticks per packet (>0)   */
	int64_t  delay_us;     /**< Playout delay in microseconds (>=0)   */
	uint32_t history_depth;/**< Max redundancy history in slots       */
	uint32_t window;       /**< Slot window, 1 .. 32767               */
};

/** Jitter buffer statistics */
struct jbuf_stat {
	uint32_t n_put;        /**< Number of frames put into jitter buffer */
	uint32_t n_get;        /**< Number of frames got from jitter buffer */
	uint32_t n_oos;        /**< Number of out-of-sequence frames        */
	uint32_t n_dups;       /**< Number of duplicate frames detected     */
	uint32_t n_late;       /**< Number of frames arriving too late      */
	uint32_t n_lost;       /**< Number of lost frames                   */
	uint32_t n_overflow;   /**< Number of overflows                     */
	uint32_t n_underflow;  /**< Number of underflows                    */
	uint32_t n_flush;      /**< Number of times jitter buffer flushed   */
};


int  jbuf_alloc(struct jbuf **jbp, uint32_t min, uint32_t max);
int  jbuf_put(struct jbuf *jb, const struct rtp_header *hdr, void *mem);
int  jbuf_get(struct jbuf *jb, struct rtp_header *hdr, void **mem);
void jbuf_flush(struct jbuf *jb);
int  jbuf_stats(const struct jbuf *jb, struct jbuf_stat *jstat);
int  jbuf_debug(struct re_printf *pf, const struct jbuf *jb);

/*
 * RFC 2198 RED audio path.
 *
 * The RED jitter buffer is a separate mode of the same jbuf object.  The
 * classic jbuf_alloc()/jbuf_put()/jbuf_get() API keeps its original
 * behaviour; mixing the two modes on one object returns EINVAL.
 */

int  jbuf_alloc_red(struct jbuf **jbp, const struct jbuf_red_cfg *cfg);
int  jbuf_put_red(struct jbuf *jb, const struct rtp_header *hdr,
		  struct mbuf *mb, uint64_t now_us);
int  jbuf_get_red(struct jbuf *jb, uint64_t now_us, struct rtp_header *hdr,
		  struct mbuf **mbp, bool *redundant);
