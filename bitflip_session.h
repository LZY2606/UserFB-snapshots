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

   A bitflip_session owns everything the six deterministic bitflip stages
   (flip1, flip2, flip4, flip8, flip16, flip32) need to run: the original
   input, the mutable work buffer, the in-progress auto-dictionary token
   collection, the effector map and the exact iteration position. Sessions
   are advanced with an explicit, non-negative execution budget and can be
   checkpointed to disk and restored in a different process.

   The implementation never touches any fuzzer-global state: every external
   observation is delivered through the callbacks handed to the open/load
   functions, so two sessions can be driven interleaved without sharing work
   buffers or pending tokens.
*/

#ifndef _HAVE_BITFLIP_SESSION_H
#define _HAVE_BITFLIP_SESSION_H

#include "types.h"
#include "config.h"

/* Outcome of bitflip_session_run(). */

#define BFS_COMPLETE 0                    /* all six stages finished        */
#define BFS_ERROR    1                    /* execution callback failed      */
#define BFS_PAUSED   2                    /* budget exhausted, resumable    */

/* Stage ids, in the original fuzz_one() execution order. */

enum {

  BFS_FLIP1 = 0,
  BFS_FLIP2,
  BFS_FLIP4,
  BFS_FLIP8,
  BFS_FLIP16,
  BFS_FLIP32,
  BFS_STAGES

};

/*
 * Execution callback. Invoked exactly once per candidate actually submitted
 * to the target; skipped effector regions never trigger it.
 *
 *   stage      - BFS_FLIP* id of the producing stage
 *   byte_pos   - original byte position the mutation starts at
 *   cand/len   - candidate bytes; the pointer is only valid for the call
 *   cksum_out  - on success, the coverage checksum of the observed trace
 *
 * Return 0 on success. A non-zero return fails the candidate: it is not
 * billed, not recorded as done, and a later run() retries it.
 */

typedef u8 (*bitflip_exec_cb)(int stage, u32 byte_pos, const u8 *cand,
                              u32 len, u32 *cksum_out, void *userdata);

/*
 * Token delivery callback. Pending auto-dictionary proposals are replayed in
 * their original proposal order, once, when the prelude completes. Never
 * invoked from open/load/save.
 */

typedef void (*bitflip_token_cb)(const u8 *token, u32 len, void *userdata);

/* Optional lifecycle events, used by the fuzzer to feed its own statistics. */

#define BFS_EV_STAGE_ENTER 0              /* a = nominal stage max          */
#define BFS_EV_STAGE_EXIT  1              /* a = candidates actually run    */
#define BFS_EV_EFF_STATS   2              /* a = total blocks, b = selected */

typedef void (*bitflip_event_cb)(int event, int stage, u64 a, u64 b,
                                 void *userdata);

struct bitflip_session;

/* Allocate a fresh session. The input is copied and owned by the session.
   Returns NULL on bad arguments or allocation failure. */

struct bitflip_session *bitflip_session_open(const u8 *input, u32 len,
                                             u32 base_cksum, u8 dumb_mode,
                                             bitflip_exec_cb exec_cb,
                                             bitflip_token_cb token_cb,
                                             bitflip_event_cb event_cb,
                                             void *userdata);

void bitflip_session_close(struct bitflip_session *s);

/* Drive the session forward. budget counts only candidates handed to the
   execution callback; skipped regions and a zero budget cost nothing.
   Returns BFS_COMPLETE / BFS_PAUSED / BFS_ERROR. */

int bitflip_session_run(struct bitflip_session *s, u64 budget);

/* Observation accessors. */

int  bitflip_session_status(const struct bitflip_session *s);
int  bitflip_session_stage(const struct bitflip_session *s);
u32  bitflip_session_len(const struct bitflip_session *s);
u64  bitflip_session_execs(const struct bitflip_session *s);
u64  bitflip_session_stage_execs(const struct bitflip_session *s, int stage);
u64  bitflip_session_budget_left(const struct bitflip_session *s);

/* Execs performed within the currently active stage, and its nominal max
   (flip16/flip32 have skipped positions subtracted). */

u32  bitflip_session_stage_index(const struct bitflip_session *s);
u32  bitflip_session_stage_max(const struct bitflip_session *s);

/* The immutable input owned by the session. */

const u8 *bitflip_session_input(const struct bitflip_session *s);

/* Borrow the effector map (valid for the session's lifetime). Only meaningful
   once flip8 has run, but the backing store exists from open(). */

const u8 *bitflip_session_eff_map(const struct bitflip_session *s,
                                  u32 *eff_len);

/*
 * Checkpoint the session to a self-contained file. The image contains no
 * pointers and no replay log: only the input, iteration state, effector map
 * and not-yet-delivered token proposals. Returns 0 on success, -1 on I/O
 * failure.
 */

int bitflip_session_save(const struct bitflip_session *s, const char *path);

/*
 * Restore a checkpoint. The caller must prove identity by passing the exact
 * input bytes, base coverage checksum and dumb_mode flag the session was
 * created with. Any version mismatch, truncation, corruption or identity
 * mismatch fails the load: NULL is returned, no callback fires and no token
 * is submitted. The previously existing session (if any) is untouched.
 */

struct bitflip_session *bitflip_session_load(const char *path,
                                             const u8 *input, u32 len,
                                             u32 base_cksum, u8 dumb_mode,
                                             bitflip_exec_cb exec_cb,
                                             bitflip_token_cb token_cb,
                                             bitflip_event_cb event_cb,
                                             void *userdata);

#endif /* ! _HAVE_BITFLIP_SESSION_H */
