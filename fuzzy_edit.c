/*
 * fuzzy_edit.c - editable buffer with an incrementally maintained ssdeep.
 *
 * Design
 * ------
 * The bytes live in an AVL tree of bounded leaf blocks (interior leaves
 * hold between FE_LEAF_MIN and FE_LEAF_MAX bytes; the two end leaves may
 * be smaller).  Every tree node carries a composable summary of its byte
 * range; leaves additionally own the raw bytes.  Space is linear in the
 * current byte count: leaf payloads are bounded, no edit history and no
 * per-edit-position tables are retained.
 *
 * Rolling hash (seven byte window; see roll_hash() in fuzzy.c).
 *   Its state depends only on the last ROLLING_WINDOW bytes.  Each node
 *   caches up to ROLLING_WINDOW-1 prefix and suffix bytes.  The ending
 *   state of a concatenation A||B is B's cached state when B spans a full
 *   window, otherwise the last window bytes of A||B are replayed from a
 *   zeroed rolling state.
 *
 * FNV style block hash.
 *   Each byte maps the six bit hash through a permutation of the 64
 *   possible values (sum_table in fuzzy.c is that permutation for one
 *   byte).  A run of bytes therefore acts as a permutation of the 64
 *   values and permutations compose by table lookup in O(64).  A rolling
 *   hash reset point ("trigger") delimits a piece.
 *
 *   The streaming engine resets the main hash at the first 63 triggers
 *   and the "half" hash at the first 32; afterwards each thread keeps
 *   accumulating.  Per blocksize level every node stores:
 *     count - exact trigger count,
 *     head  - permutation from the node start to its first trigger,
 *     tail  - permutation from its last trigger to the node end,
 *     chars - b64 indices emitted at the first (up to 64) triggers.
 *   One node-wide "full" permutation covers all bytes: the FNV
 *   permutation of a byte range does not depend on the blocksize level.
 *
 *   Concatenation A||B is a closed, allocation free operation on these
 *   fields (fe_level_concat): counts add; head/tail extend through the
 *   full/tail permutations; emitted chars concatenate and only B's first
 *   char is recomputed when A has no trigger of its own.
 *
 *   Leaves additionally store endval[j-1]: the running value at the leaf
 *   end of a thread reset to HASH_INIT at the leaf's j-th trigger, for
 *   j <= 63.  The single scanning pass keeps the first 64 piece
 *   permutations plus "post", the permutation from the 64th trigger to
 *   the leaf end, so every endval entry is a chain of stored
 *   permutations and no second byte scan is needed.
 *
 *   Saturated thread end values for the whole buffer (main thread after
 *   its 63rd trigger, half thread after its 32nd) are fixed once per
 *   splice in fe_finalize_root(): per level, an O(log n) count walk
 *   locates the leaf holding the threshold trigger, its endval gives the
 *   thread value at that leaf end, and the full permutations of the
 *   O(log n) right-hand sibling subtrees carry that single value to the
 *   buffer end.  Digest calls only read these cached root values, so
 *   repeated digests neither read raw bytes nor walk leaves.
 *
 *   The value at the last trigger (needed for the all-zero rolling-hash
 *   output case and for the truncated second component's tail) is
 *   tail^{-1} applied to the thread end value once the thread has stopped
 *   resetting; tail is a bijection.  Before that point the streaming
 *   engine clears the half tail marker, and the emitted marker is the
 *   stored char, so fe_fold_level() picks the right source explicitly.
 *
 * Splice.
 *   Two AVL splits cut at offset and offset+removed (only the intersected
 *   leaf is rescanned per cut), the removed subtree is reclaimed without
 *   a byte scan, bounded leaves are built for the inserted bytes, and the
 *   pieces join back.  Each join normalises its seam: the two boundary
 *   leaves are popped structurally (no rescans), merged when undersize
 *   and rebalanced.  Untouched leaves keep both bytes and summaries.
 *
 * Work counters.
 *   raw_bytes increments once per data byte loaded for summary work
 *   (leaf (re)builds, inserted bytes, cached prefix/suffix replays); a
 *   byte loaded more than once is counted each time and one load that
 *   updates all 31 level states counts once.  node_visits increments in
 *   fe_touch() for every node accessed for location, summary
 *   recomputation, splitting, joining, balancing or reclamation;
 *   repeated touches count repeatedly.  Digest queries add nothing.
 */

#include "fuzzy_edit.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Same precomputed FNV permutation table fuzzy.c uses. */
#include "sum_table.h"

#define FE_ROLL_WINDOW 7
#define FE_SEAM (FE_ROLL_WINDOW - 1)
#define FE_MIN_BLOCKSIZE 3
#define FE_HASH_INIT 0x27
#define FE_NUM_LEVELS 31
#define FE_SPAMSUM 64
#define FE_HALF (FE_SPAMSUM / 2)

/* Interior leaves stay within [FE_LEAF_MIN, FE_LEAF_MAX]; only the two
 * end leaves may be smaller.  A splice rescans a constant number of
 * leaves per cut/merge, each at most FE_LEAF_MAX bytes. */
#define FE_LEAF_MAX 4096u
#define FE_LEAF_MIN 1024u

static const char fe_b64[] =
  "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* ---------------- rolling hash (mirror of fuzzy.c) ------------------ */

struct fe_roll
{
  unsigned char window[FE_ROLL_WINDOW];
  uint32_t h1, h2, h3, n;
};

struct fuzzy_edit;
static void fe_bytes(struct fuzzy_edit *ed, size_t n);
static struct fe_node *fe_build_tree(struct fuzzy_edit *ed,
				     struct fe_node **leaves, size_t n);

static void fe_roll_init(struct fe_roll *r)
{
  memset(r, 0, sizeof(*r));
}

