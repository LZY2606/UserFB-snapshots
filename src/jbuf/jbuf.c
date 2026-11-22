/**
 * @file jbuf.c  Jitter Buffer implementation
 *
 * Copyright (C) 2010 Creytiv.com
 */
#include <string.h>
#include <re_types.h>
#include <re_fmt.h>
#include <re_list.h>
#include <re_mbuf.h>
#include <re_mem.h>
#include <re_rtp.h>
#include <re_jbuf.h>


#define DEBUG_MODULE "jbuf"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


#ifndef RELEASE
#define JBUF_STAT 1  /**< Jitter buffer statistics */
#endif


#if JBUF_STAT
#define STAT_ADD(var, value)  (jb->stat.var) += (value) /**< Stats add */
#define STAT_INC(var)         ++(jb->stat.var)          /**< Stats inc */
#else
#define STAT_ADD(var, value)
#define STAT_INC(var)
#endif


/** Defines a packet frame */
struct frame {
	struct le le;           /**< Linked list element       */
	struct rtp_header hdr;  /**< RTP Header                */
	void *mem;              /**< Reference counted pointer */
};


/** Jitter buffer mode */
enum jbuf_mode {
	JBUF_MODE_CLASSIC = 0,  /**< Classic jbuf_alloc() mode  */
	JBUF_MODE_RED           /**< RFC 2198 RED audio mode    */
};


/** One playout slot in the RED jitter buffer */
struct red_slot {
	bool used;              /**< Slot contains audio        */
	bool from_red;          /**< Audio recovered from RED   */
	int64_t k;              /**< Slot index relative anchor */
	struct rtp_header hdr;  /**< RTP Header for the slot    */
	void *mem;              /**< Reference counted mbuf     */
};


/** A validated block waiting to be committed */
struct red_pending {
	int64_t k;              /**< Slot index                 */
	bool from_red;          /**< Recovered from redundancy  */
	struct rtp_header hdr;  /**< Header for the slot        */
	void *mem;              /**< Reference counted mbuf     */
};


/**
 * Defines a jitter buffer
 *
 * The jitter buffer is for incoming RTP packets, which are sorted by
 * sequence number.
 */
struct jbuf {
	struct list pooll;   /**< List of free frames in pool               */
	struct list framel;  /**< List of buffered frames                   */
	uint32_t n;          /**< [# frames] Current # of frames in buffer  */
	uint32_t min;        /**< [# frames] Minimum # of frames to buffer  */
	uint32_t max;        /**< [# frames] Maximum # of frames to buffer  */
	uint16_t seq_put;    /**< Sequence number for last jbuf_put()       */
	bool running;        /**< Jitter buffer is running                  */

	enum jbuf_mode mode; /**< Operation mode of the buffer              */
	struct {
		struct jbuf_red_cfg cfg; /**< Configuration              */
		bool     anchored;   /**< Anchor packet is established   */
		uint16_t seq0;       /**< Anchor sequence number         */
		uint32_t ts0;        /**< Anchor timestamp               */
		uint64_t at0;        /**< Anchor arrival time in [us]    */
		int64_t  front_k;    /**< Highest main packet slot seen  */
		int64_t  play_k;     /**< Next slot to play out          */
		struct red_slot *slotv; /**< Slot window                 */
		struct rtp_red_block *blockv; /**< RED decode scratch     */
		struct red_pending *pendv; /**< Commit scratch           */
	} red;

#if JBUF_STAT
	uint16_t seq_get;      /**< Timestamp of last played frame */
	struct jbuf_stat stat; /**< Jitter buffer Statistics       */
#endif
};


/** Is x less than y? */
static inline bool seq_less(uint16_t x, uint16_t y)
{
	return ((int16_t)(x - y)) < 0;
}


/**
 * Get a frame from the pool
 */
