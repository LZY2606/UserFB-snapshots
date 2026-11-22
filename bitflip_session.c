/*
  Copyright 2024 Google LLC All rights reserved.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at:

    http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

/*
   american fuzzy lop - resumable deterministic bitflip prelude
   -------------------------------------------------------------

   See bitflip_session.h for the contract. This file is deliberately free of
   any afl-fuzz.c global state: it is a plain, re-entrant state machine over
   the exact iteration order of the original fuzz_one() bitflip stages.
*/

#include "bitflip_session.h"
#include "alloc-inl.h"
#include "debug.h"
#include "hash.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* Effector map geometry, identical to the macros used by fuzz_one(). */

#define BFS_EFF_APOS(_p)          ((_p) >> EFF_MAP_SCALE2)
#define BFS_EFF_REM(_x)           ((_x) & ((1 << EFF_MAP_SCALE2) - 1))
#define BFS_EFF_ALEN(_l)          (BFS_EFF_APOS(_l) + !!BFS_EFF_REM(_l))

#define BFS_FLIP_BIT(_ar, _b) do { \
    u8* _arf = (u8*)(_ar); \
    u32 _bf = (_b); \
    _arf[(_bf) >> 3] ^= (128 >> ((_bf) & 7)); \
  } while (0)

/* Pending auto-dictionary proposal: the collected bytes plus their length. */

struct bfs_token {
  u8  data[MAX_AUTO_EXTRA];
  u32 len;
};

struct bitflip_session {

  /* Identity / options. */
  u32 input_len;
  u32 base_cksum;
  u8  dumb_mode;

  u8 *input;                        /* immutable original input            */
  u8 *work;                         /* mutable candidate buffer            */
  u8 *eff_map;                      /* effector map, BFS_EFF_ALEN(len)     */

  /* Progression: stage id (0..5) and stage-local iteration index. */
  u8  stage;
  u32 pos;                          /* bit index or byte index of stage    */
  u32 stage_max;                    /* nominal max of the active stage     */
  u32 eff_cnt;                      /* entries flagged in effector map     */
  u8  eff_densified;                /* map was force-filled at flip8 end   */

  /* flip1 dictionary construction state. */
  u8  a_collect[MAX_AUTO_EXTRA];
  u32 a_len;
  u32 prev_cksum;

  /* Token proposals accepted in order, held until prelude completion. */
  struct bfs_token *tokens;
  u32 token_cnt;
  u8  tokens_delivered;

  /* Accounting. */
  u64 stage_execs[BFS_STAGES];      /* actually submitted candidates       */
  u64 total_execs;
  u64 budget_left;
  u8  status;                       /* BFS_PAUSED / BFS_COMPLETE / ERROR   */

  /* Callbacks. */
  bitflip_exec_cb   exec_cb;
  bitflip_token_cb  token_cb;
  bitflip_event_cb  event_cb;
  void             *userdata;

};

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static void bfs_event(struct bitflip_session *s, int event, u64 a, u64 b) {

  if (s->event_cb) s->event_cb(event, s->stage, a, b, s->userdata);

}

static u32 bfs_nominal_max(int stage, u32 len) {

  switch (stage) {

    case BFS_FLIP1:  return len << 3;
    case BFS_FLIP2:  return (len << 3) - 1;
    case BFS_FLIP4:  return (len << 3) - 3;
    case BFS_FLIP8:  return len;
    case BFS_FLIP16: return len ? len - 1 : 0;
    case BFS_FLIP32: return len >= 4 ? len - 3 : 0;
    default:         return 0;

  }

}

static void bfs_store_token(struct bitflip_session *s, const u8 *mem,
                            u32 len) {

  struct bfs_token *grown;

  /* Only dictionary-length proposals are ever accepted; the bounds check
     also keeps a corrupt checkpoint from overrunning the inline buffer. */

  if (len < MIN_AUTO_EXTRA || len > MAX_AUTO_EXTRA) return;

  grown = ck_realloc_block(s->tokens,
                           (s->token_cnt + 1) * sizeof(struct bfs_token));
  s->tokens = grown;
  memcpy(s->tokens[s->token_cnt].data, mem, len);
  s->tokens[s->token_cnt].len = len;
  s->token_cnt++;

}

/* Submit one candidate. The mutation must already be applied to s->work; the
   flip-back is performed by the caller. Returns 0 on success, -1 when the
   execution callback failed (candidate not billed, state not advanced). */