static void fe_roll_hash(struct fe_roll *r, unsigned char c)
{
  r->h2 -= r->h1;
  r->h2 += FE_ROLL_WINDOW * (uint32_t)c;
  r->h1 += (uint32_t)c;
  r->h1 -= (uint32_t)r->window[r->n];
  r->window[r->n] = c;
  if (++r->n == FE_ROLL_WINDOW)
    r->n = 0;
  r->h3 <<= 5;
  r->h3 ^= c;
}

static uint32_t fe_roll_sum(const struct fe_roll *r)
{
  return r->h1 + r->h2 + r->h3;
}

/*
 * Rolling state after processing data[0..k) starting from state prime.
 * Once seven bytes have been seen the state is fully determined by the
 * last seven input bytes, so a long prefix costs only seven byte reads;
 * a short prefix replays from the carried predecessor state.  The bytes
 * read here are charged to raw_bytes.
 */
static struct fe_roll fe_roll_after(struct fuzzy_edit *ed,
				    const struct fe_roll *prime,
				    const unsigned char *data, size_t k)
{
  struct fe_roll roll;
  size_t i, start = 0;
  if (k >= FE_ROLL_WINDOW)
  {
    fe_roll_init(&roll);
    start = k - FE_ROLL_WINDOW;
  }
  else
  {
    roll = *prime;
  }
  for (i = start; i < k; ++i)
  {
    fe_bytes(ed, 1);
    fe_roll_hash(&roll, data[i]);
  }
  return roll;
}

static unsigned char fe_sum_hash(unsigned char c, unsigned char h)
{
  return sum_table[h][c & 0x3f];
}

/* ---------------- permutations of the 64 hash values ---------------- */

static void fe_perm_identity(unsigned char p[64])
{
  unsigned i;
  for (i = 0; i < 64; ++i)
    p[i] = (unsigned char)i;
}

/* dst = outer o inner. */
static void fe_perm_compose(unsigned char dst[64],
			    const unsigned char outer[64],
			    const unsigned char inner[64])
{
  unsigned i;
  for (i = 0; i < 64; ++i)
    dst[i] = outer[inner[i]];
}

static void fe_perm_extend(unsigned char p[64], unsigned char c)
{
  unsigned i;
  for (i = 0; i < 64; ++i)
    p[i] = fe_sum_hash(c, p[i]);
}

static void fe_perm_invert(unsigned char inv[64],
			   const unsigned char p[64])
{
  unsigned i;
  for (i = 0; i < 64; ++i)
    inv[p[i]] = (unsigned char)i;
}

/* ---------------- per-level trigger summary ------------------------- */

struct fe_level
{
  uint32_t count;             /* exact trigger count at this level */
  unsigned char head[64];     /* start -> first trigger */
  unsigned char tail[64];     /* last trigger -> end */
  unsigned char chars[FE_SPAMSUM]; /* emitted b64 indices, first 64 */
  /* Root-only cached end values of the two reset threads. */
  unsigned char main_end;
  unsigned char half_end;
  /* Leaf-only: endval[j-1] = value at leaf end when a thread is reset to
   * HASH_INIT at the leaf's j-th trigger (1 <= j <= 63). */
  unsigned char endval[FE_SPAMSUM - 1];
};

struct fe_node
{
  int is_leaf;
  int height;
  size_t len;
  struct fe_node *left, *right;

  /* FNV permutation of the whole node byte range. */
  unsigned char full[64];

  /* Rolling summary. */
  struct fe_roll roll;
  size_t pre_len, suf_len;
  unsigned char prefix[FE_SEAM];
  unsigned char suffix[FE_SEAM];

  struct fe_level level[FE_NUM_LEVELS];

  /* Leaf only: rolling state immediately before the leaf's first byte,
   * i.e. the state after every predecessor byte.  Triggers inside the
   * first ROLLING_WINDOW-1 leaf bytes only become correct once scanning
   * starts from this primed state. */
  struct fe_roll prime;

  /* Leaf payload (NULL on internal nodes). */
  unsigned char *data;
};

struct fuzzy_edit
{
  struct fe_node *root;
  size_t total;
  uint64_t raw_bytes;
  uint64_t node_visits;
};

static const struct fe_node *fe_touch(struct fuzzy_edit *ed,
				      const struct fe_node *node)
{
  ed->node_visits++;
  return node;
}

static void fe_bytes(struct fuzzy_edit *ed, size_t n)
{
  ed->raw_bytes += (uint64_t)n;
}

/* ---------------- node lifecycle ------------------------------------ */

static void fe_node_free(struct fe_node *node)
{
  if (node == NULL)
    return;
  if (!node->is_leaf)
  {
    fe_node_free(node->left);
    fe_node_free(node->right);
  }
  free(node->data);
  free(node);
}

/* Reclaim a subtree that an edit removed, charging one node visit per
 * reclaimed node (reclamation is one of the visit categories the stats
 * expose).  No raw bytes are read. */
static void fe_reclaim(struct fuzzy_edit *ed, struct fe_node *node)
{
  if (node == NULL)
    return;
  fe_touch(ed, node);
  if (!node->is_leaf)
  {
    fe_reclaim(ed, node->left);
    fe_reclaim(ed, node->right);
  }
  free(node->data);
  free(node);
}

static int fe_height(const struct fe_node *n)
{
  return n == NULL ? 0 : n->height;
}

/* ---------------- leaf scanning ------------------------------------- */

/*
 * Scan data[0..len) once.  For every blocksize level maintain the rolling
 * trigger logic of fuzzy_engine_step() with an always-zero roll mask (so
 * every level's full trigger set is captured; blocksize reductions only
 * filter lower bits and never change a surviving level's triggers) and
 * the head/tail/chars/endval fields that make summaries composable.
 * Every scanned byte is charged to raw_bytes.
 */
