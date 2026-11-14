/*
 * sanity/edit_cases.c - incremental fuzzy_edit test cases.
 *
 * Driven by sanity/harness.c, which calls check_case() once for each of
 * the groups "compat", "splices", "boundaries" and "work".  Every group
 * makes its assertions through CHECK_CHECK; this file deliberately has
 * no main().  The independent slow reference only uses the public
 * streaming interface (fuzzy_new / fuzzy_update / fuzzy_digest); it never
 * reaches into the incremental structure or its internal merge helpers.
 */

#include "harness.h"

#include "fuzzy.h"
#include "fuzzy_edit.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Deterministic generator: fixed seed, no clock or environment input. */
static uint32_t ec_rng;

static void ec_seed(uint32_t s)
{
  ec_rng = s;
}

static unsigned char ec_next(void)
{
  ec_rng = ec_rng * 1664525u + 1013904223u;
  return (unsigned char)(ec_rng >> 24);
}

static void ec_fill(unsigned char *buf, size_t n)
{
  size_t i;
  for (i = 0; i < n; ++i)
    buf[i] = ec_next();
}

/* Independent streaming reference, fed in deliberately uneven chunks. */
static void ec_stream(const unsigned char *data, size_t n,
		      unsigned int flags, char *out)
{
  struct fuzzy_state *st = fuzzy_new();
  size_t i = 0;
  CHECK_CHECK(st != NULL);
  while (i < n)
  {
    size_t step = (i % 37u) + 1u;
    if (step > n - i)
      step = n - i;
    CHECK_CHECK(fuzzy_update(st, data + i, step) == 0);
    i += step;
  }
  CHECK_CHECK(fuzzy_digest(st, out, flags) == 0);
  fuzzy_free(st);
}

/* All four flag combinations must match the streaming reference. */
static void ec_match(const struct fuzzy_edit *ed,
		     const unsigned char *buf, size_t n)
{
  char a[FUZZY_MAX_RESULT], b[FUZZY_MAX_RESULT];
  unsigned int flags;
  CHECK_CHECK(fuzzy_edit_size(ed) == n);
  for (flags = 0; flags < 4; ++flags)
  {
    CHECK_CHECK(fuzzy_edit_digest((struct fuzzy_edit *)ed, a, flags) == 0);
    ec_stream(buf, n, flags, b);
    CHECK_CHECK(strcmp(a, b) == 0);
  }
}

/* Exact node-visit allowance from the documented work bound. */
static uint64_t ec_visit_limit(size_t n, size_t k, size_t r)
{
  uint64_t ceil_n = n / 1024u + (n % 1024u ? 1u : 0u);
  uint64_t x = 1u + ceil_n, lg = 0;
  while (x > 1)
  {
    x = (x + 1u) / 2u;       /* ceil(log2) */
    ++lg;
  }
  return 256u * (1u + lg) + 16u * ((k + r + 1023u) / 1024u);
}

/*
 * Run one splice, mirror it in the flat buffer, perform the four flag
 * queries, and verify both work allowances.  The visit allowance uses the
 * maximum of the pre- and post-edit lengths, exactly as the bound is
 * stated.
 */
static void ec_edit(struct fuzzy_edit *ed, unsigned char *buf,
		    size_t *np, size_t offset, size_t removed,
		    const unsigned char *insert, size_t inserted)
{
  uint64_t rb0, nv0, rb1, nv1;
  size_t n = *np;
  size_t new_n = n - removed + inserted;
  size_t nmax = n > new_n ? n : new_n;
  CHECK_CHECK(offset <= n && removed <= n - offset);
  fuzzy_edit_stats(ed, &rb0, &nv0);
  CHECK_CHECK(fuzzy_edit_splice(ed, offset, removed,
				inserted ? insert : NULL, inserted) == 0);
  memmove(buf + offset + inserted, buf + offset + removed,
	  n - offset - removed);
  if (inserted)
    memcpy(buf + offset, insert, inserted);
  *np = new_n;
  {
    char d[FUZZY_MAX_RESULT];
    unsigned int flags;
    for (flags = 0; flags < 4; ++flags)
      CHECK_CHECK(fuzzy_edit_digest(ed, d, flags) == 0);
  }
  fuzzy_edit_stats(ed, &rb1, &nv1);
  CHECK_CHECK(rb1 - rb0 <= inserted + 65536u);
  CHECK_CHECK(nv1 - nv0 <= ec_visit_limit(nmax, inserted, removed));
}

/* ---------------- group: compat ------------------------------------- */