static void frame_alloc(struct jbuf *jb, struct frame **f)
{
	struct le *le;

	le = jb->pooll.head;
	if (le) {
		list_unlink(le);
		++jb->n;
	}
	else {
		struct frame *f0;

		/* Steal an old frame */
		le = jb->framel.head;
		f0 = le->data;

		STAT_INC(n_overflow);
		DEBUG_INFO("drop 1 old frame seq=%u (total dropped %u)\n",
			   f0->hdr.seq, jb->stat.n_overflow);

		f0->mem = mem_deref(f0->mem);
		list_unlink(le);
	}

	*f = le->data;
}


/**
 * Release a frame, put it back in the pool
 */
static void frame_deref(struct jbuf *jb, struct frame *f)
{
	f->mem = mem_deref(f->mem);
	list_unlink(&f->le);
	list_append(&jb->pooll, &f->le, f);
	--jb->n;
}


static void jbuf_destructor(void *data)
{
	struct jbuf *jb = data;

	jbuf_flush(jb);

	/* Free all frames in the pool list */
	list_flush(&jb->pooll);

	if (JBUF_MODE_RED == jb->mode) {
		mem_deref(jb->red.slotv);
		mem_deref(jb->red.blockv);
		mem_deref(jb->red.pendv);
	}
}


/**
 * Allocate a new jitter buffer
 *
 * @param jbp  Pointer to returned jitter buffer
 * @param min  Minimum delay in [frames]
 * @param max  Maximum delay in [frames]
 *
 * @return 0 if success, otherwise errorcode
 */
int jbuf_alloc(struct jbuf **jbp, uint32_t min, uint32_t max)
{
	struct jbuf *jb;
	uint32_t i;
	int err = 0;

	if (!jbp || ( min > max))
		return EINVAL;

	DEBUG_INFO("alloc: delay=%u-%u frames\n", min, max);

	/* self-test: x < y (also handle wrap around) */
	if (!seq_less(10, 20) || seq_less(20, 10) || !seq_less(65535, 0)) {
		DEBUG_WARNING("seq_less() is broken\n");
		return ENOSYS;
	}

	jb = mem_zalloc(sizeof(*jb), jbuf_destructor);
	if (!jb)
		return ENOMEM;

	list_init(&jb->pooll);
	list_init(&jb->framel);

	jb->min  = min;
	jb->max  = max;

	/* Allocate all frames now */
	for (i=0; i<jb->max; i++) {
		struct frame *f = mem_zalloc(sizeof(*f), NULL);
		if (!f) {
			err = ENOMEM;
			break;
		}

		list_append(&jb->pooll, &f->le, f);
		DEBUG_INFO("alloc: adding to pool list %u\n", i);
	}

	if (err)
		mem_deref(jb);
	else
		*jbp = jb;

	return err;
}


/**
 * Put one frame into the jitter buffer
 *
 * @param jb   Jitter buffer
 * @param hdr  RTP Header
 * @param mem  Memory pointer - will be referenced
 *
 * @return 0 if success, otherwise errorcode
 */
int jbuf_put(struct jbuf *jb, const struct rtp_header *hdr, void *mem)
{
	struct frame *f;
	struct le *le, *tail;
	uint16_t seq;
	int err = 0;

	if (!jb || !hdr || JBUF_MODE_CLASSIC != jb->mode)
		return EINVAL;

	seq = hdr->seq;

	STAT_INC(n_put);

	if (jb->running) {

		/* Packet arrived too late to be put into buffer */
		if (seq_less((seq + jb->n), jb->seq_put)) {
			STAT_INC(n_late);
			DEBUG_INFO("packet too late: seq=%u (seq_put=%u)\n",
				   seq, jb->seq_put);
			return ETIMEDOUT;
		}
	}

	frame_alloc(jb, &f);

	tail = jb->framel.tail;

	/* If buffer is empty -> append to tail
	   Frame is later than tail -> append to tail
	*/
	if (!tail || seq_less(((struct frame *)tail->data)->hdr.seq, seq)) {
		list_append(&jb->framel, &f->le, f);
		goto out;
	}

	/* Out-of-sequence, find right position */
	for (le = tail; le; le = le->prev) {
		const uint16_t seq_le = ((struct frame *)le->data)->hdr.seq;

		if (seq_less(seq_le, seq)) { /* most likely */
			DEBUG_INFO("put: out-of-sequence"
				   " - inserting after seq=%u (seq=%u)\n",
				   seq_le, seq);
			list_insert_after(&jb->framel, le, &f->le, f);
			break;
		}
		else if (seq == seq_le) { /* less likely */
			/* Detect duplicates */
			DEBUG_INFO("duplicate: seq=%u\n", seq);
			STAT_INC(n_dups);
			list_insert_after(&jb->framel, le, &f->le, f);
			frame_deref(jb, f);
			return EALREADY;
		}

		/* sequence number less than current seq, continue */
	}

	/* no earlier timestamps found, put in head */
	if (!le) {
		DEBUG_INFO("put: out-of-sequence"
			   " - put in head (seq=%u)\n", seq);
		list_prepend(&jb->framel, &f->le, f);
	}

	STAT_INC(n_oos);

 out:
	/* Update last timestamp */
	jb->running = true;
	jb->seq_put = seq;

	/* Success */
	f->hdr = *hdr;
	f->mem = mem_ref(mem);

	return err;
}