static int bfs_submit(struct bitflip_session *s, u32 byte_pos,
                      u32 *cksum_out) {

  u32 cksum = 0;

  if (!s->budget_left) return -2;

  if (s->exec_cb(s->stage, byte_pos, s->work, s->input_len,
                 &cksum, s->userdata)) {
    s->status = BFS_ERROR;
    return -1;
  }

  s->budget_left--;
  s->total_execs++;
  s->stage_execs[s->stage]++;
  if (cksum_out) *cksum_out = cksum;
  return 0;

}

/* ------------------------------------------------------------------ */
/* Stage 1: single walking bit with dictionary construction           */
/* ------------------------------------------------------------------ */

static int bfs_run_flip1(struct bitflip_session *s) {

  u32 stage_max = bfs_nominal_max(BFS_FLIP1, s->input_len);

  while (s->pos < stage_max) {

    u32 bit = s->pos;
    u32 byte_pos = bit >> 3;
    u32 cksum;
    int res;

    BFS_FLIP_BIT(s->work, bit);

    res = bfs_submit(s, byte_pos, &cksum);

    if (res == -2) {                        /* budget exhausted pre-submit  */
      BFS_FLIP_BIT(s->work, bit);
      return BFS_PAUSED;
    }

    if (res == -1) {                        /* exec failed: retry same bit  */
      BFS_FLIP_BIT(s->work, bit);
      return BFS_ERROR;
    }

    BFS_FLIP_BIT(s->work, bit);

    /* Dictionary trick, evaluated on the least-significant bit of each
       byte, exactly as the original code does. */

    if (!s->dumb_mode && (bit & 7) == 7) {

      if (bit == stage_max - 1 && cksum == s->prev_cksum) {

        /* End of file while still collecting: force the final token out. */

        if (s->a_len < MAX_AUTO_EXTRA)
          s->a_collect[s->a_len] = s->work[bit >> 3];
        s->a_len++;

        if (s->a_len >= MIN_AUTO_EXTRA && s->a_len <= MAX_AUTO_EXTRA)
          bfs_store_token(s, s->a_collect, s->a_len);

      } else if (cksum != s->prev_cksum) {

        if (s->a_len >= MIN_AUTO_EXTRA && s->a_len <= MAX_AUTO_EXTRA)
          bfs_store_token(s, s->a_collect, s->a_len);

        s->a_len = 0;
        s->prev_cksum = cksum;

      }

      /* Keep collecting only when the flip changed the path. */

      if (cksum != s->base_cksum) {

        if (s->a_len < MAX_AUTO_EXTRA)
          s->a_collect[s->a_len] = s->work[bit >> 3];
        s->a_len++;

      }

    }

    s->pos++;

  }

  return BFS_COMPLETE;

}

/* ------------------------------------------------------------------ */
/* Stages 2 and 3: two / four walking bits                            */
/* ------------------------------------------------------------------ */

static int bfs_run_flip2(struct bitflip_session *s) {

  u32 stage_max = bfs_nominal_max(BFS_FLIP2, s->input_len);

  while (s->pos < stage_max) {

    u32 bit = s->pos;
    int res;

    BFS_FLIP_BIT(s->work, bit);
    BFS_FLIP_BIT(s->work, bit + 1);

    res = bfs_submit(s, bit >> 3, NULL);

    BFS_FLIP_BIT(s->work, bit);
    BFS_FLIP_BIT(s->work, bit + 1);

    if (res == -2) return BFS_PAUSED;
    if (res == -1) return BFS_ERROR;

    s->pos++;

  }

  return BFS_COMPLETE;

}

static int bfs_run_flip4(struct bitflip_session *s) {

  u32 stage_max = bfs_nominal_max(BFS_FLIP4, s->input_len);

  while (s->pos < stage_max) {

    u32 bit = s->pos;
    int res;

    BFS_FLIP_BIT(s->work, bit);
    BFS_FLIP_BIT(s->work, bit + 1);
    BFS_FLIP_BIT(s->work, bit + 2);
    BFS_FLIP_BIT(s->work, bit + 3);

    res = bfs_submit(s, bit >> 3, NULL);

    BFS_FLIP_BIT(s->work, bit);
    BFS_FLIP_BIT(s->work, bit + 1);
    BFS_FLIP_BIT(s->work, bit + 2);
    BFS_FLIP_BIT(s->work, bit + 3);

    if (res == -2) return BFS_PAUSED;
    if (res == -1) return BFS_ERROR;

    s->pos++;

  }

  return BFS_COMPLETE;

}