static int fe_leaf_scan(struct fuzzy_edit *ed, struct fe_node *node,
			const unsigned char *data, size_t len,
			const struct fe_roll *prime)
{
  /* run:    piece permutation since the last trigger (resets every time).
   * post:   permutation from the 64th trigger to the current position.
   * pieces: first 64 piece permutations, used once to derive endval[]. */
  unsigned char run[FE_NUM_LEVELS][64];
  unsigned char post[FE_NUM_LEVELS][64];
  unsigned char (*pieces[FE_NUM_LEVELS])[64];
  struct fe_roll roll;
  size_t pos;
  unsigned i;

  memset(pieces, 0, sizeof(pieces));
  if (prime != NULL)
    roll = *prime;
  else
    fe_roll_init(&roll);
  node->prime = roll;
  fe_perm_identity(node->full);
  for (i = 0; i < FE_NUM_LEVELS; ++i)
  {
    node->level[i].count = 0;
    node->level[i].main_end = node->level[i].half_end = FE_HASH_INIT;
    fe_perm_identity(run[i]);
    fe_perm_identity(post[i]);
  }

  for (pos = 0; pos < len; ++pos)
  {
    unsigned char c = data[pos];
    uint32_t horg, h;
    fe_bytes(ed, 1);
    fe_roll_hash(&roll, c);
    fe_perm_extend(node->full, c);
    for (i = 0; i < FE_NUM_LEVELS; ++i)
    {
      fe_perm_extend(run[i], c);
      if (node->level[i].count >= FE_SPAMSUM)
        fe_perm_extend(post[i], c);
    }

    horg = fe_roll_sum(&roll) + 1;
    if (horg == 0 || horg % (uint32_t)FE_MIN_BLOCKSIZE != 0)
      continue;
    /* Cascade identical to fuzzy_engine_step with bhstart = 0. */
    h = horg / (uint32_t)FE_MIN_BLOCKSIZE;
    i = 0;
    do
    {
      struct fe_level *lv = &node->level[i];
      uint32_t idx = lv->count;
      if (idx == 0)
      {
        memcpy(lv->head, run[i], 64);
        pieces[i] = malloc(FE_SPAMSUM * 64);
        if (pieces[i] == NULL)
          goto oom;
      }
      if (idx < FE_SPAMSUM)
      {
        lv->chars[idx] = run[i][FE_HASH_INIT];
        memcpy(pieces[i][idx], run[i], 64);
      }
      lv->count++;
      memcpy(lv->tail, run[i], 64); /* overwritten until the final pass */
      fe_perm_identity(run[i]);
      if (idx + 1 == FE_SPAMSUM)
        fe_perm_identity(post[i]);  /* begin post at the 64th trigger */
      if (h & 1u)
        break;
      h >>= 1;
      ++i;
    } while (i < FE_NUM_LEVELS);
  }

  for (i = 0; i < FE_NUM_LEVELS; ++i)
  {
    struct fe_level *lv = &node->level[i];
    uint32_t c = lv->count;
    unsigned char p[64];
    uint32_t j, top;
    if (c == 0)
      continue;
    /* tail = permutation from the last trigger to the node end. */
    memcpy(lv->tail, run[i], 64);
    /* Derive endval backward.  P chains the pieces after trigger j; when
     * the leaf has 64+ triggers the missing later pieces are exactly the
     * stored post permutation. */
    if (c >= FE_SPAMSUM)
      memcpy(p, post[i], 64);
    else
      memcpy(p, lv->tail, 64);
    top = c < FE_SPAMSUM ? c : FE_SPAMSUM;
    for (j = top; j > 0; --j)
    {
      unsigned char q[64];
      if (j < FE_SPAMSUM)
        lv->endval[j - 1] = p[FE_HASH_INIT];
      /* Extend the chain one trigger backward:
       * p_new(x) = p_old(piece_j(x)).  Use a temp so the lookup table is
       * not clobbered while the composition is being written. */
      fe_perm_compose(q, p, pieces[i][j - 1]);
      memcpy(p, q, 64);
    }
    free(pieces[i]);
    pieces[i] = NULL;
  }

  node->roll = roll;
  node->len = len;
  node->pre_len = len < FE_SEAM ? len : FE_SEAM;
  memcpy(node->prefix, data, node->pre_len);
  node->suf_len = node->pre_len;
  if (len >= FE_SEAM)
    memcpy(node->suffix, data + len - FE_SEAM, FE_SEAM);
  else
    memcpy(node->suffix, data, len);
  return 0;

oom:
  for (i = 0; i < FE_NUM_LEVELS; ++i)
    free(pieces[i]);
  return -1;
}

static struct fe_node *fe_leaf_newp(struct fuzzy_edit *ed,
				    const unsigned char *data, size_t len,
				    const struct fe_roll *prime)
{
  struct fe_node *node = calloc(1, sizeof(*node));
  if (node == NULL)
    return NULL;
  fe_touch(ed, node);
  node->is_leaf = 1;
  node->height = 1;
  node->len = len;
  if (len > 0)
  {
    node->data = malloc(len);
    if (node->data == NULL)
    {
      free(node);
      return NULL;
    }
    memcpy(node->data, data, len);
    if (fe_leaf_scan(ed, node, node->data, len, prime) < 0)
    {
      fe_node_free(node);
      return NULL;
    }
  }
  else
  {
    if (prime != NULL)
    {
      node->roll = *prime;
      node->prime = *prime;
    }
    else
      fe_roll_init(&node->roll);
    fe_perm_identity(node->full);
  }
  return node;
}

/* ---------------- summary composition ------------------------------- */

/*
 * Compose the rolling summary of adjacent nodes l||r.  When r spans a
 * full rolling window its cached ending state is the answer; otherwise
 * the last window bytes of the concatenation are replayed from a zeroed
 * state.  The cached seam bytes read here are charged to raw_bytes.
 */
static void fe_roll_concat(struct fe_node *dst, const struct fe_node *l,
			   const struct fe_node *r,
			   struct fuzzy_edit *ed)
{
  if (r->len >= FE_ROLL_WINDOW)
  {
    dst->roll = r->roll;
  }
  else
  {
    unsigned char buf[2 * FE_SEAM];
    size_t n = 0, i, take;
    struct fe_roll roll;
    take = FE_ROLL_WINDOW - r->len;
    if (take > l->suf_len)
      take = l->suf_len;
    memcpy(buf, l->suffix + (l->suf_len - take), take);
    n = take;
    memcpy(buf + n, r->prefix, r->pre_len);
    n += r->pre_len;
    fe_roll_init(&roll);
    for (i = 0; i < n; ++i)
    {
      fe_bytes(ed, 1);
      fe_roll_hash(&roll, buf[i]);
    }
    dst->roll = roll;
  }