/**
 * Get one frame from the jitter buffer
 *
 * @param jb   Jitter buffer
 * @param hdr  Returned RTP Header
 * @param mem  Pointer to memory object storage - referenced on success
 *
 * @return 0 if success, otherwise errorcode
 */
int jbuf_get(struct jbuf *jb, struct rtp_header *hdr, void **mem)
{
	struct frame *f;

	if (!jb || !hdr || !mem || JBUF_MODE_CLASSIC != jb->mode)
		return EINVAL;

	STAT_INC(n_get);

	if (jb->n <= jb->min || !jb->framel.head) {
		DEBUG_INFO("not enough buffer frames - wait.. (n=%u min=%u)\n",
			   jb->n, jb->min);
		STAT_INC(n_underflow);
		return ENOENT;
	}

	/* When we get one frame F[i], check that the next frame F[i+1]
	   is present and have a seq no. of seq[i] + 1 !
	   if not, we should consider that packet lost */

	f = jb->framel.head->data;

#if JBUF_STAT
	/* Check timestamp of previously played frame */
	if (jb->seq_get) {
		const int16_t seq_diff = f->hdr.seq - jb->seq_get;
		if (seq_less(f->hdr.seq, jb->seq_get)) {
			DEBUG_WARNING("get: seq=%u too late\n", f->hdr.seq);
		}
		else if (seq_diff > 1) {
			STAT_ADD(n_lost, 1);
			DEBUG_INFO("get: n_lost: diff=%d,seq=%u,seq_get=%u\n",
				   seq_diff, f->hdr.seq, jb->seq_get);
		}
	}

	/* Update sequence number for 'get' */
	jb->seq_get = f->hdr.seq;
#endif

	*hdr = f->hdr;
	*mem = mem_ref(f->mem);

	frame_deref(jb, f);

	return 0;
}


/**
 * Flush all frames in the jitter buffer
 *
 * @param jb   Jitter buffer
 */
void jbuf_flush(struct jbuf *jb)
{
	struct le *le;
#if JBUF_STAT
	uint32_t n_flush;
#endif

	if (!jb)
		return;

	if (JBUF_MODE_RED == jb->mode && jb->red.slotv) {
		uint32_t i;

		for (i = 0; i < jb->red.cfg.window; ++i) {
			jb->red.slotv[i].mem = mem_deref(jb->red.slotv[i].mem);
			jb->red.slotv[i].used = false;
			jb->red.slotv[i].from_red = false;
		}

		/* Rebuild the first-packet anchor; keep configuration */
		jb->red.anchored = false;
		jb->red.seq0     = 0;
		jb->red.ts0      = 0;
		jb->red.at0      = 0;
		jb->red.front_k  = 0;
		jb->red.play_k   = 0;

#if JBUF_STAT
		n_flush = STAT_INC(n_flush);
		jb->seq_get = 0;
		memset(&jb->stat, 0, sizeof(jb->stat));
		jb->stat.n_flush = n_flush;
#endif
		return;
	}

	if (jb->framel.head) {
		DEBUG_INFO("flush: %u frames\n", jb->n);
	}

	/* put all buffered frames back in free list */
	for (le = jb->framel.head; le; le = jb->framel.head) {
		DEBUG_INFO(" flush frame: seq=%u\n",
			   ((struct frame *)(le->data))->hdr.seq);

		frame_deref(jb, le->data);
	}

	jb->n       = 0;
	jb->running = false;

#if JBUF_STAT
	n_flush = STAT_INC(n_flush);
	jb->seq_get = 0;
	memset(&jb->stat, 0, sizeof(jb->stat));
	jb->stat.n_flush = n_flush;
#endif

}