/* ------------------------------------------------------------------ */
/* Stage 4: walking byte, also building the effector map              */
/* ------------------------------------------------------------------ */

static int bfs_run_flip8(struct bitflip_session *s) {

  u32 stage_max = bfs_nominal_max(BFS_FLIP8, s->input_len);

  while (s->pos < stage_max) {

    u32 byte_pos = s->pos;
    u32 cksum;
    int res;

    s->work[byte_pos] ^= 0xFF;

    res = bfs_submit(s, byte_pos, &cksum);

    if (res == -2) {
      s->work[byte_pos] ^= 0xFF;
      return BFS_PAUSED;
    }

    if (res == -1) {
      s->work[byte_pos] ^= 0xFF;
      return BFS_ERROR;
    }

    if (!s->eff_map[BFS_EFF_APOS(byte_pos)]) {

      u32 eff_cksum;

      /* For short inputs or dumb mode everything is flagged. */

      if (!s->dumb_mode && s->input_len >= EFF_MIN_LEN)
        eff_cksum = cksum;
      else
        eff_cksum = ~s->base_cksum;

      if (eff_cksum != s->base_cksum) {
        s->eff_map[BFS_EFF_APOS(byte_pos)] = 1;
        s->eff_cnt++;
      }

    }

    s->work[byte_pos] ^= 0xFF;
    s->pos++;

  }

  /* Densify if the map is more than EFF_MAX_PERC full. */

  if (s->eff_cnt != BFS_EFF_ALEN(s->input_len) &&
      s->eff_cnt * 100 / BFS_EFF_ALEN(s->input_len) > EFF_MAX_PERC) {

    memset(s->eff_map, 1, BFS_EFF_ALEN(s->input_len));
    s->eff_densified = 1;
    bfs_event(s, BFS_EV_EFF_STATS, BFS_EFF_ALEN(s->input_len),
              BFS_EFF_ALEN(s->input_len));

  } else {

    bfs_event(s, BFS_EV_EFF_STATS, BFS_EFF_ALEN(s->input_len), s->eff_cnt);

  }

  return BFS_COMPLETE;

}

/* ------------------------------------------------------------------ */
/* Stages 5 and 6: two / four walking bytes with effector skipping    */
/* ------------------------------------------------------------------ */

static int bfs_run_flip16(struct bitflip_session *s) {

  u32 limit = s->input_len >= 2 ? s->input_len - 1 : 0;

  while (s->pos < limit) {

    u32 i = s->pos;
    u16 orig;
    int res;

    if (!s->eff_map[BFS_EFF_APOS(i)] && !s->eff_map[BFS_EFF_APOS(i + 1)]) {
      s->pos++;                               /* skipped: not billed        */
      continue;
    }

    orig = *(u16 *)(s->work + i);
    *(u16 *)(s->work + i) ^= 0xFFFF;

    res = bfs_submit(s, i, NULL);

    *(u16 *)(s->work + i) = orig;

    if (res == -2) return BFS_PAUSED;
    if (res == -1) return BFS_ERROR;

    s->pos++;

  }

  return BFS_COMPLETE;

}

static int bfs_run_flip32(struct bitflip_session *s) {

  u32 limit = s->input_len >= 4 ? s->input_len - 3 : 0;

  while (s->pos < limit) {

    u32 i = s->pos;
    u32 orig;
    int res;

    if (!s->eff_map[BFS_EFF_APOS(i)] &&
        !s->eff_map[BFS_EFF_APOS(i + 1)] &&
        !s->eff_map[BFS_EFF_APOS(i + 2)] &&
        !s->eff_map[BFS_EFF_APOS(i + 3)]) {
      s->pos++;                               /* skipped: not billed        */
      continue;
    }

    orig = *(u32 *)(s->work + i);
    *(u32 *)(s->work + i) ^= 0xFFFFFFFF;

    res = bfs_submit(s, i, NULL);

    *(u32 *)(s->work + i) = orig;

    if (res == -2) return BFS_PAUSED;
    if (res == -1) return BFS_ERROR;

    s->pos++;

  }

  return BFS_COMPLETE;

}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                          */
/* ------------------------------------------------------------------ */