  dst->pre_len = l->pre_len;
  memcpy(dst->prefix, l->prefix, dst->pre_len);
  if (l->len < FE_SEAM && r->pre_len > 0)
  {
    size_t add = FE_SEAM - dst->pre_len;
    if (add > r->pre_len)
      add = r->pre_len;
    memcpy(dst->prefix + dst->pre_len, r->prefix, add);
    dst->pre_len += add;
  }

  if (r->len < FE_SEAM && l->suf_len > 0)
  {
    size_t keep = FE_SEAM - r->suf_len;
    if (keep > l->suf_len)
      keep = l->suf_len;
    memmove(dst->suffix, l->suffix + (l->suf_len - keep), keep);
    memcpy(dst->suffix + keep, r->suffix, r->suf_len);
    dst->suf_len = keep + r->suf_len;
  }
  else
  {
    dst->suf_len = r->suf_len;
    memcpy(dst->suffix, r->suffix, r->suf_len);
  }
}

/*
 * Monoidal concatenation of two per-level summaries.  Allocation free:
 * every field is fixed size.  See the file header for the invariant.
 */
static void fe_level_concat(struct fe_level *dst,
			    const struct fe_level *a,
			    const struct fe_level *b,
			    const unsigned char full_a[64],
			    const unsigned char full_b[64])
{
  uint32_t total, keep, i;
  struct fe_level out;
  memset(&out, 0, sizeof(out));
  total = a->count + b->count;
  out.count = total;
  out.main_end = out.half_end = FE_HASH_INIT;

  if (total == 0)
  {
    *dst = out;
    return;
  }

  if (a->count == 0)
  {
    /* First trigger sits in b: the head piece extends through all of a. */
    fe_perm_compose(out.head, b->head, full_a);
    memcpy(out.tail, b->tail, 64);
    keep = b->count < FE_SPAMSUM ? b->count : FE_SPAMSUM;
    for (i = 0; i < keep; ++i)
      out.chars[i] = b->chars[i];
    /* B's first emit sees full_a(HASH_INIT), not HASH_INIT. */
    out.chars[0] = b->head[full_a[FE_HASH_INIT]];
    *dst = out;
    return;
  }

  if (b->count == 0)
  {
    memcpy(out.head, a->head, 64);
    fe_perm_compose(out.tail, full_b, a->tail);
    keep = a->count < FE_SPAMSUM ? a->count : FE_SPAMSUM;
    memcpy(out.chars, a->chars, keep);
    *dst = out;
    return;
  }

  /* Both sides trigger. */
  memcpy(out.head, a->head, 64);
  memcpy(out.tail, b->tail, 64);
  keep = total < FE_SPAMSUM ? total : FE_SPAMSUM;
  for (i = 0; i < keep; ++i)
  {
    if (i < a->count)
      out.chars[i] = a->chars[i];
    else if (i == a->count)
    {
      /* Bridge piece: a's open tail followed by b's head piece. */
      unsigned char bridge[64];
      fe_perm_compose(bridge, b->head, a->tail);
      out.chars[i] = bridge[FE_HASH_INIT];
    }
    else
      out.chars[i] = b->chars[i - a->count];
  }
  *dst = out;
}

/* Recompute every summary field of an internal node from its children.
 * A recompute is one visit of the node itself. */
static void fe_recompute(struct fuzzy_edit *ed, struct fe_node *node)
{
  unsigned i;
  fe_touch(ed, node);
  node->len = node->left->len + node->right->len;
  node->height = 1 + (fe_height(node->left) > fe_height(node->right)
		      ? fe_height(node->left) : fe_height(node->right));
  fe_perm_compose(node->full, node->right->full, node->left->full);
  fe_roll_concat(node, node->left, node->right, ed);
  for (i = 0; i < FE_NUM_LEVELS; ++i)
    fe_level_concat(&node->level[i], &node->left->level[i],
		    &node->right->level[i],
		    node->left->full, node->right->full);
}

static struct fe_node *fe_internal_new(struct fuzzy_edit *ed,
				       struct fe_node *left,
				       struct fe_node *right)
{
  struct fe_node *node = calloc(1, sizeof(*node));
  if (node == NULL)
    return NULL;
  node->is_leaf = 0;
  node->left = left;
  node->right = right;
  fe_recompute(ed, node);
  return node;
}

/* ---------------- AVL rotations and join ---------------------------- */

static struct fe_node *fe_rotate_right(struct fuzzy_edit *ed,
				       struct fe_node *y)
{
  struct fe_node *x = y->left;
  struct fe_node *t2 = x->right;
  x->right = y;
  y->left = t2;
  fe_recompute(ed, y);
  fe_recompute(ed, x);
  return x;
}

static struct fe_node *fe_rotate_left(struct fuzzy_edit *ed,
				      struct fe_node *x)
{
  struct fe_node *y = x->right;
  struct fe_node *t2 = y->left;
  y->left = x;
  x->right = t2;
  fe_recompute(ed, x);
  fe_recompute(ed, y);
  return y;
}

static struct fe_node *fe_balance(struct fuzzy_edit *ed,
				  struct fe_node *node)
{
  int bf = fe_height(node->left) - fe_height(node->right);
  if (bf > 1)
  {
    if (fe_height(node->left->left) < fe_height(node->left->right))
      node->left = fe_rotate_left(ed, node->left);
    return fe_rotate_right(ed, node);
  }
  if (bf < -1)
  {
    if (fe_height(node->right->right) < fe_height(node->right->left))
      node->right = fe_rotate_right(ed, node->right);
    return fe_rotate_left(ed, node);
  }
  fe_recompute(ed, node);
  return node;
}