static void ec_group_compat(void)
{
  static const size_t sizes[] = {
    0, 1, 6, 7, 8, 63, 64, 100, 1023, 1024, 1025, 4096, 4097,
    65536, 100003
  };
  size_t si;
  for (si = 0; si < sizeof(sizes) / sizeof(sizes[0]); ++si)
  {
    size_t n = sizes[si];
    unsigned char *buf = malloc(n ? n : 1);
    struct fuzzy_edit *ed;
    CHECK_CHECK(buf != NULL);
    ec_seed(0x1234u + (uint32_t)n);
    ec_fill(buf, n);
    ed = fuzzy_edit_new(n ? buf : NULL, n);
    CHECK_CHECK(ed != NULL);
    ec_match(ed, buf, n);
    fuzzy_edit_free(ed);
    free(buf);
  }

  /* Every byte value, including NUL and high bytes, round trips. */
  {
    unsigned char all[256];
    struct fuzzy_edit *ed;
    int i;
    for (i = 0; i < 256; ++i)
      all[i] = (unsigned char)i;
    ed = fuzzy_edit_new(all, sizeof(all));
    CHECK_CHECK(ed != NULL);
    ec_match(ed, all, sizeof(all));
    fuzzy_edit_free(ed);
  }
}

/* ---------------- group: splices ------------------------------------ */

static void ec_group_splices(void)
{
  size_t cap = 200000u;
  unsigned char *buf = malloc(cap);
  size_t n = 40000u, i;
  struct fuzzy_edit *ed;
  CHECK_CHECK(buf != NULL);
  ec_seed(0xC0FFEEu);
  ec_fill(buf, n);
  ed = fuzzy_edit_new(buf, n);
  CHECK_CHECK(ed != NULL);

  for (i = 0; i < 250; ++i)
  {
    size_t off = n ? (size_t)ec_next() * n / 256u : 0;
    size_t removed = (i % 7u == 0u) ? 0u : (size_t)ec_next() % 191u;
    size_t inserted = (i % 5u == 0u) ? 0u : (size_t)ec_next() % 257u;
    unsigned char tmp[300];
    size_t j;
    if (off > n) off = n;
    if (removed > n - off) removed = n - off;
    if (n - removed + inserted > cap) inserted = 0;
    for (j = 0; j < inserted; ++j)
      tmp[j] = ec_next();
    ec_edit(ed, buf, &n, off, removed, tmp, inserted);
    if ((i % 25u) == 0u)
      ec_match(ed, buf, n);
  }
  ec_match(ed, buf, n);

  /* A failed splice must leave the buffer and every digest unchanged. */
  {
    char good[FUZZY_MAX_RESULT], again[FUZZY_MAX_RESULT];
    size_t before = fuzzy_edit_size(ed);
    CHECK_CHECK(fuzzy_edit_digest(ed, good, 0) == 0);
    CHECK_CHECK(fuzzy_edit_splice(ed, n + 1u, 0, NULL, 0) == -1);
    CHECK_CHECK(fuzzy_edit_splice(ed, 0, n + 1u, NULL, 0) == -1);
    CHECK_CHECK(fuzzy_edit_splice(ed, 0, 1, NULL, 1) == -1);
    CHECK_CHECK(fuzzy_edit_digest(ed, again, 0) == 0);
    CHECK_CHECK(strcmp(good, again) == 0);
    CHECK_CHECK(fuzzy_edit_size(ed) == before);
    ec_match(ed, buf, n);
  }

  /* Repeated digest queries add no byte reads and no node visits. */
  {
    uint64_t rb0, nv0, rb1, nv1;
    char d[FUZZY_MAX_RESULT];
    unsigned int f;
    fuzzy_edit_stats(ed, &rb0, &nv0);
    for (f = 0; f < 16u; ++f)
      CHECK_CHECK(fuzzy_edit_digest(ed, d, f & 3u) == 0);
    fuzzy_edit_stats(ed, &rb1, &nv1);
    CHECK_CHECK(rb0 == rb1);
    CHECK_CHECK(nv0 == nv1);
  }

  fuzzy_edit_free(ed);
  free(buf);
}

/* ---------------- group: boundaries --------------------------------- */

