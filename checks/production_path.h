/* Regression helpers for the resumable bitflip prelude. Apache-2.0. */
#ifndef CHECKS_PRODUCTION_PATH_H
#define CHECKS_PRODUCTION_PATH_H

/* Drives the real fuzz_one()/fuzz_one_budget() built into afl-fuzz.c through
   the AFL_TASK_PROBE hooks and asserts (via TASK_CHECK) that the production
   code runs the six bitflip stages through a bitflip_session. Returns 0 on
   success, 1 on the first failed check. */

int production_path_run(void);

/* The infinite-budget fuzz_one() wrapper must finish the whole prelude in a
   single call and reach the deterministic boundary hook. Returns 0/1. */

int production_full_run(void);

#endif