/* Join two trees where every byte of l precedes every byte of r. */
static struct fe_node *fe_join(struct fuzzy_edit *ed,
			       struct fe_node *l, struct fe_node *r)
{
  struct fe_node *node;
  if (l == NULL)
    return r;
  if (r == NULL)
    return l;
  if (fe_height(l) > fe_height(r) + 1)
  {
    fe_touch(ed, l);
    l->right = fe_join(ed, l->right, r);
    return fe_balance(ed, l);
  }
  if (fe_height(r) > fe_height(l) + 1)
  {
    fe_touch(ed, r);
    r->left = fe_join(ed, l, r->left);
    return fe_balance(ed, r);
  }
  node = fe_internal_new(ed, l, r);
  return node;
}

/* Structural extreme-leaf pops: no byte rescans, only path recomputes. */
static struct fe_node *fe_pop_right(struct fuzzy_edit *ed,
				    struct fe_node *node,
				    struct fe_node **leaf_out)
{
  struct fe_node *rest;
  fe_touch(ed, node);
  if (node->is_leaf)
  {
    *leaf_out = node;
    return NULL;
  }
  if (node->right->is_leaf)
  {
    *leaf_out = node->right;
    rest = node->left;
    node->left = node->right = NULL;
    fe_node_free(node);
    return rest;
  }
  node->right = fe_pop_right(ed, node->right, leaf_out);
  return fe_balance(ed, node);
}

static struct fe_node *fe_pop_left(struct fuzzy_edit *ed,
				   struct fe_node *node,
				   struct fe_node **leaf_out)
{
  struct fe_node *rest;
  fe_touch(ed, node);
  if (node->is_leaf)
  {
    *leaf_out = node;
    return NULL;
  }
  if (node->left->is_leaf)
  {
    *leaf_out = node->left;
    rest = node->right;
    node->left = node->right = NULL;
    fe_node_free(node);
    return rest;
  }
  node->left = fe_pop_left(ed, node->left, leaf_out);
  return fe_balance(ed, node);
}

/*
 * The splice edit zone.  All bytes whose rolling-hash context can change
 * in an edit are gathered into one contiguous buffer and rechunked into
 * bounded leaves: the last leaf immediately left of the edit, every
 * inserted byte, and the first leaf immediately right of the edit.  The
 * leaves are rebuilt in one forward pass that chains the rolling state,
 * so triggers in the first window bytes of every leaf see the correct
 * predecessor bytes.  The outer prefix and suffix subtrees are untouched
 * and their summaries stay valid: the zone starts with the prefix's end
 * state, and a right boundary leaf of at least seven bytes ends on the
 * same content-determined state it had before.
 */
struct fe_zone
{
  unsigned char *bytes;
  size_t len;
};

/* Append a leaf's raw bytes to the zone buffer. */
static int fe_zone_append(struct fe_zone *z, const unsigned char *data,
			  size_t len)
{
  unsigned char *grow = realloc(z->bytes, z->len + len + 1);
  if (grow == NULL)
    return -1;
  z->bytes = grow;
  memcpy(z->bytes + z->len, data, len);
  z->len += len;
  return 0;
}

/*
 * Chunk the zone bytes into bounded leaves, chaining the rolling state
 * from prime.  When the final leaf is undersize another leaf precedes it,
 * rebalance the last pair so interior leaves stay at or above
 * FE_LEAF_MIN.  Returns a balanced subtree (or NULL for an empty zone).
 */
static struct fe_node *fe_zone_build(struct fuzzy_edit *ed,
				     const struct fe_zone *z,
				     const struct fe_roll *prime)
{
  struct fe_node **leaves;
  size_t cap = z->len / FE_LEAF_MAX + 2, n = 0, done = 0, i;
  struct fe_node *root;
  struct fe_roll carry = *prime;
  if (z->len == 0)
    return NULL;
  leaves = malloc(cap * sizeof(*leaves));
  if (leaves == NULL)
    return NULL;
  while (done < z->len)
  {
    size_t step = z->len - done;
    struct fe_node *leaf;
    if (step > FE_LEAF_MAX)
      step = FE_LEAF_MAX;
    leaf = fe_leaf_newp(ed, z->bytes + done, step, &carry);
    if (leaf == NULL)
      goto fail;
    carry = leaf->roll;
    leaves[n++] = leaf;
    done += step;
  }
  /* Rebalance an undersize final leaf against its predecessor. */
  if (n >= 2 && leaves[n - 1]->len < FE_LEAF_MIN)
  {
    struct fe_node *a = leaves[n - 2], *b = leaves[n - 1];
    size_t total = a->len + b->len, half = total / 2;
    struct fe_node *na, *nb;
    struct fe_roll ap = a->prime;
    na = fe_leaf_newp(ed, z->bytes + done - total, half, &ap);
    if (na == NULL)
      goto fail;
    {
      struct fe_roll bp = fe_roll_after(ed, &ap,
					z->bytes + done - total, half);
      nb = fe_leaf_newp(ed, z->bytes + done - total + half,
			total - half, &bp);
    }
    if (nb == NULL)
    {
      fe_node_free(na);
      goto fail;
    }
    fe_node_free(a);
    fe_node_free(b);
    leaves[n - 2] = na;
    leaves[n - 1] = nb;
  }
  root = fe_build_tree(ed, leaves, n);
  free(leaves);
  return root;
fail:
  for (i = 0; i < n; ++i)
    fe_node_free(leaves[i]);
  free(leaves);
  return NULL;
}

/*
 * Split tree root into *l_out (first k bytes) and *r_out (the rest).
 * Cuts at leaf boundaries are purely structural; a cut inside a leaf
 * rebuilds that one leaf in two pieces.
 */