/** Compare two RTP sequence numbers in a 32768 wide window */
static int64_t seq_diff(uint16_t a, uint16_t b)
{
	return (int64_t)(int16_t)(uint16_t)(a - b);
}


/** Compare two RTP timestamps in a 2^31 wide window */
static int64_t ts_diff(uint32_t a, uint32_t b)
{
	return (int64_t)(int32_t)(uint32_t)(a - b);
}


/** Test if a slot index is inside the configured window */
static bool red_k_ok(const struct jbuf *jb, int64_t k)
{
	return k >= 0 && k < (int64_t)jb->red.cfg.window;
}


/** Build an independent audio copy for one playout slot */
static int red_pend_add(struct jbuf *jb, struct red_pending *p,
			const struct rtp_header *base, int64_t k, bool from_red,
			struct mbuf *mb, size_t off, size_t len)
{
	struct mbuf *m;

	p->mem = NULL;

	if (!len)
		return EBADMSG;

	/* off is relative to the start of the RTP payload, which is at the
	   current mbuf position */
	if (off > mbuf_get_left(mb) || len > mbuf_get_left(mb) - off)
		return EBADMSG;

	m = mbuf_alloc(len);
	if (!m)
		return ENOMEM;

	if (mbuf_write_mem(m, mbuf_buf(mb) + off, len)) {
		mem_deref(m);
		return ENOMEM;
	}
	m->pos = 0;

	p->hdr = *base;
	p->hdr.pt   = jb->red.cfg.pt;
	p->hdr.ssrc = jb->red.cfg.ssrc;

	if (from_red) {
		p->hdr.m   = false;
		p->hdr.seq = (uint16_t)(jb->red.seq0 + k);
		p->hdr.ts  = jb->red.ts0 +
			(uint32_t)((uint64_t)k * jb->red.cfg.ptime_ticks);
	}

	p->k        = k;
	p->from_red = from_red;
	p->mem      = m;

	return 0;
}


/** Store one pending block, honouring playout precedence */
static void red_commit_one(struct jbuf *jb, struct red_pending *p)
{
	struct red_slot *slot;

	if (!red_k_ok(jb, p->k))
		goto discard;

	if (p->k < jb->red.play_k)
		goto discard;

	slot = &jb->red.slotv[p->k];

	if (slot->used) {
		/* A primary copy always wins over a recovered copy; between
		 * copies of the same class the first accepted copy wins. */
		if (!(slot->from_red && !p->from_red))
			goto discard;

		slot->mem = mem_deref(slot->mem);
	}

	slot->hdr      = p->hdr;
	slot->mem      = p->mem;
	slot->used     = true;
	slot->from_red = p->from_red;
	slot->k        = p->k;
	p->mem         = NULL;
	return;

 discard:
	p->mem = mem_deref(p->mem);
}


/**
 * Allocate a jitter buffer for RFC 2198 RED audio
 *
 * The buffer keeps a fixed slot window anchored on the first accepted
 * primary block.  Slot k corresponds to sequence number S+k and timestamp
 * T+k*ptime_ticks and becomes due at
 * A + delay_us + floor(k*ptime_ticks*1000000/rate) microseconds.
 *
 * @param jbp Pointer to returned jitter buffer
 * @param cfg RED configuration, copied into the buffer
 *
 * @return 0 if success, otherwise errorcode
 */