/* Silent core allocator: no lifecycle events, shared by open() and load(). */

static struct bitflip_session *bfs_alloc_core(const u8 *input, u32 len,
                                              u32 base_cksum, u8 dumb_mode,
                                              bitflip_exec_cb exec_cb,
                                              bitflip_token_cb token_cb,
                                              bitflip_event_cb event_cb,
                                              void *userdata) {

  struct bitflip_session *s;
  u32 eff_len;

  if (!input || !len || !exec_cb) return NULL;

  s = ck_alloc(sizeof(struct bitflip_session));

  s->input_len  = len;
  s->base_cksum = base_cksum;
  s->dumb_mode  = dumb_mode;

  s->input = ck_memdup((void *)input, len);
  s->work  = ck_memdup((void *)input, len);

  eff_len = BFS_EFF_ALEN(len);
  s->eff_map = ck_alloc(eff_len);

  /* First and last byte always count as effector positions. */

  s->eff_map[0] = 1;
  s->eff_cnt = 1;

  if (BFS_EFF_APOS(len - 1) != 0) {
    s->eff_map[BFS_EFF_APOS(len - 1)] = 1;
    s->eff_cnt++;
  }

  s->stage       = BFS_FLIP1;
  s->pos         = 0;
  s->stage_max   = bfs_nominal_max(BFS_FLIP1, len);
  s->prev_cksum  = base_cksum;
  s->status      = BFS_PAUSED;

  s->exec_cb  = exec_cb;
  s->token_cb = token_cb;
  s->event_cb = event_cb;
  s->userdata = userdata;

  return s;

}

struct bitflip_session *bitflip_session_open(const u8 *input, u32 len,
                                             u32 base_cksum, u8 dumb_mode,
                                             bitflip_exec_cb exec_cb,
                                             bitflip_token_cb token_cb,
                                             bitflip_event_cb event_cb,
                                             void *userdata) {

  struct bitflip_session *s =
    bfs_alloc_core(input, len, base_cksum, dumb_mode, exec_cb, token_cb,
                   event_cb, userdata);

  /* Announce the first stage exactly like every subsequent transition. */

  if (s) bfs_event(s, BFS_EV_STAGE_ENTER, s->stage_max, 0);

  return s;

}

void bitflip_session_close(struct bitflip_session *s) {

  if (!s) return;
  ck_free(s->input);
  ck_free(s->work);
  ck_free(s->eff_map);
  ck_free(s->tokens);
  ck_free(s);

}

static int bfs_advance_stage(struct bitflip_session *s) {

  int rc = BFS_COMPLETE;

  switch (s->stage) {

    case BFS_FLIP1:  rc = bfs_run_flip1(s);  break;
    case BFS_FLIP2:  rc = bfs_run_flip2(s);  break;
    case BFS_FLIP4:  rc = bfs_run_flip4(s);  break;
    case BFS_FLIP8:  rc = bfs_run_flip8(s);  break;
    case BFS_FLIP16: rc = bfs_run_flip16(s); break;
    case BFS_FLIP32: rc = bfs_run_flip32(s); break;
    default:         s->status = BFS_ERROR; return BFS_ERROR;

  }

  if (rc != BFS_COMPLETE) {
    s->status = rc;
    return rc;
  }

  /* Stage finished: report actual execs, then arm the next one. */

  bfs_event(s, BFS_EV_STAGE_EXIT, s->stage_execs[s->stage], 0);

  if (s->stage == BFS_FLIP32) {

    /* Prelude done: hand all token proposals over in original order. */

    if (s->token_cb) {
      u32 i;
      for (i = 0; i < s->token_cnt; i++)
        s->token_cb(s->tokens[i].data, s->tokens[i].len, s->userdata);
    }

    s->tokens_delivered = 1;
    s->status = BFS_COMPLETE;
    return BFS_COMPLETE;

  }

  s->stage++;
  s->pos = 0;
  s->stage_max = bfs_nominal_max(s->stage, s->input_len);

  bfs_event(s, BFS_EV_STAGE_ENTER, s->stage_max, 0);

  return BFS_COMPLETE;

}