static int fe_split(struct fuzzy_edit *ed, struct fe_node *root, size_t k,
		    struct fe_node **l_out, struct fe_node **r_out)
{
  if (root == NULL)
  {
    *l_out = *r_out = NULL;
    return 0;
  }
  fe_touch(ed, root);
  if (root->is_leaf)
  {
    struct fe_node *l, *r;
    if (k == 0)
    {
      *l_out = NULL;
      *r_out = root;
      return 0;
    }
    if (k == root->len)
    {
      *l_out = root;
      *r_out = NULL;
      return 0;
    }
    l = fe_leaf_newp(ed, root->data, k, &root->prime);
    {
      struct fe_roll rprime =
	fe_roll_after(ed, &root->prime, root->data, k);
      r = fe_leaf_newp(ed, root->data + k, root->len - k, &rprime);
    }
    if (l == NULL || r == NULL)
    {
      fe_node_free(l);
      fe_node_free(r);
      return -1;
    }
    fe_node_free(root);
    *l_out = l;
    *r_out = r;
    return 0;
  }
  if (k <= root->left->len)
  {
    struct fe_node *ll, *lr, *right;
    if (fe_split(ed, root->left, k, &ll, &lr) < 0)
      return -1;
    right = root->right;
    root->left = root->right = NULL;
    fe_node_free(root);
    *l_out = ll;
    *r_out = fe_join(ed, lr, right);
    return 0;
  }
  {
    struct fe_node *rl, *rr, *left;
    if (fe_split(ed, root->right, k - root->left->len, &rl, &rr) < 0)
      return -1;
    left = root->left;
    root->left = root->right = NULL;
    fe_node_free(root);
    *r_out = rr;
    *l_out = fe_join(ed, left, rl);
    return 0;
  }
}

/* Build a balanced tree out of ordered, non-overlapping leaves. */
static struct fe_node *fe_build_tree(struct fuzzy_edit *ed,
				     struct fe_node **leaves, size_t n)
{
  while (n > 1)
  {
    size_t out = 0, i;
    for (i = 0; i + 1 < n; i += 2)
    {
      struct fe_node *m = fe_internal_new(ed, leaves[i], leaves[i + 1]);
      if (m == NULL)
      {
        size_t j;
        for (j = out; j < n; ++j)
          fe_node_free(leaves[j]);
        return NULL;
      }
      leaves[out++] = m;
    }
    if (i < n)
      leaves[out++] = leaves[i];
    n = out;
  }
  return n == 0 ? NULL : leaves[0];
}

/* ---------------- root finalization --------------------------------- */

/*
 * Carry the value of a thread reset to HASH_INIT at global trigger t
 * (1-based) through to the buffer end.  A count walk locates the leaf
 * holding the trigger; the leaf's endval gives the value at its end and
 * right-hand siblings crossed on the way contribute their whole-node
 * permutation.  No raw bytes are read; only nodes on one root-to-leaf
 * path plus its right siblings are visited.
 */
static unsigned char fe_suffix_thread(struct fuzzy_edit *ed,
				      const struct fe_node *root,
				      unsigned level, uint32_t t)
{
  const struct fe_node *node = root;
  const struct fe_node *stack[64];
  int sp = 0;
  uint32_t before = 0;
  unsigned char value;
  for (;;)
  {
    fe_touch(ed, node);
    if (node->is_leaf)
      break;
    if (t <= before + node->left->level[level].count)
    {
      stack[sp++] = node->right;
      node = node->left;
    }
    else
    {
      before += node->left->level[level].count;
      node = node->right;
    }
  }
  value = node->level[level].endval[t - before - 1];
  while (sp > 0)
  {
    const struct fe_node *sib = stack[--sp];
    fe_touch(ed, sib);
    value = sib->full[value];
  }
  return value;
}

/* Fill the root-only main_end/half_end cached values for every level. */
static void fe_finalize_root(struct fuzzy_edit *ed,
			     struct fe_node *root)
{
  unsigned i;
  if (root == NULL)
    return;
  fe_touch(ed, root);
  for (i = 0; i < FE_NUM_LEVELS; ++i)
  {
    struct fe_level *lv = &root->level[i];
    uint32_t c = lv->count;
    if (c == 0)
    {
      lv->main_end = root->full[FE_HASH_INIT];
      lv->half_end = root->full[FE_HASH_INIT];
      continue;
    }
    /* Main thread last reset: after trigger min(c, 63). */
    lv->main_end = fe_suffix_thread(ed, root, i,
				    c < FE_SPAMSUM - 1 ? c
						       : FE_SPAMSUM - 1);
    /* Half thread: after trigger t the reset test dindex < 32 runs with
     * dindex == t, so the last half reset follows trigger 31.  From
     * trigger 32 on it accumulates. */
    lv->half_end = fe_suffix_thread(ed, root, i,
				    c < FE_HALF - 1 ? c : FE_HALF - 1);
  }
}

/* ---------------- splice -------------------------------------------- */

static int fe_check_splice(size_t cur, size_t offset, size_t removed,
			   const unsigned char *insert, size_t inserted,
			   size_t *new_len)
{
  if (offset > cur || removed > cur - offset)
    return -1;
  if (inserted > 0 && insert == NULL)
    return -1;
  if (inserted > FUZZY_EDIT_MAX_LENGTH - (cur - removed))
    return -1;
  *new_len = cur - removed + inserted;
  if (*new_len > FUZZY_EDIT_MAX_LENGTH)
    return -1;
  return 0;
}