int jbuf_alloc_red(struct jbuf **jbp, const struct jbuf_red_cfg *cfg)
{
	struct jbuf *jb;
	uint64_t span;
	int err;

	if (!jbp || !cfg)
		return EINVAL;

	/* rate, ptime and window must be positive */
	if (!cfg->rate || !cfg->ptime_ticks || !cfg->window)
		return EINVAL;

	/* playout delay is non-negative */
	if (cfg->delay_us < 0)
		return EINVAL;

	/* both payload types must differ and be in range 0..127 */
	if (cfg->pt > 0x7f || cfg->red_pt > 0x7f || cfg->pt == cfg->red_pt)
		return EINVAL;

	/* the slot window is strictly less than 32768 */
	if (cfg->window >= JBUF_RED_WINDOW_MAX)
		return EINVAL;

	/* history depth is 0 .. window-1 */
	if (cfg->history_depth > cfg->window - 1)
		return EINVAL;

	/* the sequence span stays below 32768 and the timestamp span below
	   2^31, so the signed sequence/timestamp differences stay valid and
	   the playout time computation fits in 64 bits */
	span = (uint64_t)cfg->window * cfg->ptime_ticks;
	if (span > 0x7fffffffULL || span > ((uint64_t)-1) / 1000000ULL)
		return EINVAL;

	jb = mem_zalloc(sizeof(*jb), jbuf_destructor);
	if (!jb)
		return ENOMEM;

	list_init(&jb->pooll);
	list_init(&jb->framel);

	jb->mode    = JBUF_MODE_RED;
	jb->red.cfg = *cfg;

	jb->red.slotv = mem_zalloc((size_t)cfg->window * sizeof(*jb->red.slotv),
				   NULL);
	jb->red.blockv = mem_zalloc(((size_t)cfg->history_depth + 1) *
				    sizeof(*jb->red.blockv), NULL);
	jb->red.pendv = mem_zalloc(((size_t)cfg->history_depth + 2) *
				   sizeof(*jb->red.pendv), NULL);
	if (!jb->red.slotv || !jb->red.blockv || !jb->red.pendv) {
		err = ENOMEM;
		goto out;
	}

	*jbp = jb;
	err  = 0;

 out:
	if (err)
		mem_deref(jb);

	return err;
}


/**
 * Put an RTP packet into a RED jitter buffer
 *
 * The packet may carry a normal audio payload (payload type cfg.pt) or an
 * RFC 2198 RED payload (payload type cfg.red_pt).  The mbuf must be
 * positioned at the start of the RTP payload.  On success the payload is
 * copied, so the caller may release or reuse the input buffer.
 *
 * @param jb     Jitter buffer
 * @param hdr    RTP header
 * @param mb     Buffer with the RTP payload
 * @param now_us Monotonic time in microseconds
 *
 * @return 0 if success, otherwise errorcode
 */
int jbuf_put_red(struct jbuf *jb, const struct rtp_header *hdr,
		 struct mbuf *mb, uint64_t now_us)
{
	struct rtp_red red;
	struct rtp_red_block primary;
	const struct rtp_red_block *blockv;
	struct red_pending *pendv;
	size_t nblk, i, npend = 0, j;
	int64_t main_k;
	bool first;
	int err = 0;
	if (!jb || !hdr || !mb || JBUF_MODE_RED != jb->mode)
		return EINVAL;

	if (hdr->ssrc != jb->red.cfg.ssrc)
		return EINVAL;

	if (hdr->pt != jb->red.cfg.pt && hdr->pt != jb->red.cfg.red_pt)
		return EINVAL;

	first = !jb->red.anchored;