int bitflip_session_run(struct bitflip_session *s, u64 budget) {

  if (!s) return BFS_ERROR;
  if (s->status == BFS_COMPLETE) return BFS_COMPLETE;

  /* A previous call ended on a failed candidate. Its position was never
     advanced, so re-arm to PAUSED and let this call retry it. */

  if (s->status == BFS_ERROR) s->status = BFS_PAUSED;

  s->budget_left = budget;

  if (!budget) return BFS_PAUSED;

  while (s->status != BFS_COMPLETE) {

    int rc = bfs_advance_stage(s);

    if (rc == BFS_PAUSED || rc == BFS_ERROR) return rc;

  }

  return BFS_COMPLETE;

}

/* ------------------------------------------------------------------ */
/* Accessors                                                          */
/* ------------------------------------------------------------------ */

int  bitflip_session_status(const struct bitflip_session *s)
  { return s ? s->status : BFS_ERROR; }
int  bitflip_session_stage(const struct bitflip_session *s)
  { return s ? s->stage : -1; }
u32  bitflip_session_len(const struct bitflip_session *s)
  { return s ? s->input_len : 0; }
u64  bitflip_session_execs(const struct bitflip_session *s)
  { return s ? s->total_execs : 0; }
u64  bitflip_session_stage_execs(const struct bitflip_session *s, int stage)
  { return (s && stage >= 0 && stage < BFS_STAGES) ? s->stage_execs[stage] : 0; }
u64  bitflip_session_budget_left(const struct bitflip_session *s)
  { return s ? s->budget_left : 0; }
u32  bitflip_session_stage_index(const struct bitflip_session *s)
  { return s ? s->pos : 0; }
u32  bitflip_session_stage_max(const struct bitflip_session *s)
  { return s ? s->stage_max : 0; }

const u8 *bitflip_session_input(const struct bitflip_session *s)
  { return s ? s->input : NULL; }

const u8 *bitflip_session_eff_map(const struct bitflip_session *s,
                                  u32 *eff_len) {

  if (!s) return NULL;
  if (eff_len) *eff_len = BFS_EFF_ALEN(s->input_len);
  return s->eff_map;

}

/* ------------------------------------------------------------------ */
/* Checkpoint format                                                  */
/* ------------------------------------------------------------------ */

/*
 * Layout (fixed-width integer types, byte-packed; integers are stored in
 * native byte order, so a checkpoint is portable across processes on the
 * same architecture):
 *
 *   struct bfs_header hdr           magic/version/lengths/options/identity
 *   u8  input[input_len]            original input bytes
 *   u8  work[input_len]             current work buffer (== input at rest)
 *   u8  eff_map[eff_len]
 *   u8  a_collect[MAX_AUTO_EXTRA]   in-progress flip1 token collection
 *   struct bfs_token_hdr th[0..]    token table
 *   u8  token bytes concatenated
 *   u32 crc32                       CRC32 of every preceding byte
 *
 * Identity is bound via FNV-1a hashes of the input and the full option set;
 * load() additionally requires the caller to present the same input bytes,
 * base checksum and dumb_mode flag explicitly.
 */

#define BFS_MAGIC   0x42465343u       /* "CFSB" little-endian              */
#define BFS_VERSION 1u

#pragma pack(push, 1)

struct bfs_header {
  u32 magic;
  u16 version;
  u16 header_size;                   /* sizeof(struct bfs_header)          */
  u32 input_len;
  u32 eff_len;
  u32 base_cksum;
  u8  dumb_mode;
  u8  stage;
  u8  status;
  u8  eff_densified;
  u32 pos;
  u32 stage_max;
  u32 eff_cnt;
  u32 a_len;
  u32 prev_cksum;
  u32 token_cnt;
  u32 token_bytes;
  u64 stage_execs[BFS_STAGES];
  u64 total_execs;
  u64 input_hash;                   /* FNV-1a of the input                */
  u64 options_hash;                 /* FNV-1a of identity-affecting opts  */
};

struct bfs_token_hdr {
  u32 len;
  u32 offset;                       /* offset into the token byte region  */
};

#pragma pack(pop)

/* CRC32 (IEEE 802.3), table-driven, no external dependencies. */

static u32 bfs_crc_table[256];
static u8 bfs_crc_ready;

static void bfs_crc_init(void) {

  u32 i;

  for (i = 0; i < 256; i++) {
    u32 c = i;
    int k;
    for (k = 0; k < 8; k++)
      c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    bfs_crc_table[i] = c;
  }

  bfs_crc_ready = 1;

}