static void ec_group_boundaries(void)
{
  size_t cap = 200000u;
  unsigned char *buf = malloc(cap);
  struct fuzzy_edit *ed;
  size_t n, i;
  CHECK_CHECK(buf != NULL);

  /* Long zero run: all-zero stretches, repeated bytes and sparse
   * non-zero triggers, exercising tails that contain no reset point. */
  n = 120000u;
  memset(buf, 0, n);
  ed = fuzzy_edit_new(buf, n);
  CHECK_CHECK(ed != NULL);
  ec_match(ed, buf, n);
  for (i = 0; i < 80; ++i)
  {
    size_t off = (i * 1301u) % (n + 1u);
    size_t removed = (i * 7u) % 53u;
    unsigned char one = (unsigned char)(i + 1u);
    if (removed > n - off) removed = n - off;
    ec_edit(ed, buf, &n, off, removed, &one, 1u);
  }
  ec_match(ed, buf, n);
  fuzzy_edit_free(ed);

  /* Insert fewer than seven bytes between two existing blocks: the
   * rolling window crosses the seam and no new chunk point is assumed. */
  n = 20000u;
  ec_seed(777u);
  ec_fill(buf, n);
  ed = fuzzy_edit_new(buf, n);
  CHECK_CHECK(ed != NULL);
  for (i = 1; i <= 6; ++i)
  {
    unsigned char tiny[6];
    size_t j;
    for (j = 0; j < i; ++j)
      tiny[j] = (unsigned char)(0xA0u + j);
    ec_edit(ed, buf, &n, 5000u + i, 0, tiny, i);
  }
  ec_match(ed, buf, n);

  /* Delete an existing trigger region, then grow and shrink back so the
   * chosen blocksize rises and falls again. */
  for (i = 0; i < 6; ++i)
  {
    size_t add = 45000u;
    unsigned char *grow = malloc(add);
    CHECK_CHECK(grow != NULL);
    ec_fill(grow, add);
    ec_edit(ed, buf, &n, n, 0, grow, add);
    free(grow);
    ec_match(ed, buf, n);
    ec_edit(ed, buf, &n, n - add, add, NULL, 0);
    ec_match(ed, buf, n);
  }

  /* Component truncation and sequence elimination: every flag output
   * matches the streaming reference.  A dedicated larger material below
   * is guaranteed to make the flag variants differ from the default. */
  {
    char def[FUZZY_MAX_RESULT], notrunc[FUZZY_MAX_RESULT],
         elim[FUZZY_MAX_RESULT], both[FUZZY_MAX_RESULT];
    CHECK_CHECK(fuzzy_edit_digest(ed, def, 0) == 0);
    CHECK_CHECK(fuzzy_edit_digest(ed, notrunc, FUZZY_FLAG_NOTRUNC) == 0);
    CHECK_CHECK(fuzzy_edit_digest(ed, elim, FUZZY_FLAG_ELIMSEQ) == 0);
    CHECK_CHECK(fuzzy_edit_digest(ed, both,
		FUZZY_FLAG_ELIMSEQ | FUZZY_FLAG_NOTRUNC) == 0);
    ec_match(ed, buf, n);
  }

  /* Component truncation.  The trigger rate depends on the bytes; the
   * baseline generator (LCG seeded to 7) produces more than 32 second
   * component pieces by 768 bytes, so default and NOTRUNC diverge there.
   * The incremental output must match the streaming reference exactly. */
  {
    size_t m = 768u;
    unsigned char *big = malloc(m);
    struct fuzzy_edit *eb;
    uint32_t x = 7u;
    char def[FUZZY_MAX_RESULT], nt[FUZZY_MAX_RESULT],
         rdef[FUZZY_MAX_RESULT], rnt[FUZZY_MAX_RESULT];
    size_t i;
    CHECK_CHECK(big != NULL);
    for (i = 0; i < m; ++i)
    {
      x = x * 1664525u + 1013904223u;
      big[i] = (unsigned char)(x >> 24);
    }
    ec_stream(big, m, 0, rdef);
    ec_stream(big, m, FUZZY_FLAG_NOTRUNC, rnt);
    CHECK_CHECK(strcmp(rdef, rnt) != 0);     /* truncation is active */
    eb = fuzzy_edit_new(big, m);
    CHECK_CHECK(eb != NULL);
    CHECK_CHECK(fuzzy_edit_digest(eb, def, 0) == 0);
    CHECK_CHECK(fuzzy_edit_digest(eb, nt, FUZZY_FLAG_NOTRUNC) == 0);
    CHECK_CHECK(strcmp(def, rdef) == 0);
    CHECK_CHECK(strcmp(nt, rnt) == 0);
    fuzzy_edit_free(eb);
    free(big);
  }

  /* Repeated bytes: ELIMSEQ collapses runs and therefore differs from
   * the default component, and both match the streaming reference. */
  {
    size_t m = 65536u;
    unsigned char *rep = malloc(m);
    struct fuzzy_edit *er;
    char def[FUZZY_MAX_RESULT], es[FUZZY_MAX_RESULT],
         rdef[FUZZY_MAX_RESULT], res[FUZZY_MAX_RESULT];
    CHECK_CHECK(rep != NULL);
    memset(rep, 4, m);
    er = fuzzy_edit_new(rep, m);
    CHECK_CHECK(er != NULL);
    CHECK_CHECK(fuzzy_edit_digest(er, def, 0) == 0);
    CHECK_CHECK(fuzzy_edit_digest(er, es, FUZZY_FLAG_ELIMSEQ) == 0);
    ec_stream(rep, m, 0, rdef);
    ec_stream(rep, m, FUZZY_FLAG_ELIMSEQ, res);
    CHECK_CHECK(strcmp(def, rdef) == 0);
    CHECK_CHECK(strcmp(es, res) == 0);
    CHECK_CHECK(strcmp(def, es) != 0);
    fuzzy_edit_free(er);
    free(rep);
  }

  /* Tail ending in a zero rolling state: shrink to empty and back. */
  CHECK_CHECK(fuzzy_edit_splice(ed, 0, n, NULL, 0) == 0);
  n = 0;
  CHECK_CHECK(fuzzy_edit_size(ed) == 0);
  {
    char d[FUZZY_MAX_RESULT];
    CHECK_CHECK(fuzzy_edit_digest(ed, d, 3) == 0);
    CHECK_CHECK(strcmp(d, "3::") == 0);
  }
  ec_edit(ed, buf, &n, 0, 0, buf, 123u);
  ec_match(ed, buf, n);

  fuzzy_edit_free(ed);
  free(buf);
}