	if (hdr->pt == jb->red.cfg.pt) {

		/* Normal audio packet: a single primary block */
		primary.pt        = hdr->pt;
		primary.ts_offset = 0;
		primary.off       = 0;
		primary.len       = mbuf_get_left(mb);
		if (!primary.len)
			return EBADMSG;

		blockv = &primary;
		nblk   = 1;
	}
	else {
		/* RFC 2198 RED packet, fully validated before use */
		err = rtp_red_decode(&red, mb, jb->red.blockv,
				     jb->red.cfg.history_depth);
		if (err)
			return err;

		if (!red.n)
			return EBADMSG;

		/* The final block is the primary block */
		if (red.blockv[red.n - 1].pt != jb->red.cfg.pt)
			return EBADMSG;
		if (red.blockv[red.n - 1].ts_offset != 0)
			return EBADMSG;

		blockv = red.blockv;
		nblk   = red.n;
	}

	/* Validate the primary block position on the fixed timeline */
	if (first) {
		main_k = 0;
	}
	else {
		main_k = seq_diff(hdr->seq, jb->red.seq0);

		/* Beyond the configured slot window */
		if (main_k >= (int64_t)jb->red.cfg.window)
			return EINVAL;

		/* Must sit exactly on the fixed-step timeline */
		if (ts_diff(hdr->ts, jb->red.ts0) !=
		    main_k * (int64_t)jb->red.cfg.ptime_ticks)
			return EINVAL;

		/* Before the first slot or already played out */
		if (main_k < jb->red.play_k)
			return ETIMEDOUT;
	}

	pendv = jb->red.pendv;

	/* The primary block is committed first.  An empty primary block is
	   never accepted. */
	err = red_pend_add(jb, &pendv[npend], hdr, main_k, false, mb,
			   blockv[nblk - 1].off, blockv[nblk - 1].len);
	if (err)
		goto fail;
	++npend;

	/* Validate every redundant block before committing anything */
	for (i = 0; i + 1 < nblk; ++i) {
		const struct rtp_red_block *b = &blockv[i];
		uint32_t ptime = jb->red.cfg.ptime_ticks;
		int64_t k_red;

		if (b->pt != jb->red.cfg.pt)
			goto inval;
		if (!b->len)
			goto inval;

		/* offset must be a positive multiple of ptime */
		if (!b->ts_offset || (b->ts_offset % ptime))
			goto inval;
		if (b->ts_offset / ptime > jb->red.cfg.history_depth)
			goto inval;

		k_red = main_k - (int64_t)(b->ts_offset / ptime);

		/* Before the first slot or already played out: ignore */
		if (k_red < 0)
			continue;
		if (k_red < jb->red.play_k)
			continue;
		if (!red_k_ok(jb, k_red))
			continue;

		err = red_pend_add(jb, &pendv[npend], hdr, k_red, true, mb,
				   b->off, b->len);
		if (err)
			goto fail;
		++npend;
	}

	/* Establish the anchor only after the whole packet is valid */
	if (first) {
		uint64_t maxoff, base;

		maxoff = (uint64_t)(jb->red.cfg.window - 1) *
			jb->red.cfg.ptime_ticks * 1000000ULL / jb->red.cfg.rate;

		if ((uint64_t)jb->red.cfg.delay_us > ((uint64_t)-1) - now_us)
			goto inval;
		base = now_us + (uint64_t)jb->red.cfg.delay_us;
		if (maxoff > ((uint64_t)-1) - base)
			goto inval;

		jb->red.seq0     = hdr->seq;
		jb->red.ts0      = hdr->ts;
		jb->red.at0      = now_us;
		jb->red.anchored = true;
		jb->red.front_k  = 0;
		jb->red.play_k   = 0;
	}

	/* Redundancy never extends the highest primary packet frontier */
	if (main_k > jb->red.front_k)
		jb->red.front_k = main_k;

	for (j = 0; j < npend; ++j)
		red_commit_one(jb, &pendv[j]);

	return 0;

 inval:
	err = EBADMSG;

 fail:
	for (j = 0; j < npend; ++j)
		pendv[j].mem = mem_deref(pendv[j].mem);

	return err;
}