static u32 bfs_crc32(const u8 *buf, size_t len) {

  u32 c = 0xFFFFFFFFu;
  size_t i;

  if (!bfs_crc_ready) bfs_crc_init();

  for (i = 0; i < len; i++)
    c = bfs_crc_table[(c ^ buf[i]) & 0xFF] ^ (c >> 8);

  return c ^ 0xFFFFFFFFu;

}

static u64 bfs_fnv_init(void) { return 1469598103934665603ULL; }

static u64 bfs_fnv_u64(u64 h, u64 v) {

  int k;

  for (k = 0; k < 8; k++) {
    h ^= (u8)(v & 0xFF);
    h *= 1099511628211ULL;
    v >>= 8;
  }

  return h;

}

static u64 bfs_fnv_bytes(u64 h, const u8 *buf, size_t len) {

  size_t i;

  for (i = 0; i < len; i++) {
    h ^= buf[i];
    h *= 1099511628211ULL;
  }

  return h;

}

static u64 bfs_input_hash(const u8 *input, u32 len)
  { return bfs_fnv_bytes(bfs_fnv_init(), input, len); }

static u64 bfs_options_hash(u32 len, u32 base_cksum, u8 dumb_mode) {

  u64 h = bfs_fnv_init();

  h = bfs_fnv_u64(h, len);
  h = bfs_fnv_u64(h, base_cksum);
  h = bfs_fnv_bytes(h, &dumb_mode, 1);
  h = bfs_fnv_u64(h, BFS_VERSION);
  return h;

}

static int bfs_write_all(int fd, const u8 *buf, size_t len) {

  while (len) {
    ssize_t n = write(fd, buf, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (!n) return -1;
    buf += n;
    len -= (size_t)n;
  }

  return 0;

}

int bitflip_session_save(const struct bitflip_session *s, const char *path) {

  int fd = -1;
  int ret = -1;
  struct bfs_header hdr;
  u32 eff_len, token_bytes = 0, i;
  u8 *blob = NULL, *tp;
  u32 crc;
  char *tmp_path;

  if (!s || !path) return -1;

  eff_len = BFS_EFF_ALEN(s->input_len);

  for (i = 0; i < s->token_cnt; i++) token_bytes += s->tokens[i].len;

  memset(&hdr, 0, sizeof(hdr));
  hdr.magic          = BFS_MAGIC;
  hdr.version        = BFS_VERSION;
  hdr.header_size    = sizeof(hdr);
  hdr.input_len      = s->input_len;
  hdr.eff_len        = eff_len;
  hdr.base_cksum     = s->base_cksum;
  hdr.dumb_mode      = s->dumb_mode;
  hdr.stage          = s->stage;
  hdr.status         = s->status;
  hdr.eff_densified  = s->eff_densified;
  hdr.pos            = s->pos;
  hdr.stage_max      = s->stage_max;
  hdr.eff_cnt        = s->eff_cnt;
  hdr.a_len          = s->a_len;
  hdr.prev_cksum     = s->prev_cksum;
  hdr.token_cnt      = s->token_cnt;
  hdr.token_bytes    = token_bytes;

  for (i = 0; i < BFS_STAGES; i++) hdr.stage_execs[i] = s->stage_execs[i];
  hdr.total_execs = s->total_execs;
  hdr.input_hash   = bfs_input_hash(s->input, s->input_len);
  hdr.options_hash = bfs_options_hash(s->input_len, s->base_cksum,
                                      s->dumb_mode);

  size_t table_bytes = (size_t)s->token_cnt * sizeof(struct bfs_token_hdr);
  size_t total = sizeof(hdr) + 2 * (size_t)s->input_len + eff_len +
                 MAX_AUTO_EXTRA + table_bytes + token_bytes + sizeof(u32);

  if (total < sizeof(hdr) || total > MAX_FILE * 4) return -1;

  blob = ck_alloc((u32)total);
  tp = blob;

  memcpy(tp, &hdr, sizeof(hdr));
  tp += sizeof(hdr);

  memcpy(tp, s->input, s->input_len);
  tp += s->input_len;

  /* At every suspension point work equals input; persist it anyway so the
     invariant is verified on load rather than assumed. */

  memcpy(tp, s->work, s->input_len);
  tp += s->input_len;

  memcpy(tp, s->eff_map, eff_len);
  tp += eff_len;

  memcpy(tp, s->a_collect, MAX_AUTO_EXTRA);
  tp += MAX_AUTO_EXTRA;

  {
    u32 off = 0;
    for (i = 0; i < s->token_cnt; i++) {

      struct bfs_token_hdr th;

      th.len = s->tokens[i].len;
      th.offset = off;
      memcpy(tp, &th, sizeof(th));
      tp += sizeof(th);
      off += th.len;

    }
  }

  for (i = 0; i < s->token_cnt; i++) {
    memcpy(tp, s->tokens[i].data, s->tokens[i].len);
    tp += s->tokens[i].len;
  }

  crc = bfs_crc32(blob, total - sizeof(u32));
  memcpy(tp, &crc, sizeof(crc));

  /* Write to a sibling temp file and rename, so a failed save can never
     leave a truncated checkpoint behind. */

  tmp_path = alloc_printf("%s.tmp.%d", path, (int)getpid());

  fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) goto out;

  if (bfs_write_all(fd, blob, total)) goto out;
  if (fsync(fd)) goto out;
  close(fd);
  fd = -1;

  if (rename(tmp_path, path)) goto out;

  ret = 0;

out:
  if (fd >= 0) close(fd);
  if (tmp_path) {
    unlink(tmp_path);
    ck_free(tmp_path);
  }
  ck_free(blob);
  return ret;

}