int fuzzy_edit_splice(struct fuzzy_edit *self, size_t offset,
		      size_t removed, const unsigned char *insert,
		      size_t inserted)
{
  struct fe_node *left = NULL, *removed_tree = NULL, *right = NULL;
  struct fe_node *la = NULL, *lb = NULL, *prefix = NULL, *suffix = NULL;
  struct fe_node *zone_tree = NULL, *joined;
  struct fe_zone zone;
  struct fe_roll prime;
  size_t new_len;
  if (self == NULL)
    return -1;
  if (fe_check_splice(self->total, offset, removed, insert, inserted,
		      &new_len) < 0)
    return -1;

  /* Cut the buffer into left | removed_tree | right.  Cuts at leaf
   * boundaries are purely structural; a cut inside a leaf rebuilds only
   * that one bounded leaf.  removed_tree is retained until success so a
   * later allocation failure can restore the exact original tree. */
  if (fe_split(self, self->root, offset, &left, &removed_tree) < 0)
    return -1;
  self->root = NULL;
  if (fe_split(self, removed_tree, removed, &removed_tree, &right) < 0)
  {
    self->root = fe_join(self, left, fe_join(self, removed_tree, right));
    fe_finalize_root(self, self->root);
    return -1;
  }

  /* Detach the two boundary leaves; their summaries are rebuilt in the
   * zone.  Original layout is prefix | la | removed_tree | lb | suffix. */
  fe_roll_init(&prime);
  zone.bytes = NULL;
  zone.len = 0;
  if (left != NULL)
  {
    prefix = fe_pop_right(self, left, &la);
    prime = la->prime;
    if (fe_zone_append(&zone, la->data, la->len) < 0)
      goto fail_restore;
  }
  if (inserted > 0 && fe_zone_append(&zone, insert, inserted) < 0)
    goto fail_restore;
  if (right != NULL)
  {
    suffix = fe_pop_left(self, right, &lb);
    if (fe_zone_append(&zone, lb->data, lb->len) < 0)
      goto fail_restore;
  }

  zone_tree = fe_zone_build(self, &zone, &prime);
  if (zone_tree == NULL && zone.len > 0)
    goto fail_restore;
  free(zone.bytes);
  zone.bytes = NULL;       /* fail_restore must not free it twice */

  /* Build and validate the replacement tree before reclaiming anything,
   * so an allocation failure still leaves every original piece alive. */
  joined = fe_join(self, fe_join(self, prefix, zone_tree), suffix);
  if (joined == NULL && new_len > 0)
    goto fail_restore;

  /* Success: the removed bytes and old boundary leaves are reclaimed
   * without any byte rescan.  Everything else keeps its summary.  The
   * reclaimed node count is proportional to the removed leaf count and
   * covered by the 16*ceil((k+r)/1024) visit allowance. */
  fe_reclaim(self, removed_tree);
  fe_reclaim(self, la);
  fe_reclaim(self, lb);

  fe_finalize_root(self, joined);
  self->root = joined;
  self->total = new_len;
  return 0;

fail_restore:
  free(zone.bytes);
  /* Reassemble the untouched original from the surviving valid pieces:
   * prefix | la | removed_tree | lb | suffix. */
  joined = fe_join(self, fe_join(self, prefix, la),
		   fe_join(self, removed_tree,
			   fe_join(self, lb, suffix)));
  fe_finalize_root(self, joined);
  self->root = joined;
  return -1;
}

/* ---------------- public constructor -------------------------------- */

struct fuzzy_edit *fuzzy_edit_new(const unsigned char *data, size_t length)
{
  struct fuzzy_edit *ed;
  struct fe_node **leaves;
  size_t cap, n = 0, done = 0, i;
  struct fe_node *root;
  struct fe_roll carry;
  if (length > FUZZY_EDIT_MAX_LENGTH)
    return NULL;
  if (length > 0 && data == NULL)
    return NULL;
  ed = calloc(1, sizeof(*ed));
  if (ed == NULL)
    return NULL;
  ed->total = length;
  if (length == 0)
    return ed;
  cap = length / FE_LEAF_MAX + 1;
  leaves = malloc(cap * sizeof(*leaves));
  if (leaves == NULL)
  {
    free(ed);
    return NULL;
  }
  fe_roll_init(&carry);
  while (done < length)
  {
    size_t step = length - done;
    struct fe_node *leaf;
    if (step > FE_LEAF_MAX)
      step = FE_LEAF_MAX;
    leaf = fe_leaf_newp(ed, data + done, step, &carry);
    if (leaf == NULL)
    {
      for (i = 0; i < n; ++i)
        fe_node_free(leaves[i]);
      free(leaves);
      free(ed);
      return NULL;
    }
    carry = leaf->roll;       /* end state primes the next leaf */
    leaves[n++] = leaf;
    done += step;
  }
  root = fe_build_tree(ed, leaves, n);
  free(leaves);
  if (root == NULL)
  {
    free(ed);
    return NULL;
  }
  fe_finalize_root(ed, root);
  ed->root = root;
  return ed;
}

void fuzzy_edit_free(struct fuzzy_edit *self)
{
  if (self == NULL)
    return;
  fe_node_free(self->root);
  free(self);
}

size_t fuzzy_edit_size(const struct fuzzy_edit *self)
{
  return self->total;
}

/* ---------------- digest formatting --------------------------------- */

static size_t fe_eliminate(char *dst, const char *src, size_t n)
{
  const char *end = src + n;
  if (src < end) *dst++ = *src++;
  if (src < end) *dst++ = *src++;
  if (src < end) *dst++ = *src++;
  while (src < end)
  {
    if (*src == dst[-1] && *src == dst[-2] && *src == dst[-3])
    {
      ++src;
      --n;
    }
    else
      *dst++ = *src++;
  }
  return n;
}

static uint32_t fe_bs(unsigned int index)
{
  return (uint32_t)FE_MIN_BLOCKSIZE << index;
}

/* Folded blockhash state for one level, derived from the root summary. */
struct fe_folded
{
  unsigned int dindex;
  char digest[FE_SPAMSUM + 1];
  unsigned char h;           /* running main hash at the buffer end */
  unsigned char halfh;       /* running half hash at the buffer end */
  char halfdigest;           /* half marker used when roll sum is zero */
};

static void fe_fold_level(const struct fe_level *lv,
			  struct fe_folded *out)
{
  uint32_t count = lv->count;
  unsigned char inv[64];
  uint32_t i;