/* ---------------- group: work --------------------------------------- */

/* One full work campaign on a material of size n.  zero_run selects
 * between fixed-seed pseudo-random bytes and a long zero string. */
static void ec_work_campaign(size_t n, int zero_run)
{
  unsigned char *buf = malloc(n + 4096u);
  unsigned char *patch;
  struct fuzzy_edit *ed;
  size_t k = 4096u, cur;
  CHECK_CHECK(buf != NULL);
  if (zero_run)
    memset(buf, 0, n);
  else
  {
    ec_seed(0x5EEDu ^ (uint32_t)n);
    ec_fill(buf, n);
  }
  ed = fuzzy_edit_new(buf, n);
  CHECK_CHECK(ed != NULL);
  ec_match(ed, buf, n);
  cur = n;

  patch = malloc(k);
  CHECK_CHECK(patch != NULL);
  ec_seed(0x1111u + (uint32_t)n + (zero_run ? 1u : 0u));
  ec_fill(patch, k);

  /* Equal-length replacement in the middle. */
  ec_edit(ed, buf, &cur, cur / 2u, k, patch, k);
  ec_match(ed, buf, cur);

  /* Front insertion, when the 8 MiB ceiling allows it. */
  if (cur + k <= FUZZY_EDIT_MAX_LENGTH)
  {
    ec_edit(ed, buf, &cur, 0, 0, patch, k);
    ec_match(ed, buf, cur);
  }

  /* Large deletion, then insert a run of the same length back. */
  {
    size_t deleted = cur / 4u;
    unsigned char *back = malloc(deleted);
    CHECK_CHECK(back != NULL);
    if (zero_run)
      memset(back, 0, deleted);
    else
      ec_fill(back, deleted);
    ec_edit(ed, buf, &cur, cur / 3u, deleted, NULL, 0);
    ec_match(ed, buf, cur);
    ec_edit(ed, buf, &cur, cur / 3u, 0, back, deleted);
    ec_match(ed, buf, cur);
    free(back);
  }

  fuzzy_edit_free(ed);
  free(patch);
  free(buf);
}

static void ec_group_work(void)
{
  static const size_t materials[] = {
    1u << 20, 2u << 20, 4u << 20, 8u << 20
  };
  size_t i;
  for (i = 0; i < sizeof(materials) / sizeof(materials[0]); ++i)
  {
    ec_work_campaign(materials[i], 0);   /* fixed-seed bytes */
    ec_work_campaign(materials[i], 1);   /* long zero string */
  }
}

/* ---------------- entry point --------------------------------------- */

void check_case(const char *group)
{
  if (strcmp(group, "compat") == 0)
    ec_group_compat();
  else if (strcmp(group, "splices") == 0)
    ec_group_splices();
  else if (strcmp(group, "boundaries") == 0)
    ec_group_boundaries();
  else if (strcmp(group, "work") == 0)
    ec_group_work();
  else
    CHECK_CHECK(0);
}