static int bfs_read_all(int fd, u8 *buf, size_t len) {

  while (len) {
    ssize_t n = read(fd, buf, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (!n) return -1;                            /* unexpected EOF         */
    buf += n;
    len -= (size_t)n;
  }

  return 0;

}

struct bitflip_session *bitflip_session_load(const char *path,
                                             const u8 *input, u32 len,
                                             u32 base_cksum, u8 dumb_mode,
                                             bitflip_exec_cb exec_cb,
                                             bitflip_token_cb token_cb,
                                             bitflip_event_cb event_cb,
                                             void *userdata) {

  int fd = -1;
  struct bitflip_session *s = NULL;
  u8 *blob = NULL;
  struct bfs_header hdr;
  u32 eff_len, crc, stored_crc, i, token_off;
  const u8 *p_input, *p_work, *p_eff, *p_collect, *p_table, *p_tokens;
  size_t table_bytes, expected;
  u64 total_execs;
  u64 stage_execs[BFS_STAGES];

  if (!path || !input || !len || !exec_cb) return NULL;

  fd = open(path, O_RDONLY);
  if (fd < 0) return NULL;

  if (bfs_read_all(fd, (u8 *)&hdr, sizeof(hdr))) goto out;

  /* Fixed-format checks first: anything off means this is not our file. */

  if (hdr.magic != BFS_MAGIC || hdr.version != BFS_VERSION ||
      hdr.header_size != sizeof(struct bfs_header))
    goto out;

  if (!hdr.input_len || hdr.input_len != len ||
      hdr.input_len > MAX_FILE || hdr.eff_len != BFS_EFF_ALEN(len))
    goto out;

  if (hdr.stage >= BFS_STAGES ||
      (hdr.status != BFS_PAUSED && hdr.status != BFS_COMPLETE) ||
      hdr.pos > bfs_nominal_max(hdr.stage, len))
    goto out;

  if (hdr.status == BFS_COMPLETE && hdr.stage != BFS_FLIP32)
    goto out;

  /* stage_max and the effector counters are fully determined by input. */

  if (hdr.stage_max != bfs_nominal_max(hdr.stage, len) ||
      hdr.eff_cnt == 0 || hdr.eff_cnt > hdr.eff_len)
    goto out;

  {
    u64 sum_execs = 0;
    for (i = 0; i < BFS_STAGES; i++) sum_execs += hdr.stage_execs[i];
    if (sum_execs != hdr.total_execs) goto out;
  }

  /* a_len can transiently reach MAX_AUTO_EXTRA + 1 (the final increment is
     not guarded in the original loop); the stored buffer stays 32 bytes.
     Token proposals are not bounded by MAX_AUTO_EXTRAS: that cap belongs to
     maybe_add_auto() on the consumer side, not to the proposal stream. */

  if (hdr.a_len > MAX_AUTO_EXTRA + 1)
    goto out;

  eff_len = hdr.eff_len;
  table_bytes = (size_t)hdr.token_cnt * sizeof(struct bfs_token_hdr);
  expected = sizeof(hdr) + 2 * (size_t)len + eff_len + MAX_AUTO_EXTRA +
             table_bytes + hdr.token_bytes + sizeof(u32);

  blob = ck_alloc((u32)expected);
  memcpy(blob, &hdr, sizeof(hdr));

  if (bfs_read_all(fd, blob + sizeof(hdr), expected - sizeof(hdr)))
    goto out;

  /* Refuse trailing garbage as well. */

  {
    u8 scratch;
    ssize_t n = read(fd, &scratch, 1);
    if (n != 0) goto out;
  }

  close(fd);
  fd = -1;

  /* Integrity over the entire image except the trailing CRC word. */

  memcpy(&stored_crc, blob + expected - sizeof(u32), sizeof(u32));
  crc = bfs_crc32(blob, expected - sizeof(u32));
  if (crc != stored_crc) goto out;

  /* Identity: caller-supplied bytes, checksum and option flags. */

  p_input    = blob + sizeof(hdr);
  p_work     = p_input + len;
  p_eff      = p_work + len;
  p_collect  = p_eff + eff_len;
  p_table    = p_collect + MAX_AUTO_EXTRA;
  p_tokens   = p_table + table_bytes;

  if (hdr.base_cksum != base_cksum || hdr.dumb_mode != dumb_mode)
    goto out;

  if (bfs_input_hash(input, len) != hdr.input_hash ||
      bfs_options_hash(len, base_cksum, dumb_mode) != hdr.options_hash)
    goto out;

  if (memcmp(p_input, input, len)) goto out;

  /* The work buffer must be at rest: every candidate flips back before the
     session yields, so work equals input at every checkpoint boundary. */

  if (memcmp(p_work, input, len)) goto out;

  /* Structural validation of the token table. */

  token_off = 0;
  for (i = 0; i < hdr.token_cnt; i++) {

    const struct bfs_token_hdr *th =
      (const struct bfs_token_hdr *)(p_table +
                                     i * sizeof(struct bfs_token_hdr));

    if (th->len < MIN_AUTO_EXTRA || th->len > MAX_AUTO_EXTRA ||
        th->offset != token_off ||
        (u64)th->offset + th->len > hdr.token_bytes)
      goto out;

    token_off += th->len;

  }

  if (token_off != hdr.token_bytes) goto out;

  /* All checks passed; only now do we construct the session. The silent
     core avoids re-emitting a flip1 stage-enter event for a resumed run. */

  s = bfs_alloc_core(input, len, base_cksum, dumb_mode, exec_cb, token_cb,
                     event_cb, userdata);
  if (!s) goto out;

  memcpy(s->eff_map, p_eff, eff_len);
  s->eff_cnt       = hdr.eff_cnt;
  s->eff_densified = hdr.eff_densified;
  s->stage         = hdr.stage;
  s->pos           = hdr.pos;
  s->stage_max     = hdr.stage_max;
  s->a_len         = hdr.a_len;
  s->prev_cksum    = hdr.prev_cksum;
  s->status        = hdr.status;

  memcpy(s->stage_execs, hdr.stage_execs, sizeof(stage_execs));
  total_execs = hdr.total_execs;
  s->total_execs = total_execs;

  /* Restore the in-progress flip1 collection (at most 32 stored bytes even
     when a_len transiently reads MAX_AUTO_EXTRA + 1 at end of file). */

  memcpy(s->a_collect, p_collect, MAX_AUTO_EXTRA);

  /* Rebuild the token proposals. */

  for (i = 0; i < hdr.token_cnt; i++) {

    const struct bfs_token_hdr *th =
      (const struct bfs_token_hdr *)(p_table +
                                     i * sizeof(struct bfs_token_hdr));

    bfs_store_token(s, p_tokens + th->offset, th->len);

  }

  /* A completed session has already handed its tokens out; do not replay. */

  if (s->status == BFS_COMPLETE) {

    s->tokens_delivered = 1;

  } else {

    /* Re-announce the stage the checkpoint was paused in, so a consumer
       process gets the right stage name/max before the first submission. */

    bfs_event(s, BFS_EV_STAGE_ENTER, s->stage_max, 0);

  }

  ck_free(blob);
  return s;

out:
  if (fd >= 0) close(fd);
  ck_free(blob);
  if (s) bitflip_session_close(s);
  return NULL;

}