/**
 * Get one audio block from a RED jitter buffer
 *
 * Only the current slot is considered.  On success the returned mbuf is
 * independent of the input buffers.
 *
 * @param jb        Jitter buffer
 * @param now_us    Monotonic time in microseconds
 * @param hdr       Returned RTP header
 * @param mbp       Returned audio buffer
 * @param redundant Returned true if the audio was recovered from RED
 *
 * @return 0 if an audio block was returned, EAGAIN if not due yet,
 *         ENOENT if the slot is beyond the primary frontier, ENODATA if
 *         the slot is due but empty (the slot is then advanced).
 */
int jbuf_get_red(struct jbuf *jb, uint64_t now_us, struct rtp_header *hdr,
		 struct mbuf **mbp, bool *redundant)
{
	int64_t k;
	uint64_t off, due;
	struct red_slot *slot;

	if (!jb || !hdr || !mbp || !redundant || JBUF_MODE_RED != jb->mode)
		return EINVAL;

	if (!jb->red.anchored)
		return ENOENT;

	k = jb->red.play_k;
	if (!red_k_ok(jb, k))
		return ENOENT;

	/* Slots beyond the highest primary packet are not known yet */
	if (k > jb->red.front_k)
		return ENOENT;

	off = (uint64_t)k * jb->red.cfg.ptime_ticks * 1000000ULL /
		jb->red.cfg.rate;
	due = jb->red.at0 + (uint64_t)jb->red.cfg.delay_us + off;

	if (now_us < due)
		return EAGAIN;

	slot = &jb->red.slotv[k];

	if (!slot->used) {
		jb->red.play_k = k + 1;
		return ENODATA;
	}

	*hdr       = slot->hdr;
	*mbp       = slot->mem;
	*redundant = slot->from_red;

	slot->mem      = NULL;
	slot->used     = false;
	slot->from_red = false;

	jb->red.play_k = k + 1;

	return 0;
}


/**
 * Get jitter buffer statistics
 *
 * @param jb    Jitter buffer
 * @param jstat Pointer to statistics storage
 *
 * @return 0 if success, otherwise errorcode
 */
int jbuf_stats(const struct jbuf *jb, struct jbuf_stat *jstat)
{
	if (!jb || !jstat)
		return EINVAL;

#if JBUF_STAT
	*jstat = jb->stat;

	return 0;
#else
	return ENOSYS;
#endif
}


/**
 * Debug the jitter buffer
 *
 * @param pf Print handler
 * @param jb Jitter buffer
 *
 * @return 0 if success, otherwise errorcode
 */
int jbuf_debug(struct re_printf *pf, const struct jbuf *jb)
{
	int err = 0;

	if (!jb)
		return 0;

	err |= re_hprintf(pf, "--- jitter buffer debug---\n");

	err |= re_hprintf(pf, " running=%d", jb->running);
	err |= re_hprintf(pf, " min=%u cur=%u max=%u [frames]\n",
			  jb->min, jb->n, jb->max);
	err |= re_hprintf(pf, " seq_put=%u\n", jb->seq_put);

#if JBUF_STAT
	err |= re_hprintf(pf, " Stat: put=%u", jb->stat.n_put);
	err |= re_hprintf(pf, " get=%u", jb->stat.n_get);
	err |= re_hprintf(pf, " oos=%u", jb->stat.n_oos);
	err |= re_hprintf(pf, " dup=%u", jb->stat.n_dups);
	err |= re_hprintf(pf, " late=%u", jb->stat.n_late);
	err |= re_hprintf(pf, " or=%u", jb->stat.n_overflow);
	err |= re_hprintf(pf, " ur=%u", jb->stat.n_underflow);
	err |= re_hprintf(pf, " flush=%u", jb->stat.n_flush);
	err |= re_hprintf(pf, "       put/get_ratio=%u%%", jb->stat.n_get ?
			  100*jb->stat.n_put/jb->stat.n_get : 0);
	err |= re_hprintf(pf, " lost=%u (%u.%02u%%)\n",
			  jb->stat.n_lost,
			  jb->stat.n_put ?
			  100*jb->stat.n_lost/jb->stat.n_put : 0,
			  jb->stat.n_put ?
			  10000*jb->stat.n_lost/jb->stat.n_put%100 : 0);
#endif

	return err;
}