  memset(out, 0, sizeof(*out));
  out->dindex = count < FE_SPAMSUM ? count : FE_SPAMSUM - 1;
  /* First 63 emitted characters are stable once written. */
  for (i = 0; i < out->dindex; ++i)
    out->digest[i] = fe_b64[lv->chars[i]];
  out->digest[out->dindex] = '\0';

  if (count == 0)
  {
    out->h = lv->main_end;
    out->halfh = lv->half_end;
    out->halfdigest = '\0';
    return;
  }

  /* tail maps a trigger-time running value to the value at the node end,
   * so the value at the last trigger is tail^{-1} of the end value. */
  fe_perm_invert(inv, lv->tail);

  out->h = lv->main_end;
  out->halfh = lv->half_end;
  if (count >= FE_SPAMSUM)
  {
    /* Saturated main thread: every later trigger overwrites the terminal
     * slot digest[63] with the running value at that trigger. */
    out->digest[FE_SPAMSUM - 1] = fe_b64[inv[lv->main_end]];
  }
  if (count < FE_HALF)
  {
    /* The half thread was still resetting at the final trigger, so the
     * engine cleared its retained marker and no tail character exists. */
    out->halfdigest = '\0';
  }
  else
  {
    /* From trigger 33 on the half thread never resets; its marker is
     * overwritten at every later trigger and ends on the last-trigger
     * running value. */
    out->halfdigest = fe_b64[inv[lv->half_end]];
  }
}

int fuzzy_edit_digest(struct fuzzy_edit *self, char *result,
		      unsigned int flags)
{
  struct fe_folded fold[FE_NUM_LEVELS];
  unsigned int bhstart = 0, bhend = 1, bi, i;
  uint_least64_t border;
  uint32_t h;
  size_t sz, remain = FUZZY_MAX_RESULT - 1;
  int isz;
  char ch;

  if (self == NULL || result == NULL)
    return -1;
  if (flags & ~(FUZZY_FLAG_ELIMSEQ | FUZZY_FLAG_NOTRUNC))
    return -1;
  if (self->total == 0)
  {
    memcpy(result, "3::", 4);   /* streaming an empty state yields "3::" */
    return 0;
  }

  for (i = 0; i < FE_NUM_LEVELS; ++i)
  {
    fe_fold_level(&self->root->level[i], &fold[i]);
    if (self->root->level[i].count > 0)
    {
      unsigned int want = i + 2;
      if (want > FE_NUM_LEVELS)
        want = FE_NUM_LEVELS;
      if (want > bhend)
        bhend = want;
    }
  }

  /* Reproduce the streaming blocksize reductions: the start level must
   * be saturated (64 triggers), the next level needs 32, and the doubled
   * border must stay below the total size. */
  border = (uint_least64_t)FE_MIN_BLOCKSIZE * FE_SPAMSUM;
  while (bhstart + 1 < bhend && border < self->total &&
	 self->root->level[bhstart].count >= FE_SPAMSUM &&
	 self->root->level[bhstart + 1].count >= FE_HALF)
  {
    ++bhstart;
    border *= 2;
  }

  /* Initial blocksize guess, then adapt to the actual digest length. */
  bi = bhstart;
  while ((uint_least64_t)fe_bs(bi) * FE_SPAMSUM < self->total)
    ++bi;
  if (bi >= bhend)
    bi = bhend - 1;
  while (bi > bhstart && fold[bi].dindex < FE_HALF)
    --bi;

  h = fe_roll_sum(&self->root->roll);

  isz = snprintf(result, remain, "%u:", fe_bs(bi));
  if (isz <= 0)
    return -1;
  sz = (size_t)isz;
  remain -= sz;
  result += sz;

  /* First component. */
  sz = fold[bi].dindex;
  if (flags & FUZZY_FLAG_ELIMSEQ)
    sz = fe_eliminate(result, fold[bi].digest, sz);
  else
    memcpy(result, fold[bi].digest, sz);
  result += sz;
  remain -= sz;
  ch = (h != 0) ? fe_b64[fold[bi].h]
		: fold[bi].digest[fold[bi].dindex];
  if (ch != '\0')
  {
    *result = ch;
    if (!(flags & FUZZY_FLAG_ELIMSEQ) || sz < 3 ||
	*result != result[-1] || *result != result[-2] ||
	*result != result[-3])
    {
      ++result;
      --remain;
    }
  }
  *result++ = ':';
  --remain;

  /* Second component. */
  if (bi + 1 < bhend)
  {
    unsigned int j = bi + 1;
    sz = fold[j].dindex;
    if (!(flags & FUZZY_FLAG_NOTRUNC) && sz > FE_HALF - 1)
      sz = FE_HALF - 1;
    if (flags & FUZZY_FLAG_ELIMSEQ)
      sz = fe_eliminate(result, fold[j].digest, sz);
    else
      memcpy(result, fold[j].digest, sz);
    result += sz;
    remain -= sz;
    ch = (h != 0)
      ? fe_b64[(flags & FUZZY_FLAG_NOTRUNC) ? fold[j].h
					    : fold[j].halfh]
      : ((flags & FUZZY_FLAG_NOTRUNC)
	 ? fold[j].digest[fold[j].dindex]
	 : fold[j].halfdigest);
    if (ch != '\0')
    {
      *result = ch;
      if (!(flags & FUZZY_FLAG_ELIMSEQ) || sz < 3 ||
	  *result != result[-1] || *result != result[-2] ||
	  *result != result[-3])
      {
        ++result;
        --remain;
      }
    }
  }
  else if (h != 0)
  {
    /* Nearly empty second component (the level-30 lasth special case
     * cannot occur within the 8 MiB buffer limit). */
    *result++ = fe_b64[fold[bi].h];
    --remain;
  }
  *result = '\0';
  (void)remain;
  return 0;
}

void fuzzy_edit_stats(const struct fuzzy_edit *self, uint64_t *raw_bytes,
		      uint64_t *node_visits)
{
  if (raw_bytes != NULL)
    *raw_bytes = self->raw_bytes;
  if (node_visits != NULL)
    *node_visits = self->node_visits;
}
